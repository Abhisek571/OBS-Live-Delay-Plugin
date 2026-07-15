#include "delayed-output-watchdog.hpp"

namespace active_delay {

DelayedOutputHealth evaluate_delayed_output_health(bool active, int total_video_frames,
	std::chrono::milliseconds active_for, std::chrono::milliseconds video_timeout)
{
	if (!active)
		return DelayedOutputHealth::Stopped;
	if (total_video_frames > 0)
		return DelayedOutputHealth::Progressing;
	if (active_for >= video_timeout)
		return DelayedOutputHealth::VideoTimeout;
	return DelayedOutputHealth::WaitingForVideo;
}

} // namespace active_delay
