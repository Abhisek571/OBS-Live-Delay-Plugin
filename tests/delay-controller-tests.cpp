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
	{
		DelayController limited({10s, 1000}); std::string error;
		require(!limited.arm(0s, error) && !limited.arm(-1s, error), "zero and negative targets cannot arm");
		require(limited.arm(2s, error), "bounded controller must arm");
		limited.ingest(video(0, true));
		require(limited.status().state == DelayState::Error && limited.status().buffered_bytes == 0,
			"armed memory exhaustion must fail closed and release retained payloads");
	}
	{
		DelayController missing; std::string error;
		require(missing.arm(1s, error), "missing-media fixture");
		missing.ingest(audio(0)); missing.ingest(video(1000000, false));
		require(!missing.prebuffer_ready() && !missing.begin_broadcast(), "no keyframe cannot become Ready");
		missing.ingest(video(2000000, true)); missing.ingest(video(3000000, true));
		require(!missing.prebuffer_ready(), "video without retained audio cannot become Ready");
		require(!missing.set_target(2s, &error), "armed target changes require disarm");
		missing.return_live(); require(missing.arm(1s, error), "rearm must be allowed after disarm");
		require(!missing.prebuffer_ready() && missing.status().buffered_bytes == 0, "rearm must never reuse history");
	}
	{
		DelayController armed({10s, 50000});
		std::string error;
		require(armed.arm(2s, error), "positive target must arm off-air");
		for (int i = 0; i <= 36000; ++i) {
			armed.ingest(video(i * 100000LL, i % 10 == 0));
			armed.ingest(audio(i * 100000LL));
			require(armed.take_ready_packets().empty(), "armed history must never be released");
		}
		require(armed.prebuffer_ready(), "rolling history must retain aged keyframe and audio");
		require(armed.status().buffered_bytes <= 50000, "history must stay bounded");
		require(armed.begin_broadcast(), "ready history must start without reset");
		armed.ingest(video(3600100000LL, false));
		auto packets = armed.take_ready_packets();
		require(!packets.empty() && packets.front().keyframe && packets.front().dts_us == 3598000000LL,
			"broadcast must begin at latest safely aged keyframe, not stale or live prefix");
		require(armed.status().target_delay == 2s, "start must preserve target");
	}
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

