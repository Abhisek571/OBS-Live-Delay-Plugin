#pragma once

#include "active-delay-session.hpp"

#include <memory>

extern "C" {
#include <obs-module.h>
}

namespace active_delay {

void register_active_delay_output(std::shared_ptr<ActiveDelaySession> session);

} // namespace active_delay
