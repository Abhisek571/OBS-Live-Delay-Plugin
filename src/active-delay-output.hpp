#pragma once

#include "delay-controller.hpp"

extern "C" {
#include <obs-module.h>
}

namespace active_delay {

// Registers an encoded H.264/AAC output. The output owns packet buffering;
// RTMP/FLV delivery is intentionally injected behind this boundary.
void register_active_delay_output();

} // namespace active_delay
