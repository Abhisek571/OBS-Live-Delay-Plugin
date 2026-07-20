#include "flv-muxer.hpp"
#include "multi-target-sender.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
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

FlvCodecHeaders codec_headers()
{
	return {{0x01}, {0x12, 0x10}};
}

EncodedPacket keyframe(std::uint8_t value, std::int64_t timestamp = 1000)
{
	EncodedPacket packet;
	packet.kind = PacketKind::Video;
	packet.keyframe = true;
	packet.dts_us = timestamp;
	packet.pts_us = timestamp;
	packet.payload = {0x00, 0x00, 0x00, 0x02, 0x65, value};
	return packet;
}

struct FakeState {
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::vector<std::uint8_t>> writes;
	bool fail_connect = false;
	bool delay_media = false;
	std::size_t sends = 0;
};

class FakeConnection final : public IRtmpConnection {
public:
	explicit FakeConnection(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

	bool connect(const RtmpTarget &, std::string &error) override
	{
		std::scoped_lock lock(state_->mutex);
		if (!state_->fail_connect)
			return true;
		error = "injected connection failure";
		return false;
	}

	bool send(std::span<const std::uint8_t> bytes, std::string &) override
	{
		bool delay = false;
		{
			std::scoped_lock lock(state_->mutex);
			++state_->sends;
			delay = state_->delay_media && state_->sends > 3;
			state_->changed.notify_all();
		}
		if (delay)
			std::this_thread::sleep_for(400ms);
		{
			std::scoped_lock lock(state_->mutex);
			state_->writes.emplace_back(bytes.begin(), bytes.end());
			state_->changed.notify_all();
		}
		return true;
	}

	void interrupt() noexcept override {}
	void close() noexcept override {}

private:
	std::shared_ptr<FakeState> state_;
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count, std::chrono::milliseconds timeout = 2s)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, timeout, [&] { return state->writes.size() >= count; });
}

bool wait_for_sends(const std::shared_ptr<FakeState> &state, std::size_t count)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, 2s, [&] { return state->sends >= count; });
}

MultistreamConfiguration one_secondary()
{
	MultistreamConfiguration configuration;
	configuration.secondary_destinations.push_back({"secondary_1", "Secondary test", {"rtmp://secondary.test/live", "secondary-key"}});
	return configuration;
}

void destinations_receive_identical_flv_order()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] {
		return std::make_unique<FakeConnection>(factory_index++ == 0 ? primary : secondary);
	}, {{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "primary-key"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"primary plus secondary sender should start");
	auto batch = std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x42)}});
	sender.consume(batch);
	require(wait_for_writes(primary, 4), "primary should receive headers and media");
	require(wait_for_writes(secondary, 4), "secondary should receive headers and media");
	sender.stop();
	std::scoped_lock primary_lock(primary->mutex);
	std::scoped_lock secondary_lock(secondary->mutex);
	require(primary->writes == secondary->writes, "both destinations must receive identical FLV bytes in order");
}

void secondary_start_failure_is_isolated()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	secondary->fail_connect = true;
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] {
		return std::make_unique<FakeConnection>(factory_index++ == 0 ? primary : secondary);
	}, {{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "primary-key"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"a failed secondary must not reject a healthy primary");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x43)}}));
	require(wait_for_writes(primary, 4), "healthy primary must continue after secondary startup failure");
	const auto status = sender.status();
	require(status.aggregate_state == SenderState::Running, "aggregate state follows primary health");
	require(status.destinations.size() == 2 && status.destinations[1].sender.state == SenderState::Failed,
		"secondary failure should be retained per target");
	sender.stop();
}

void slow_secondary_does_not_stall_primary()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] {
		return std::make_unique<FakeConnection>(factory_index++ == 0 ? primary : secondary);
	}, {{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "primary-key"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"fan-out sender should start");
	secondary->delay_media = true;
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x44)}}));
	require(wait_for_writes(primary, 4, 150ms), "slow secondary must not delay primary media delivery");
	sender.stop();
}

void secondary_backpressure_is_isolated_and_shutdown_is_clean()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] {
		return std::make_unique<FakeConnection>(factory_index++ == 0 ? primary : secondary);
	}, {{1, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "primary-key"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"fan-out sender should start with bounded queues");
	secondary->delay_media = true;
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x50, 1000)}}));
	require(wait_for_writes(primary, 4), "primary should deliver the first packet");
	require(wait_for_sends(secondary, 4), "secondary should be occupied sending its first packet");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x51, 2000)}}));
	require(wait_for_writes(primary, 5), "primary should deliver while the secondary is slow");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x52, 3000)}}));
	require(wait_for_writes(primary, 6, 150ms), "secondary queue saturation must not stall primary delivery");
	const auto status = sender.status();
	require(status.destinations[1].sender.state == SenderState::Failed,
		"secondary queue saturation must be visible only on that target");
	auto stop_start = std::chrono::steady_clock::now();
	sender.stop();
	require(std::chrono::steady_clock::now() - stop_start < 2s, "fan-out shutdown should join workers cleanly");
}

void validation_and_labels_never_expose_secrets()
{
	auto configuration = one_secondary();
	configuration.secondary_destinations[0].target.username = "private-user";
	configuration.secondary_destinations[0].target.password = "private-password";
	configuration.secondary_destinations[0].target.stream_key = "private-stream-key";
	configuration.secondary_destinations[0].target.server_url.clear();
	std::string error;
	require(!validate_multistream_configuration(configuration, error), "bad target should fail validation");
	require(error.find("private-user") == std::string::npos && error.find("private-password") == std::string::npos &&
			error.find("private-stream-key") == std::string::npos, "validation errors must redact credentials");
	const auto label = safe_destination_label(configuration.secondary_destinations[0]);
	require(label.find("private") == std::string::npos, "safe status label must not include target credentials");
}
} // namespace

int main()
{
	try {
		destinations_receive_identical_flv_order();
		secondary_start_failure_is_isolated();
		slow_secondary_does_not_stall_primary();
		secondary_backpressure_is_isolated_and_shutdown_is_clean();
		validation_and_labels_never_expose_secrets();
		std::cout << "multi-target sender tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "multi-target sender test failure: " << error.what() << '\n';
		return 1;
	}
}