void released_payloads_do_not_count_toward_retained_memory()
{
	DelayController controller({10s, 6'000});
	require(controller.set_target(2s), "delay should be accepted");
	for (std::int64_t second = 0; second < 1'000; ++second) {
		controller.ingest(video(second * 1'000'000, true));
		controller.ingest(audio(second * 1'000'000));
		const auto state = controller.status();
		require(state.state != DelayState::Error, "released payloads must not exhaust the memory limit");
		const auto retained_seconds = second < 2 ? second + 1 : 3;
		require(state.buffered_bytes == retained_seconds * (1'200 + 256),
			"retained bytes must exclude moved video and audio payloads");
		controller.take_ready_packets();
	}
	controller.return_live();
	require(controller.status().buffered_bytes == 0, "return live must clear retained bytes");

	DelayController empty_payloads({10s, 0});
	require(empty_payloads.set_target(1s), "zero-byte buffering should be accepted");
	for (std::int64_t second = 0; second < 4; ++second) {
		empty_payloads.ingest({PacketKind::Video, {}, second * 1'000'000, second * 1'000'000, true});
		require(empty_payloads.status().buffered_bytes == 0, "empty payloads must not underflow byte accounting");
		require(empty_payloads.status().state != DelayState::Error, "empty payloads must fit a zero-byte limit");
		empty_payloads.take_ready_packets();
	}
}

void trimming_equal_timestamps_makes_progress()
{
	for (const bool duplicate_keyframe : {false, true}) {
		DelayController controller;
		require(controller.set_target(3s), "initial delay should be accepted");
		controller.ingest(video(0, true));
		controller.ingest(audio(1'000'000));
		controller.ingest(video(1'000'000, true));
		if (duplicate_keyframe)
			controller.ingest(video(1'000'000, true));
		controller.ingest(video(2'000'000, true));
		controller.ingest(video(3'000'000, false));
		require(controller.set_target(1s), "equal-DTS trim must finish at a safe later keyframe");
		require(controller.status().current_delay == 1s, "trim must reach the requested delay");
		require(controller.status().buffered_bytes == 2 * 1'200, "trim must account for every discarded packet");
		controller.ingest(video(4'000'000, false));
		const auto ready = controller.take_ready_packets();
		require(ready.size() == 1 && ready.front().keyframe && ready.front().dts_us == 2'000'000,
			"trimmed playback must start at the selected keyframe, not preceding equal-DTS audio");
	}
}

void trimming_without_a_later_keyframe_fails()
{
 DelayController controller;
 require(controller.set_target(3s), "initial delay should be accepted");
 controller.ingest(video(0, true));
 controller.ingest(video(1'000'000, false));
 controller.ingest(video(3'000'000, false));
 require(controller.set_target(1s), "reduction must remain pending rather than fail when a safe boundary is not yet available");
 require(controller.status().state == DelayState::BuildingDelay, "missing safe boundary must withhold programme behind holding");
 controller.ingest(video(4'000'000, true));
 require(controller.take_ready_packets().empty(), "reduction cannot leak the unsafe dependent prefix");
 controller.ingest(video(5'100'000, false));
 const auto ready=controller.take_ready_packets();
 require(!ready.empty() && ready.front().keyframe && ready.front().dts_us==4'000'000,
  "pending reduction must resume at a safe new programme keyframe");
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

void live_transitions_wait_for_video_keyframe_and_aligned_audio()
{
	for (int transition = 0; transition < 3; ++transition) {
		DelayController controller;
		require(controller.set_target(2s), "delay should be accepted");
		controller.ingest(video(0, true));
		controller.ingest(video(3'000'000, true));
		if (transition == 0)
			controller.return_live();
		else if (transition == 1)
			require(controller.set_target(0us), "zero delay should be accepted");
		else
			controller.reset_for_discontinuity("test reset");
		controller.ingest(audio(4'000'000));
		controller.ingest(video(4'000'000, false));
		require(controller.take_ready_packets().empty(), "transition must discard audio and dependent video before the keyframe");
		controller.ingest(video(5'000'000, true));
		controller.ingest(audio(4'900'000));
		controller.ingest(audio(5'000'000));
		const auto ready = controller.take_ready_packets();
		require(ready.size() == 2 && ready[0].keyframe && ready[1].kind == PacketKind::Audio,
			"transition should resume with the keyframe and matching audio");
	}
}

void changing_delay_discards_pending_releases()
{
	for (const auto target : {1s, 4s}) {
		DelayController controller;
		require(controller.set_target(2s), "delay should be accepted");
		for (int second = 0; second <= 3; ++second)
			controller.ingest(video(second * 1'000'000, true));
		require(controller.set_target(target), "changed delay should be accepted");
		require(controller.take_ready_packets().empty(), "old released media must not survive a delay change");
	}
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
 require(controller.set_target(1s),"delay accepted");
 controller.ingest(video(0,false));
 controller.ingest(audio(1'000'000));
 require(controller.status().state==DelayState::BuildingDelay,"initial build must wait behind holding when capture begins inside a GOP");
 require(controller.take_ready_packets().empty(),"dependent initial prefix must not be published");
 controller.ingest(video(2'000'000,true));
 controller.ingest(audio(2'001'000));
 controller.ingest(video(3'100'000,false));
 const auto ready=controller.take_ready_packets();
 require(!ready.empty() && ready.front().keyframe && ready.front().dts_us==2'000'000,
  "initial build must resume from the first safe retained programme keyframe");
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
		live_transitions_wait_for_video_keyframe_and_aligned_audio();
		changing_delay_discards_pending_releases();
		trimming_equal_timestamps_makes_progress();
		trimming_without_a_later_keyframe_fails();
		released_payloads_do_not_count_toward_retained_memory();
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
