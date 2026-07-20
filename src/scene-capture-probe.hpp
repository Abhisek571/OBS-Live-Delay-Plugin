#pragma once

namespace active_delay {

// P4-only source registration.  This probe mirrors one selected scene's video
// and libobs scene mix; it intentionally does not decode, delay, or publish.
void register_scene_capture_probe();

} // namespace active_delay
