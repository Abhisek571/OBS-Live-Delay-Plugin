#pragma once

#include "delay-controller.hpp"

extern "C" {
#include <obs.h>
}

namespace active_delay {

EncodedPacket copy_encoder_packet(const encoder_packet &packet);

} // namespace active_delay
