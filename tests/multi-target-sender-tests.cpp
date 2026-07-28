#include "flv-muxer.hpp"
#include "multi-target-sender.hpp"
#include "multistream-preflight.hpp"

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
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count, std::chrono::milliseconds timeout);

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
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x46)}}));
	require(wait_for_writes(primary, 4), "primary must continue after one secondary fails");
	require(wait_for_writes(healthy, 4), "healthy secondary must continue after the other secondary fails");
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
	secondary->delay_media = true;
	sender.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{1, {keyframe(0x44)}}));
	require(wait_for_writes(primary, 4, 150ms), "slow secondary must not delay primary media delivery");
	sender.stop();
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
	secondary->delay_media = true;
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
	auto stop_start = std::chrono::steady_clock::now();
	sender.stop();
	require(std::chrono::steady_clock::now() - stop_start < 2s, "fan-out shutdown should join workers cleanly");
	std::scoped_lock lock(primary->mutex, secondary->mutex, other_secondary->mutex);
	require(primary->closes > 0 && secondary->closes > 0 && other_secondary->closes > 0,
		"two-secondary shutdown must close every target worker");
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
} // namespace

int main()
{
	try {
		destinations_receive_identical_flv_order();
		three_destinations_receive_identical_flv_order();
		secondary_start_failure_is_isolated();
		one_secondary_failure_does_not_stop_the_other_secondary();
		slow_secondary_does_not_stall_primary();
		secondary_backpressure_is_isolated_and_shutdown_is_clean();
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
