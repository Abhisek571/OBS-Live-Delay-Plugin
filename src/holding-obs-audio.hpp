#pragma once
#include <media-io/audio-io.h>
#include <string>

namespace active_delay {
class HoldingSilentAudio {
public:
	~HoldingSilentAudio() { close(); }
	HoldingSilentAudio() = default;
	HoldingSilentAudio(const HoldingSilentAudio &) = delete;
	HoldingSilentAudio &operator=(const HoldingSilentAudio &) = delete;
	bool open(uint32_t rate, speaker_layout speakers, std::string &error);
	void close() noexcept;
	audio_t *get() const noexcept { return audio_; }
private:
	static bool input(void *, uint64_t start, uint64_t end, uint64_t *timestamp,
		uint32_t active_mixers, audio_output_data *mixes) noexcept;
	audio_t *audio_ = nullptr;
	std::size_t channels_ = 0;
};
} // namespace active_delay