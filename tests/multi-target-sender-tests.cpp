#include "flv-muxer.hpp"
#include "multi-target-sender.hpp"
#include "multistream-preflight.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
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

EncodedPacket keyframe(std::uint8_t value, std::int64_t timestamp);

EncodedPacket keyframe(std::uint8_t value)
{
	return keyframe(value, 1000);
}

EncodedPacket keyframe(std::uint8_t value, std::int64_t timestamp)
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
	std::size_t closes = 0;
	std::size_t interrupts = 0;
	std::size_t connects = 0;
	std::size_t fail_on_send = 0;
	std::size_t stall_on_send = 0;
	bool block_reconnect = false;
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count, std::chrono::milliseconds timeout);

class FakeConnection final : public IRtmpConnection {
public:
	explicit FakeConnection(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

	bool connect(const RtmpTarget &, std::string &error) override
	{
		std::unique_lock lock(state_->mutex);
		++state_->connects;
		state_->changed.notify_all();
		if (state_->connects > 1 && state_->block_reconnect) {
			state_->changed.wait_for(lock, 2s, [&] { return !state_->block_reconnect || state_->interrupts > 0; });
			if (state_->interrupts > 0)
				return false;
		}
		if (!state_->fail_connect)
			return true;
		error = "injected connection failure";
		return false;
	}

	bool send(std::span<const std::uint8_t> bytes, std::string &error) override
	{
		bool delay = false;
		{
			std::unique_lock lock(state_->mutex);
			++state_->sends;
			if (state_->sends == state_->fail_on_send) {
				error = "injected media failure";
				state_->changed.notify_all();
				return false;
			}
			if (state_->sends == state_->stall_on_send) {
				// A congested uplink: the write hangs until the sender is cancelled.
				state_->changed.notify_all();
				state_->changed.wait_for(lock, 3s, [&] { return state_->interrupts > 0; });
				error = "stalled write cancelled";
				return false;
			}
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

	void interrupt() noexcept override
	{
		std::scoped_lock lock(state_->mutex);
		++state_->interrupts;
		state_->changed.notify_all();
	}
	void close() noexcept override
	{
		std::scoped_lock lock(state_->mutex);
		++state_->closes;
		state_->changed.notify_all();
	}

private:
	std::shared_ptr<FakeState> state_;
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count)
{
	return wait_for_writes(state, count, 2s);
}

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count, std::chrono::milliseconds timeout)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, timeout, [&] { return state->writes.size() >= count; });
}

bool wait_for_sends(const std::shared_ptr<FakeState> &state, std::size_t count)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, 2s, [&] { return state->sends >= count; });
}

