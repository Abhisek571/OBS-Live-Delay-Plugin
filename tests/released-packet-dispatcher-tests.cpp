#include "active-delay-session.hpp"
#include "network-packet-consumer.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

EncodedPacket video_packet(std::uint8_t value)
{
	return {PacketKind::Video, {0, 0, 0, 2, 0x65, value}, 1'000'000, 1'000'000, true};
}

class RecordingConsumer final : public ReleasedPacketConsumer {
public:
	void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) override { batches.push_back(batch); }
	void discontinuity(const PacketDiscontinuity &event) override { discontinuities.push_back(event); }
	void stop() noexcept override { ++stops; }

	std::vector<std::shared_ptr<const ReleasedPacketBatch>> batches;
	std::vector<PacketDiscontinuity> discontinuities;
	std::size_t stops = 0;
};

class ThrowingConsumer final : public ReleasedPacketConsumer {
public:
	void consume(const std::shared_ptr<const ReleasedPacketBatch> &) override { throw std::runtime_error("injected consumer failure"); }
	void discontinuity(const PacketDiscontinuity &) override {}
	void stop() noexcept override {}
};

struct FakeConnectionState {
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::vector<std::uint8_t>> writes;
};

class FakeConnection final : public IRtmpConnection {
public:
	explicit FakeConnection(std::shared_ptr<FakeConnectionState> state) : state_(std::move(state)) {}
	bool connect(const RtmpTarget &, std::string &) override { return true; }
	bool send(std::span<const std::uint8_t> bytes, std::string &) override
	{
		std::scoped_lock lock(state_->mutex);
		state_->writes.emplace_back(bytes.begin(), bytes.end());
		state_->changed.notify_all();
		return true;
	}
	void interrupt() noexcept override {}
	void close() noexcept override {}

private:
	std::shared_ptr<FakeConnectionState> state_;
};

void dispatcher_shares_one_immutable_batch_and_keeps_other_consumers_running()
{
	ReleasedPacketDispatcher dispatcher;
	auto first = std::make_shared<RecordingConsumer>();
	auto second = std::make_shared<RecordingConsumer>();
	dispatcher.add_consumer(first);
	dispatcher.add_consumer(std::make_shared<ThrowingConsumer>());
	dispatcher.add_consumer(second);
	auto batch = std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{7, {video_packet(0x44)}});
	const auto failures = dispatcher.dispatch(batch);
	require(failures.size() == 1 && failures.front().error == "injected consumer failure",
		"one failed consumer should be reported without stopping other consumers");
	require(first->batches.size() == 1 && second->batches.size() == 1, "healthy consumers should receive one batch in order");
	require(first->batches.front() == batch && second->batches.front() == batch,
		"all destinations must observe the same immutable batch ownership");
	require(&first->batches.front()->packets.front().payload == &second->batches.front()->packets.front().payload,
		"dispatcher must not copy packet payloads per consumer");

	const auto discontinuity_failures = dispatcher.dispatch_discontinuity({8, "encoder restarted"});
	require(discontinuity_failures.empty(), "healthy consumers should accept a discontinuity");
	require(first->discontinuities.size() == 1 && first->discontinuities.front().epoch == 8,
		"epoch discontinuity should be delivered explicitly");
	dispatcher.stop_all();
	require(first->stops == 1 && second->stops == 1, "dispatcher shutdown should stop every consumer once");
}

void session_modes_are_explicit_and_mutually_exclusive()
{
	ActiveDelaySession session;
	std::string error;
	require(session.operating_mode() == OperatingMode::DirectSingle, "direct single should remain the default mode");
	require(session.begin_consumer_lifecycle(OperatingMode::DirectSingle, error), "direct lifecycle should begin");
	require(!session.set_operating_mode(OperatingMode::NativeMultistream, error),
		"mode changes must be rejected while a consumer is active");
	session.end_consumer_lifecycle();
	require(session.set_operating_mode(OperatingMode::NativeMultistream, error), "stopped session should accept a new mode");
	require(!session.begin_consumer_lifecycle(OperatingMode::DirectSingle, error),
		"a Direct Single output must not start while Native Multistream is selected");
	require(session.begin_consumer_lifecycle(OperatingMode::NativeMultistream, error), "selected mode should begin exclusively");
}

void network_consumer_preserves_deterministic_direct_flv_bytes()
{
	auto state = std::make_shared<FakeConnectionState>();
	NetworkPacketConsumer consumer([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 0, 1ms, 1s});
	std::string error;
	FlvCodecHeaders headers{{0x01, 0x64, 0x00, 0x1f}, {0x12, 0x10}};
	require(consumer.start({"rtmp://fake/live", "test"}, headers, {}, error), "network consumer should start with fake RTMP");
	auto batch = std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {video_packet(0x55)}});
	consumer.consume(batch);
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->writes.size() >= 4; }),
			"network consumer should write FLV header, codec headers, and media");
	}
	auto muxer = FlvMuxer(headers);
	std::vector<FlvTag> expected_tags;
	require(muxer.mux(batch->packets, expected_tags, error), "fixture should mux independently");
	{
		std::scoped_lock lock(state->mutex);
		require(state->writes[3] == serialize_flv_tag(expected_tags.front()),
			"dispatcher consumer must retain byte-equivalent Direct Single FLV output");
	}
	consumer.stop();
}
} // namespace

int main()
{
	try {
		dispatcher_shares_one_immutable_batch_and_keeps_other_consumers_running();
		session_modes_are_explicit_and_mutually_exclusive();
		network_consumer_preserves_deterministic_direct_flv_bytes();
		std::cout << "Released packet pipeline tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "Released packet pipeline test failure: " << error.what() << '\n';
		return 1;
	}
}
