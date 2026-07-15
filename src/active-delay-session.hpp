#pragma once

#include "delay-controller.hpp"
#include "flv-muxer.hpp"

#include <atomic>
#include <mutex>
#include <optional>

namespace active_delay {

class ActiveDelaySession {
public:
	DelayController controller;
	std::atomic_bool preserve_controller_on_next_output_start = false;
	std::mutex codec_headers_mutex;
	std::optional<FlvCodecHeaders> cached_codec_headers;
};

} // namespace active_delay