bool wait_for_state(MultiTargetSender &sender, std::size_t destination, SenderState expected)
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	do {
		const auto status = sender.status();
		if (status.destinations.at(destination).sender.state == expected)
			return true;
		std::this_thread::sleep_for(1ms);
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

MultistreamConfiguration one_secondary()
{
	MultistreamConfiguration configuration;
	configuration.secondary_destinations.push_back({"secondary_1", "Secondary test", {"rtmp://secondary.test/live", "secondary-key"}});
	return configuration;
}

MultistreamConfiguration two_secondaries()
{
	MultistreamConfiguration configuration;
	configuration.secondary_destinations.push_back({"secondary_2", "YouTube test",
		{"rtmps://youtube.test/live", "youtube-key"}, DestinationPlatform::YouTube});
	configuration.secondary_destinations.push_back({"secondary_3", "Kick test",
		{"rtmps://kick.test/live", "kick-key"}, DestinationPlatform::Kick});
	return configuration;
}

struct CancellationGroup {
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t connected = 0;
	std::size_t interrupted = 0;
	std::size_t expected = 3;
};

class CoordinatedCancellationConnection final : public IRtmpConnection {
public:
	explicit CoordinatedCancellationConnection(std::shared_ptr<CancellationGroup> group) : group_(std::move(group)) {}
	bool connect(const RtmpTarget &, std::string &) override
	{
		std::unique_lock lock(group_->mutex);
		++group_->connected;
		group_->changed.notify_all();
		group_->changed.wait_for(lock, 2s, [&] { return interrupted_; });
		// All destinations must receive cancellation before shutdown joins one.
		group_->changed.wait_for(lock, 500ms, [&] { return group_->interrupted == group_->expected; });
		return false;
	}
	bool send(std::span<const std::uint8_t>, std::string &) override { return false; }
	void interrupt() noexcept override
	{
		std::scoped_lock lock(group_->mutex);
		if (!interrupted_) {
			interrupted_ = true;
			++group_->interrupted;
		}
		group_->changed.notify_all();
	}
	void close() noexcept override {}
private:
	std::shared_ptr<CancellationGroup> group_;
	bool interrupted_ = false;
};

void stalled_destinations_start_asynchronously_and_cancel_together()
{
	auto group = std::make_shared<CancellationGroup>();
	MultiTargetSender sender([group] { return std::make_unique<CoordinatedCancellationConnection>(group); },
		{{32, 8192}, 0, 1ms, 1s});
	std::string error;
	const auto started = std::chrono::steady_clock::now();
	require(sender.start({"rtmp://primary.test/live", "synthetic-key"}, "Primary", two_secondaries(),
		codec_headers(), {}, error), "starting workers should not wait for connections");
	const auto startup_duration = std::chrono::steady_clock::now() - started;
	const auto aggregate = sender.status().aggregate_state;
	{
		std::unique_lock lock(group->mutex);
		require(group->changed.wait_for(lock, 1s, [&] { return group->connected == 3; }), "all three workers must enter connect");
	}
	const auto stopping = std::chrono::steady_clock::now();
	sender.stop();
	const auto stop_duration = std::chrono::steady_clock::now() - stopping;
	require(startup_duration < 100ms, "stalled connections must not delay startup");
	require(stop_duration < 400ms, "cancel all destinations before joining any worker");
	require(aggregate == SenderState::Starting, "pending connections are Starting, not Failed");
	std::cout << "three-target start ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(startup_duration).count()
		<< " stop ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(stop_duration).count() << '\n';
}

void destructor_cancels_all_destinations_before_join()
{
	auto group = std::make_shared<CancellationGroup>();
	auto sender = std::make_unique<MultiTargetSender>([group] { return std::make_unique<CoordinatedCancellationConnection>(group); },
		SenderConfig{{32, 8192}, 0, 1ms, 1s});
	std::string error;
	require(sender->start({"rtmp://primary.test/live", "synthetic"}, "Primary", two_secondaries(), codec_headers(), {}, error), "start must succeed");
	{
		std::unique_lock lock(group->mutex);
		require(group->changed.wait_for(lock, 1s, [&] { return group->connected == 3; }), "all workers must enter connect");
	}
	const auto begin = std::chrono::steady_clock::now();
	sender.reset();
	require(std::chrono::steady_clock::now() - begin < 400ms, "destructor must cancel all destinations before joining one");
}

void partial_start_failure_cancels_all_before_join()
{
	auto group = std::make_shared<CancellationGroup>();
	group->expected = 2;
	std::size_t created = 0;
	MultiTargetSender sender([&]() -> std::unique_ptr<IRtmpConnection> {
		if (created++ == 0)
			return {};
		return std::make_unique<CoordinatedCancellationConnection>(group);
	}, {{32, 8192}, 0, 1ms, 1s});
	std::string error;
	const auto begin = std::chrono::steady_clock::now();
	const bool accepted = sender.start({"rtmp://primary.test/live", "synthetic"}, "Primary", two_secondaries(), codec_headers(), {}, error);
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	require(!accepted && !error.empty(), "primary creation failure must reject start");
	require(elapsed < 400ms, "partial start cleanup must cancel every secondary before joining one");
	require(sender.status().destinations.empty(), "partial failure must retain no worker ownership");
}

void three_destinations_receive_identical_flv_order()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary_2 = std::make_shared<FakeState>();
	auto secondary_3 = std::make_shared<FakeState>();
	const std::vector states{primary, secondary_2, secondary_3};
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(states.at(factory_index++)); },
		{{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmps://primary.test/live", "primary-key"}, "Primary", two_secondaries(),
		codec_headers(), {}, error), "three-destination sender should start");
	for (std::size_t index = 0; index < states.size(); ++index)
		require(wait_for_state(sender, index, SenderState::Running), "each asynchronous destination must become ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x41)}}));
	require(wait_for_writes(primary, 4), "primary should receive headers and media");
	require(wait_for_writes(secondary_2, 4), "secondary 2 should receive headers and media");
	require(wait_for_writes(secondary_3, 4), "secondary 3 should receive headers and media");
	sender.stop();
	std::scoped_lock lock(primary->mutex, secondary_2->mutex, secondary_3->mutex);
	require(primary->writes == secondary_2->writes && primary->writes == secondary_3->writes,
		"all three destinations must receive identical FLV bytes in order");
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
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 1, SenderState::Running),
		"both asynchronous destinations must become ready");
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
	require(wait_for_state(sender, 0, SenderState::Running), "healthy primary must become ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x43)}}));
	require(wait_for_writes(primary, 4), "healthy primary must continue after secondary startup failure");
	require(wait_for_state(sender, 1, SenderState::Failed), "secondary must report asynchronous startup failure");
	const auto status = sender.status();
	require(status.aggregate_state == SenderState::Running, "aggregate state follows primary health");
	require(status.destinations.size() == 2 && status.destinations[1].sender.state == SenderState::Failed,
		"secondary failure should be retained per target");
	sender.stop();
}

