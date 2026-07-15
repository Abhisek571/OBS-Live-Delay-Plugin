#include "active-delay-dock.hpp"
#include "active-delay-output.hpp"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-active-live-delay", "en-US")

namespace {
active_delay::ActiveDelayDock *dock = nullptr;
obs_hotkey_id enable_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id return_live_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id dump_hotkey = OBS_INVALID_HOTKEY_ID;

void enable_hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		dock->enable_delay();
}

void return_live_hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		dock->return_live();
}

void dump_hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		dock->emergency_dump();
}
} // namespace

bool obs_module_load(void)
{
	active_delay::register_active_delay_output();
	dock = new active_delay::ActiveDelayDock();
	obs_frontend_add_custom_qdock("active_delay_dock", dock);
	enable_hotkey = obs_hotkey_register_frontend("active_delay_enable", "Active Delay: Enable / Set Delay", enable_hotkey_callback, nullptr);
	return_live_hotkey = obs_hotkey_register_frontend("active_delay_return_live", "Active Delay: Return Live", return_live_hotkey_callback, nullptr);
	dump_hotkey = obs_hotkey_register_frontend("active_delay_dump", "Active Delay: Emergency Dump", dump_hotkey_callback, nullptr);
	return true;
}

void obs_module_unload()
{
	obs_frontend_remove_dock("active_delay_dock");
	delete dock;
	dock = nullptr;
}
