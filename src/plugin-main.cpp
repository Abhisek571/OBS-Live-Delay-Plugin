#include "active-delay-dock.hpp"
#include "active-delay-output.hpp"

#include <memory>

#include <QPointer>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-active-live-delay", "en-US")

namespace {
QPointer<active_delay::ActiveDelayDock> dock;
std::shared_ptr<active_delay::ActiveDelaySession> session;
obs_hotkey_id enable_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id return_live_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id dump_hotkey = OBS_INVALID_HOTKEY_ID;

void unregister_hotkeys()
{
	if (enable_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(enable_hotkey);
		enable_hotkey = OBS_INVALID_HOTKEY_ID;
	}
	if (return_live_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(return_live_hotkey);
		return_live_hotkey = OBS_INVALID_HOTKEY_ID;
	}
	if (dump_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(dump_hotkey);
		dump_hotkey = OBS_INVALID_HOTKEY_ID;
	}
}

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

void create_dock()
{
	if (dock)
		return;

	auto *panel = new active_delay::ActiveDelayDock(session);
	if (!obs_frontend_add_dock_by_id("active_delay_dock", "Active Live Delay", panel)) {
		blog(LOG_ERROR, "Failed to register the Active Live Delay dock");
		delete panel;
		return;
	}

	dock = panel;
}

void frontend_event_callback(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		create_dock();
		return;
	}
	if (dock)
		dock->handle_frontend_event(event);
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (dock)
			dock->shutdown();
		unregister_hotkeys();
		obs_frontend_remove_dock("active_delay_dock");
		dock = nullptr;
	}
}
} // namespace

bool obs_module_load(void)
{
	session = std::make_shared<active_delay::ActiveDelaySession>();
	active_delay::register_active_delay_output(session);
	obs_frontend_add_event_callback(frontend_event_callback, nullptr);
	enable_hotkey = obs_hotkey_register_frontend("active_delay_enable", "Active Delay: Enable / Set Delay", enable_hotkey_callback, nullptr);
	return_live_hotkey = obs_hotkey_register_frontend("active_delay_return_live", "Active Delay: Return Live", return_live_hotkey_callback, nullptr);
	dump_hotkey = obs_hotkey_register_frontend("active_delay_dump", "Active Delay: Emergency Dump", dump_hotkey_callback, nullptr);
	return true;
}

void obs_module_unload()
{
	session.reset();
}