void one_secondary_failure_does_not_stop_the_other_secondary()
{
	auto primary = std::make_shared<FakeState>();
	auto failed = std::make_shared<FakeState>();
	auto healthy = std::make_shared<FakeState>();
	failed->fail_connect = true;
	const std::vector states{primary, failed, healthy};
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(states.at(factory_index++)); },
		{{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmps://primary.test/live", "primary-key"}, "Primary", two_secondaries(),
		codec_headers(), {}, error), "one failed secondary must not reject the other destinations");
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 2, SenderState::Running),
		"healthy destinations must become ready independently");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x46)}}));
	require(wait_for_writes(primary, 4), "primary must continue after one secondary fails");
	require(wait_for_writes(healthy, 4), "healthy secondary must continue after the other secondary fails");
	require(wait_for_state(sender, 1, SenderState::Failed), "failed secondary must settle independently");
	const auto status = sender.status();
	require(status.destinations.size() == 3, "three status rows must remain visible");
	require(status.destinations[1].sender.state == SenderState::Failed &&
		status.destinations[2].sender.state == SenderState::Running,
		"only the failed secondary should report failure");
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
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 1, SenderState::Running),
		"healthy destinations must become ready");
	{
		std::scoped_lock lock(secondary->mutex);
		secondary->delay_media = true;
	}
	const auto enqueue_started = std::chrono::steady_clock::now();
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x44)}}));
	const auto enqueue_duration = std::chrono::steady_clock::now() - enqueue_started;
	require(enqueue_duration < 100ms, "slow secondary must not stall enqueue");
	std::cout << "isolated enqueue us=" << std::chrono::duration_cast<std::chrono::microseconds>(enqueue_duration).count() << '\n';
	require(wait_for_writes(primary, 4, 150ms), "slow secondary must not delay primary media delivery");
	sender.stop();
}

