#pragma once
#include <algorithm>
#include <cstddef>

namespace active_delay {
// Float-planar PCM only. No input-source or global-mixer reference exists.
inline bool fill_holding_silence(float *const *planes, std::size_t channels, std::size_t frames) noexcept
{
	if (!planes || channels == 0 || channels > 8) return false;
	for (std::size_t i = 0; i < channels; ++i)
		if (!planes[i]) return false;
	for (std::size_t i = 0; i < channels; ++i)
		std::fill_n(planes[i], frames, 0.0f);
	return true;
}
} // namespace active_delay