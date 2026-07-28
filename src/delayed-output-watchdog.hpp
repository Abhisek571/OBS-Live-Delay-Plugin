#pragma once

#include <chrono>

namespace active_delay {

enum class DelayedOutputHealth { WaitingForVideo, Progressing, Stopped, VideoTimeout };

DelayedOutputHealth evaluate_delayed_output_health(bool active, int total_video_frames,
	std::chrono::milliseconds active_for, std::chrono::milliseconds video_timeout);

} // namespace active_delay