void recovering_secondary_does_not_interrupt_healthy_destinations()
{
	auto primary = std::make_shared<FakeState>();
	auto recovering = std::make_shared<FakeState>();
	auto healthy = std::make_shared<FakeState>();
	recovering->fail_on_send = 4;
	recovering->block_reconnect = true;
	const std::vector states{primary, recovering, healthy};
	std::size_t created = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(states.at(created++)); }, {{8, 4096}, 2, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "synthetic"}, "Primary", two_secondaries(), codec_headers(), {}, error), "start must succeed");
	for (std::size_t index = 0; index < states.size(); ++index)
		require(wait_for_state(sender, index, SenderState::Running), "initial destinations must become ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x70, 1000)}}));
	{
		std::unique_lock lock(recovering->mutex);
		require(recovering->changed.wait_for(lock, 1s, [&] { return recovering->connects == 2; }), "failed destination must reach blocked reconnect");
	}
	require(sender.status().destinations[1].sender.state == SenderState::Reconnecting,
		"recovering secondary must report Reconnecting, not Failed");
	for (int index = 0; index < 12; ++index) {
		const auto begin = std::chrono::steady_clock::now();
		sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x71, 2000 + index * 1000)}}));
		require(std::chrono::steady_clock::now() - begin < 100ms, "reconnecting secondary must not stall enqueue");
		require(wait_for_writes(primary, 5 + index) && wait_for_writes(healthy, 5 + index), "healthy targets must continue while one reconnects");
	}
	require(sender.status().destinations[1].sender.queued_tags == 0, "reconnecting target must not accumulate stale media");
	{
		std::scoped_lock lock(recovering->mutex);
		recovering->block_reconnect = false;
		recovering->changed.notify_all();
	}
	require(wait_for_state(sender, 1, SenderState::Running), "secondary must recover independently");
	auto dependent = keyframe(0x72, 15000); dependent.keyframe = false;
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {dependent, keyframe(0x73, 16000)}}));
	require(wait_for_writes(recovering, 7), "recovery must prime headers and resume fresh keyframe");
	sender.stop();
	std::scoped_lock lock(primary->mutex, recovering->mutex, healthy->mutex);
	require(primary->connects == 1 && healthy->connects == 1 && recovering->connects == 2,
		"only recovering destination may reconnect");
	require(recovering->writes.size() == 7 && recovering->writes[3] == make_flv_header(),
		"recovery must resend FLV/codec headers without stale backlog");
	FlvMuxer reference(codec_headers());
	const auto codec_tags = reference.sequence_headers();
	std::vector<FlvTag> reference_media;
	require(reference.mux({keyframe(0x70, 1000), keyframe(0x73, 16000)}, reference_media, error), "reference media must mux");
	require(recovering->writes[4] == serialize_flv_tag(codec_tags[0]) && recovering->writes[5] == serialize_flv_tag(codec_tags[1]),
		"recovery must resend both exact codec headers before media");
	require(recovering->writes.back() == serialize_flv_tag(reference_media.back()),
		"recovery must choose the fresh video keyframe, not dependent or disconnected media");
}

void secondary_backpressure_is_isolated_and_shutdown_is_clean()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	auto other_secondary = std::make_shared<FakeState>();
	const std::vector states{primary, secondary, other_secondary};
	std::size_t factory_index = 0;
	MultiTargetSender sender([&] {
		return std::make_unique<FakeConnection>(states.at(factory_index++));
	}, {{1, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "primary-key"}, "Primary", two_secondaries(), codec_headers(), {}, error),
		"fan-out sender should start with bounded queues");
	for (std::size_t index = 0; index < states.size(); ++index)
		require(wait_for_state(sender, index, SenderState::Running), "bounded destinations must become ready");
	{
		std::scoped_lock lock(secondary->mutex);
		secondary->delay_media = true;
	}
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x50, 1000)}}));
	require(wait_for_writes(primary, 4), "primary should deliver the first packet");
	require(wait_for_writes(other_secondary, 4), "other secondary should deliver the first packet");
	require(wait_for_sends(secondary, 4), "secondary should be occupied sending its first packet");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x51, 2000)}}));
	require(wait_for_writes(primary, 5), "primary should deliver while the secondary is slow");
	require(wait_for_writes(other_secondary, 5), "other secondary should deliver while the secondary is slow");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x52, 3000)}}));
	require(wait_for_writes(primary, 6, 150ms), "secondary queue saturation must not stall primary delivery");
	require(wait_for_writes(other_secondary, 6, 150ms),
		"secondary queue saturation must not stall the other secondary");
	const auto status = sender.status();
	require(status.destinations[1].sender.state == SenderState::Failed,
		"secondary queue saturation must be visible only on that target");
	{
		std::scoped_lock lock(primary->mutex, secondary->mutex, other_secondary->mutex);
		require(secondary->interrupts > 0 && primary->interrupts == 0 && other_secondary->interrupts == 0,
			"isolating queue overflow must cancel only the failed secondary without joining it");
	}
	auto stop_start = std::chrono::steady_clock::now();
	sender.stop();
	require(std::chrono::steady_clock::now() - stop_start < 2s, "fan-out shutdown should join workers cleanly");
	std::scoped_lock lock(primary->mutex, secondary->mutex, other_secondary->mutex);
	require(primary->closes > 0 && secondary->closes > 0 && other_secondary->closes > 0,
		"two-secondary shutdown must close every target worker");
}

