#include "delay-controller.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
EncodedPacket video(std::int64_t timestamp, bool keyframe)
{
	return {PacketKind::Video, std::vector<std::uint8_t>(1200), timestamp, timestamp, keyframe};
}

EncodedPacket audio(std::int64_t timestamp)
{
	return {PacketKind::Audio, std::vector<std::uint8_t>(256), timestamp, timestamp, false};
}

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void builds_and_releases_delay()
{
	DelayController controller;
	require(controller.set_target(2s), "2-second delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(audio(0));
	controller.ingest(video(1'000'000, true));
	controller.ingest(audio(1'000'000));
	controller.ingest(video(2'000'000, true));
	controller.ingest(audio(2'000'000));
	require(controller.status().state == DelayState::Delayed, "delay should become active once buffered");

	controller.ingest(video(3'000'000, true));
	auto ready = controller.take_ready_packets();
	require(!ready.empty(), "delayed packets should be released after the delay is built");
	require(ready.front().kind == PacketKind::Video && ready.front().keyframe,
		"delayed playback must begin on a video keyframe");
}

void preserves_live_timestamps_and_drains_each_packet_once()
{
	DelayController controller;
	controller.ingest({PacketKind::Video, {0x01}, 1'033'333, 1'000'000, true});
	controller.ingest({PacketKind::Audio, {0x02}, 1'010'000, 1'010'000, false});

	const auto ready = controller.take_ready_packets();
	require(ready.size() == 2, "live packets should be released in one drain");
	require(ready[0].kind == PacketKind::Video && ready[0].dts_us == 1'000'000 &&
		ready[0].pts_us == 1'033'333,
		"the live path must preserve video DTS, PTS, and ingest order");
	require(ready[1].kind == PacketKind::Audio && ready[1].dts_us == 1'010'000 &&
		ready[1].pts_us == 1'010'000,
		"the live path must preserve audio timestamps and ingest order");
	require(controller.take_ready_packets().empty(), "a released packet must not be returned twice");
}

void releases_delayed_packets_in_ingest_order()
{
	DelayController controller;
	require(controller.set_target(2s), "2-second delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(audio(0));
	controller.ingest(video(1'000'000, true));
	controller.ingest(audio(1'000'000));
	controller.ingest(video(2'000'000, true));
	controller.ingest(audio(2'000'000));
	controller.ingest(video(3'000'000, true));

	const auto ready = controller.take_ready_packets();
	require(ready.size() == 2, "only media older than the target delay should be released");
	require(ready[0].kind == PacketKind::Video && ready[0].dts_us == 0,
		"the delayed release must begin with the buffered keyframe");
	require(ready[1].kind == PacketKind::Audio && ready[1].dts_us == 0,
		"equal-timestamp packets must retain ingest order");
	require(controller.take_ready_packets().empty(), "the delayed drain must be exactly once");
}

void returning_live_clears_the_buffer()
{
	DelayController controller;
	require(controller.set_target(2s), "2-second delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(2'000'000, true));
	controller.ingest(video(3'000'000, true));
	controller.return_live();
	const auto state = controller.status();
	require(state.state == DelayState::Live, "returning live should reset the state");
	require(state.current_delay == 0us, "returning live should clear buffered delay");
	require(controller.take_ready_packets().empty(),
		"returning live must discard delayed packets released concurrently before the transition");
}

void starting_delay_discards_pending_live_packets()
{
	DelayController controller;
	controller.ingest(video(0, true));
	require(controller.set_target(2s), "2-second delay should be accepted");
	require(controller.take_ready_packets().empty(),
		"starting delay must not leak a pending live packet across the transition");
}

void refuses_over_limit_target()
{
	DelayController controller({5s, 1024 * 1024});
	std::string error;
	require(!controller.set_target(6s, &error), "over-limit target must be rejected");
	require(!error.empty(), "over-limit target should provide an error");
	require(error.starts_with("[ALD-E2007]"), "controller errors must carry a stable diagnostic code");
}

void refuses_to_start_delayed_playback_without_a_keyframe()
{
	DelayController controller;
	require(controller.set_target(1s), "delay should be accepted");
	controller.ingest(video(0, false));
	controller.ingest(audio(1'000'000));
	const auto state = controller.status();
	require(state.state == DelayState::Error, "delayed playback without a keyframe is unsafe");
	require(!state.error.empty(), "missing keyframe should be reported");
	require(state.error.starts_with("[ALD-E2007]"), "controller state errors must carry a stable diagnostic code");
}

void enforces_the_memory_limit()
{
	DelayController controller({10s, 1'000});
	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(0, true));
	const auto state = controller.status();
	require(state.state == DelayState::Error, "an oversized encoded packet must stop buffering");
	require(state.buffered_bytes == 1'200, "status should account for buffered packet bytes");
}

void rejects_timestamp_regressions_while_buffering()
{
	DelayController controller;
	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(1'000'000, true));
	controller.ingest(audio(999'999));
	const auto state = controller.status();
	require(state.state == DelayState::Error, "out-of-order DTS is unsafe for delay accounting");
	require(state.error.find("backwards") != std::string::npos, "timestamp error should be clear");
}

void continues_after_a_forward_timestamp_gap()
{
	DelayController controller;
	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(1'000'000, true));
	controller.ingest(video(2'000'000, true));
	require(controller.status().state == DelayState::Delayed, "delay should be active before handoff");

	controller.ingest(video(3'000'000, true));
	require(!controller.take_ready_packets().empty(), "pre-handoff delayed packets should be available");
	controller.ingest(video(5'000'000, true));
	const auto state = controller.status();
	require(state.state == DelayState::Delayed, "a forward reconnect gap must preserve delayed state");
	require(state.current_delay == 2s, "the controller should retain the configured delay after a gap");
	require(!controller.take_ready_packets().empty(), "packets should continue releasing after a reconnect gap");
}

void rebases_a_restarted_timestamp_epoch_without_losing_delay()
{
	DelayController controller;
	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(1'000'000, true));
	controller.ingest(video(2'000'000, true));
	controller.ingest(video(3'000'000, true));
	controller.take_ready_packets();
	require(controller.status().state == DelayState::Delayed, "delay should be active before encoder restart");

	controller.begin_timestamp_epoch();
	controller.ingest(video(0, true));
	controller.ingest(audio(0));
	controller.ingest(video(1'000'000, true));
	controller.ingest(video(2'000'000, true));
	controller.ingest(video(3'000'000, true));

	const auto state = controller.status();
	require(state.state == DelayState::Delayed, "a restarted encoder clock must preserve delayed state");
	require(state.error.empty(), "timestamp rebasing must not report a backwards-clock error");
	auto ready = controller.take_ready_packets();
	require(!ready.empty(), "rebased packets should continue advancing delayed playback");
	for (std::size_t index = 1; index < ready.size(); ++index)
		require(ready[index].dts_us >= ready[index - 1].dts_us, "released DTS values must remain monotonic");
}

void preserves_composition_and_av_offsets_when_rebasing()
{
	DelayController controller;
	controller.ingest({PacketKind::Video, {0x01}, 10'033'333, 10'000'000, true});
	controller.take_ready_packets();
	controller.begin_timestamp_epoch();
	controller.ingest({PacketKind::Video, {0x02}, 0, -33'333, true});
	controller.ingest(audio(0));

	const auto packets = controller.take_ready_packets();
	require(packets.size() == 2, "both packets from the restarted epoch should be released while live");
	require(packets[0].dts_us == 10'000'001, "the restarted epoch should continue after the prior DTS");
	require(packets[0].pts_us - packets[0].dts_us == 33'333,
		"timestamp rebasing must preserve video composition time");
	require(packets[1].dts_us - packets[0].dts_us == 33'333,
		"one common timestamp offset must preserve audio/video timing");
}

void discontinuity_discards_unreleased_media_and_starts_a_clean_epoch()
{
	DelayController controller;
	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(1'000'000, true));
	controller.ingest(video(2'000'000, true));
	controller.ingest(video(3'000'000, true));
	controller.reset_for_discontinuity("encoder restarted");

	const auto reset = controller.status();
	require(reset.state == DelayState::Live, "a discontinuity should return the controller to live mode");
	require(reset.current_delay == 0us && reset.target_delay == 0us,
		"a discontinuity must discard the old delayed timeline");
	require(reset.error == "encoder restarted", "the discontinuity reason should remain observable");
	require(controller.take_ready_packets().empty(),
		"a discontinuity must discard released-but-not-yet-consumed packets from the old epoch");

	controller.ingest(video(0, true));
	const auto ready = controller.take_ready_packets();
	require(ready.size() == 1 && ready.front().dts_us == 0,
		"the next encoder epoch should not inherit timestamps from discarded media");
}
} // namespace

int main()
{
	try {
		builds_and_releases_delay();
		preserves_live_timestamps_and_drains_each_packet_once();
		releases_delayed_packets_in_ingest_order();
		returning_live_clears_the_buffer();
		starting_delay_discards_pending_live_packets();
		refuses_over_limit_target();
		refuses_to_start_delayed_playback_without_a_keyframe();
		enforces_the_memory_limit();
		rejects_timestamp_regressions_while_buffering();
		continues_after_a_forward_timestamp_gap();
		rebases_a_restarted_timestamp_epoch_without_losing_delay();
		preserves_composition_and_av_offsets_when_rebasing();
		discontinuity_discards_unreleased_media_and_starts_a_clean_epoch();
		std::cout << "delay-controller tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "delay-controller test failure: " << error.what() << '\n';
		return 1;
	}
}
