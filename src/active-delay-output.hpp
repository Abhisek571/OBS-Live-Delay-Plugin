#pragma once

#include "active-delay-session.hpp"

#include <memory>

extern "C" {
#include <obs-module.h>
}

namespace active_delay {

bool cache_active_codec_headers(obs_output_t *output, ActiveDelaySession &session, std::string &error);
void clear_cached_codec_headers(ActiveDelaySession &session);
void register_active_delay_output(std::shared_ptr<ActiveDelaySession> session);

} // namespace active_delay