void shutdown_status_remains_stopping_until_every_worker_joins()
{
	auto primary = std::make_shared<FakeState>();
	auto secondary = std::make_shared<FakeState>();
	secondary->delay_media = true;
	std::size_t created = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(created++ == 0 ? primary : secondary); }, {{8, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "synthetic"}, "Primary", one_secondary(), codec_headers(), {}, error), "start must succeed");
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 1, SenderState::Running), "destinations must become ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x74)}}));
	require(wait_for_sends(secondary, 4), "secondary must enter stalled media write");
	auto teardown = std::async(std::launch::async, [&] { sender.stop(); });
	{
		std::unique_lock lock(secondary->mutex);
		require(secondary->changed.wait_for(lock, 1s, [&] { return secondary->interrupts > 0; }), "shutdown must cancel secondary");
	}
	const auto stopping = sender.status();
	teardown.get();
	require(stopping.aggregate_state == SenderState::Stopping && stopping.destinations.size() == 2,
		"shutdown must retain truthful destination rows and Stopping until all workers join");
	require(sender.status().aggregate_state == SenderState::Stopped && sender.status().destinations.empty(),
		"completed shutdown must release destination ownership");
}

void duplicate_destination_identity_is_rejected_without_exposing_secrets()
{
	auto configuration = two_secondaries();
	configuration.secondary_destinations[1].target = configuration.secondary_destinations[0].target;
	std::string error;
	require(!validate_multistream_configuration(configuration, error),
		"duplicate secondary publish identities must be rejected");
	require(error.find("youtube-key") == std::string::npos,
		"duplicate identity error must not reveal a stream key");

	configuration = two_secondaries();
	auto primary = configuration.secondary_destinations[0].target;
	primary.username = "transport-user";
	primary.password = "transport-password";
	require(!validate_multistream_configuration(configuration, primary, error),
		"a secondary must not duplicate the primary publish identity");
	require(error.find("youtube-key") == std::string::npos,
		"primary duplicate identity error must not reveal a stream key");
}

void version_one_migration_preserves_the_secret_in_slot_two()
{
	LegacyMultistreamSettingsV1 legacy;
	legacy.enabled = true;
	legacy.name = "Existing secondary";
	legacy.target = {"rtmps://legacy.test/live", "preserved-secret"};
	const auto migrated = migrate_multistream_v1(legacy);
	require(migrated.version == 2, "migration must produce storage version 2");
	require(migrated.secondary_destinations.size() == 1 &&
		migrated.secondary_destinations[0].id == "secondary_2",
		"version-1 secondary must migrate into destination slot 2");
	require(migrated.secondary_destinations[0].target.stream_key == "preserved-secret",
		"version-1 migration must preserve the locally stored secret");
	require(migrated.secondary_destinations[0].platform == DestinationPlatform::CustomRtmp,
		"legacy destination must migrate as Custom RTMP");
}

