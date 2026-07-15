#include "delay-controller.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

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

void builds_and_releases_delay()
{
	DelayController controller;
	assert(controller.set_target(2s));
	controller.ingest(video(0, true));
	controller.ingest(audio(0));
	controller.ingest(video(1'000'000, true));
	controller.ingest(audio(1'000'000));
	controller.ingest(video(2'000'000, true));
	controller.ingest(audio(2'000'000));
	assert(controller.status().state == DelayState::Delayed);

	controller.ingest(video(3'000'000, true));
	auto ready = controller.take_ready_packets();
	assert(!ready.empty());
	assert(ready.front().kind == PacketKind::Video && ready.front().keyframe);
}

void returning_live_clears_the_buffer()
{
	DelayController controller;
	assert(controller.set_target(2s));
	controller.ingest(video(0, true));
	controller.ingest(video(2'000'000, true));
	controller.return_live();
	const auto state = controller.status();
	assert(state.state == DelayState::Live);
	assert(state.current_delay == 0us);
}

void refuses_over_limit_target()
{
	DelayController controller({5s, 1024 * 1024});
	std::string error;
	assert(!controller.set_target(6s, &error));
	assert(!error.empty());
}
} // namespace

int main()
{
	builds_and_releases_delay();
	returning_live_clears_the_buffer();
	refuses_over_limit_target();
	std::cout << "delay-controller tests passed\n";
}
