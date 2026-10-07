#include "holding-obs-audio.hpp"
#include "holding-silence.hpp"
#include <obs.h>

namespace active_delay {
bool HoldingSilentAudio::input(void *param, uint64_t start, uint64_t, uint64_t *timestamp,
	uint32_t active_mixers, audio_output_data *mixes) noexcept
{
	if (!param || !timestamp || !mixes) return false;
	const auto &self = *static_cast<HoldingSilentAudio *>(param);
	for (std::size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
		if ((active_mixers & (1u << mix)) &&
			!fill_holding_silence(mixes[mix].data, self.channels_, AUDIO_OUTPUT_FRAMES)) return false;
	}
	// The libobs audio clock supplies exact block boundaries; preserve them.
	*timestamp = start;
	return true;
}
bool HoldingSilentAudio::open(uint32_t rate, speaker_layout speakers, std::string &error)
{
	error.clear();
	if (!obs_initialized() || audio_ || rate < 8000 || rate > 192000 || !get_audio_channels(speakers)) {
		error = "HOLDING_AUDIO_CONFIG: invalid or already-open silent audio output";
		return false;
	}
	channels_ = get_audio_channels(speakers);
	audio_output_info info{};
	info.name = "Active Delay holding silence";
	info.samples_per_sec = rate;
	info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	info.speakers = speakers;
	info.input_callback = input;
	info.input_param = this;
	if (audio_output_open(&audio_, &info) != AUDIO_OUTPUT_SUCCESS) {
		error = "HOLDING_AUDIO_OPEN: plugin-owned silent audio output unavailable";
		return false;
	}
	return true;
}
void HoldingSilentAudio::close() noexcept
{
	if (audio_) audio_output_close(audio_); // joins the owned audio thread
	audio_ = nullptr;
	channels_ = 0;
}
} // namespace active_delay