void preflight_counts_upload_and_enforces_known_platform_limits()
{
	const StreamRendition compatible{"h264", "aac", "CBR", 1920, 1080, 60.0, 8'000, 160, 2};
	const auto passing = evaluate_multistream_preflight(two_secondaries(), compatible);
	require(passing.enabled_destination_count == 3, "preflight must count primary plus two secondaries");
	require(passing.estimated_upload_kbps == 24'480, "preflight must estimate total shared-rendition upload");
	require(passing.can_start(), "known-compatible common rendition should pass preflight");

	auto incompatible = compatible;
	incompatible.width = 2560;
	incompatible.video_bitrate_kbps = 9'000;
	incompatible.keyframe_interval_seconds = 5;
	const auto failing = evaluate_multistream_preflight(two_secondaries(), incompatible);
	require(!failing.can_start(), "known Kick and YouTube hard incompatibilities must block start");
	require(failing.issues.size() >= 3, "preflight must list each known incompatibility");
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
	configuration.secondary_destinations[0].name = "private-stream-key";
	require(safe_destination_label(configuration.secondary_destinations[0]) == "Custom RTMP",
		"a display name containing the stream key must be replaced with a safe platform label");
	configuration.secondary_destinations[0].name = "rtmps://private.example/live";
	require(safe_destination_label(configuration.secondary_destinations[0]) == "Custom RTMP",
		"a display name containing a publish URL must be replaced with a safe platform label");
}

void stalled_secondary_guard_ack_is_isolated(){
 auto primary=std::make_shared<FakeState>(),secondary=std::make_shared<FakeState>();secondary->delay_media=true;
 std::size_t index=0;MultiTargetSender sender([&]{return std::make_unique<FakeConnection>(index++==0 ? primary : secondary);},{{64,65536},0,1ms,2s});
 std::string error;require(sender.start({"rtmp://primary.test/live","synthetic"},"Primary",one_secondary(),codec_headers(),{},error),"guard multistream start");
 require(wait_for_state(sender,0,SenderState::Running) && wait_for_state(sender,1,SenderState::Running),"both guard destinations ready");
 sender.discontinuity({2,"guard"});
 sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{2,{{PacketKind::Audio,{0},1000,1000,false,true}},codec_headers(),1}));
 const auto until=std::chrono::steady_clock::now()+1500ms;
 while(!sender.delivered(2,1) && std::chrono::steady_clock::now()<until){(void)sender.boundary_delivered(2);std::this_thread::sleep_for(1ms);}
 const bool ack=sender.delivered(2,1);const auto status=sender.status();sender.stop();
 require(ack && status.destinations[0].sender.state==SenderState::Running && status.destinations[1].sender.state==SenderState::Failed,
  "stalled secondary guard acknowledgement must isolate that destination, not stop or indefinitely gate primary");
}

EncodedPacket audio_packet(std::int64_t timestamp)
{
	EncodedPacket packet;
	packet.kind = PacketKind::Audio;
	packet.dts_us = timestamp;
	packet.pts_us = timestamp;
	packet.payload = {0x21, 0x00};
	return packet;
}

bool poll_until(const std::function<bool()> &condition, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!condition()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

void reconnecting_secondary_survives_continuous_boundary_polls()
{
	auto primary = std::make_shared<FakeState>(), secondary = std::make_shared<FakeState>();
	std::size_t index = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(index++ == 0 ? primary : secondary); },
		{{64, 65536}, 3, 1ms, 5s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "synthetic"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"multistream start");
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 1, SenderState::Running),
		"both destinations ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x60, 1000), audio_packet(1000)}}));
	require(poll_until([&] { return sender.boundary_delivered(1); }, 2s), "both destinations publish the programme");
	{
		std::scoped_lock lock(secondary->mutex);
		secondary->fail_on_send = secondary->sends + 1;
		secondary->block_reconnect = true;
	}
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x61, 2000)}}));
	{
		std::unique_lock lock(secondary->mutex);
		require(secondary->changed.wait_for(lock, 2s, [&] { return secondary->connects == 2; }),
			"secondary network drop must reach a blocked reconnect");
	}
	// The pipeline owner polls the boundary on every loop while broadcasting.
	(void)poll_until([&] { (void)sender.boundary_delivered(1); return false; }, 1300ms);
	require(sender.status().destinations[1].sender.state == SenderState::Reconnecting,
		"a reconnecting secondary must not be shut off by steady-state boundary polls");
	{
		std::scoped_lock lock(secondary->mutex);
		secondary->block_reconnect = false;
		secondary->changed.notify_all();
	}
	require(wait_for_state(sender, 1, SenderState::Running), "secondary must reconnect");
	// Reconnected but still waiting for the programme's next keyframe.
	(void)poll_until([&] { (void)sender.boundary_delivered(1); return false; }, 1300ms);
	require(sender.status().destinations[1].sender.state == SenderState::Running,
		"a reconnected secondary awaiting a keyframe must not be shut off");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x62, 5000), audio_packet(5000)}}));
	require(poll_until([&] { return sender.boundary_delivered(1); }, 2s), "recovered secondary must resume the programme");
	const auto status = sender.status();
	sender.stop();
	require(status.destinations[1].sender.state == SenderState::Running, "recovered secondary must stay in service");
}

