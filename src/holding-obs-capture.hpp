#pragma once
#include "holding-capture.hpp"
#include <obs.h>

namespace active_delay {
struct ObsHoldingConfig {
	std::string scene_name;
	std::string video_encoder_id = "obs_x264";
	std::string audio_encoder_id = "ffmpeg_aac";
	uint32_t video_bitrate_kbps = 2500;
	uint32_t audio_bitrate_kbps = 128;
	uint32_t sample_rate = 48000;
	speaker_layout speakers = SPEAKERS_STEREO;
};
// Prepare after libobs/encoder modules and host video have initialized.
// Uses the host video format/dimensions but creates a separate view/video mix.
// Never assigns, stops, or modifies the programme output or global audio.
std::unique_ptr<HoldingCapture> make_obs_holding_capture(ObsHoldingConfig config);
// libobs output registration is not thread-safe against obs_output_create.
// Call from obs_module_load; prepare() keeps a once-only fallback for harnesses.
void register_obs_holding_outputs();
} // namespace active_delay