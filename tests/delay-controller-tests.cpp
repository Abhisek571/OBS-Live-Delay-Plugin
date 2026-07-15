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

void returning_live_clears_the_buffer()
{
	DelayController controller;
	require(controller.set_target(2s), "2-second delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(2'000'000, true));
	controller.return_live();
	const auto state = controller.status();
	require(state.state == DelayState::Live, "returning live should reset the state");
	require(state.current_delay == 0us, "returning live should clear buffered delay");
}

void refuses_over_limit_target()
{
	DelayController controller({5s, 1024 * 1024});
	std::string error;
	require(!controller.set_target(6s, &error), "over-limit target must be rejected");
	require(!error.empty(), "over-limit target should provide an error");
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

void emergency_dump_requires_a_delayed_stream()
{
	DelayController controller;
	std::string error;
	require(!controller.emergency_dump(1s, &error), "dump before delay is active must be rejected");
	require(!error.empty(), "invalid dump should have an error");

	require(controller.set_target(2s), "delay should be accepted");
	controller.ingest(video(0, true));
	controller.ingest(video(1'000'000, true));
	controller.ingest(video(2'000'000, true));
	require(controller.status().state == DelayState::Delayed, "stream should be delayed");
	require(controller.emergency_dump(1s, &error), "safe dump should be accepted");
	require(controller.status().target_delay == 1s, "dump should lower the target delay");
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
} // namespace

int main()
{
	try {
		builds_and_releases_delay();
		returning_live_clears_the_buffer();
		refuses_over_limit_target();
		refuses_to_start_delayed_playback_without_a_keyframe();
		enforces_the_memory_limit();
		rejects_timestamp_regressions_while_buffering();
		emergency_dump_requires_a_delayed_stream();
		continues_after_a_forward_timestamp_gap();
		rebases_a_restarted_timestamp_epoch_without_losing_delay();
		preserves_composition_and_av_offsets_when_rebasing();
		std::cout << "delay-controller tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "delay-controller test failure: " << error.what() << '\n';
		return 1;
	}
}