void secondary_stalled_between_alternating_tickets_is_isolated()
{
	auto primary = std::make_shared<FakeState>(), secondary = std::make_shared<FakeState>();
	std::size_t index = 0;
	MultiTargetSender sender([&] { return std::make_unique<FakeConnection>(index++ == 0 ? primary : secondary); },
		{{64, 65536}, 0, 1ms, 5s});
	std::string error;
	require(sender.start({"rtmp://primary.test/live", "synthetic"}, "Primary", one_secondary(), codec_headers(), {}, error),
		"multistream start");
	require(wait_for_state(sender, 0, SenderState::Running) && wait_for_state(sender, 1, SenderState::Running),
		"both destinations ready");
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x80, 1000), audio_packet(1000)}}));
	require(poll_until([&] { return sender.boundary_delivered(1); }, 2s), "both destinations publish the programme");
	sender.discontinuity({2, "holding"});
	sender.consume(std::make_shared<const ReleasedPacketBatch>(
		ReleasedPacketBatch{2, {{PacketKind::Audio, {0}, 2000, 2000, false, true}}, codec_headers(), 1}));
	require(poll_until([&] { return sender.delivered(2, 1); }, 2s), "both destinations deliver the drain guard");
	{
		std::scoped_lock lock(secondary->mutex);
		secondary->stall_on_send = secondary->sends + 1;
	}
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{2, {keyframe(0x81, 3000)}, std::nullopt, 2}));
	// Paced holding asks about the drain ticket and the holding ticket on every tick.
	const bool released = poll_until([&] { (void)sender.delivered(2, 1); return sender.delivered(2, 2); }, 2500ms);
	const auto status = sender.status();
	sender.stop();
	require(released && status.destinations[0].sender.state == SenderState::Running &&
			status.destinations[1].sender.state == SenderState::Failed,
		"a secondary stalled on the holding ticket must be isolated even while the drain ticket is also polled");
}
} // namespace

int main()
{
	try {
		stalled_secondary_guard_ack_is_isolated();
		reconnecting_secondary_survives_continuous_boundary_polls();
		secondary_stalled_between_alternating_tickets_is_isolated();
		stalled_destinations_start_asynchronously_and_cancel_together();
		destructor_cancels_all_destinations_before_join();
		partial_start_failure_cancels_all_before_join();
		destinations_receive_identical_flv_order();
		three_destinations_receive_identical_flv_order();
		secondary_start_failure_is_isolated();
		one_secondary_failure_does_not_stop_the_other_secondary();
		slow_secondary_does_not_stall_primary();
		recovering_secondary_does_not_interrupt_healthy_destinations();
		secondary_backpressure_is_isolated_and_shutdown_is_clean();
		shutdown_status_remains_stopping_until_every_worker_joins();
		duplicate_destination_identity_is_rejected_without_exposing_secrets();
		version_one_migration_preserves_the_secret_in_slot_two();
		preflight_counts_upload_and_enforces_known_platform_limits();
		validation_and_labels_never_expose_secrets();
		std::cout << "multi-target sender tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "multi-target sender test failure: " << error.what() << '\n';
		return 1;
	}
}
