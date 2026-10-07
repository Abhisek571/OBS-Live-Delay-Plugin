#pragma once
#include "delay-controller.hpp"
#include <obs.h>
#include "flv-muxer.hpp"
#include <mutex>
#include <optional>
#include <span>

namespace active_delay {
// Single-media encoded outputs do not rebase encoder packets. Use libobs's
// common system clock, preserving raw PTS-DTS (including B-frame offsets).
EncodedPacket copy_holding_encoder_packet(const encoder_packet &packet);

struct MonitoredCapturePacket {
 EncodedPacket packet;
 std::optional<FlvCodecHeaders> headers;
};
// Check each encoder's own extradata on its packet callback, never query the
// other encoder concurrently. One monitor per capture session, reset off-callback.
class ObsCaptureCodecMonitor {
public:
 MonitoredCapturePacket observe(const encoder_packet &,obs_encoder_t *);
 MonitoredCapturePacket observe(const encoder_packet &,std::span<const std::uint8_t> extra);
 void reset();
private:
 std::mutex mutex_;
 FlvCodecHeaders headers_;
 std::vector<std::uint8_t> video_extra_,audio_extra_;
};
} // namespace active_delay