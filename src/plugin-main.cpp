#include "active-delay-dock.hpp"
#include "active-delay-output.hpp"
#include "diagnostic-error.hpp"
#include "holding-obs-capture.hpp"
#include "scene-capture-probe.hpp"

#include <memory>

#include <QPointer>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-active-live-delay", "en-US")

namespace {
QPointer<active_delay::ActiveDelayDock> dock;
std::shared_ptr<active_delay::ActiveDelaySession> session;
obs_hotkey_id enable_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id return_live_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id dump_hotkey = OBS_INVALID_HOTKEY_ID;

struct PluginHotkey {
	obs_hotkey_id *id;
	const char *name;
};
const PluginHotkey plugin_hotkeys[] = {{&enable_hotkey, "active_delay_enable"},
	{&return_live_hotkey, "active_delay_return_live"}, {&dump_hotkey, "active_delay_emergency_dump"}};

void unregister_hotkeys()
{
	for (const auto &hotkey : plugin_hotkeys) {
		if (*hotkey.id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(*hotkey.id);
			*hotkey.id = OBS_INVALID_HOTKEY_ID;
		}
	}
}

// OBS saves the bindings of plugin hotkeys into the profile when Settings is
// applied, but restores only its own; without this they vanish on restart.
void load_hotkey_bindings()
{
	auto *profile = obs_frontend_get_profile_config();
	if (!profile)
		return;
	for (const auto &hotkey : plugin_hotkeys) {
		if (*hotkey.id == OBS_INVALID_HOTKEY_ID)
			continue;
		const auto *json = config_get_string(profile, "Hotkeys", hotkey.name);
		obs_data_t *data = json ? obs_data_create_from_json(json) : nullptr;
		obs_data_array_t *bindings = data ? obs_data_get_array(data, "bindings") : nullptr;
		obs_hotkey_load(*hotkey.id, bindings);
		obs_data_array_release(bindings);
		obs_data_release(data);
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

// A bound key is a deliberate act; unlike the button it skips the confirmation.
void dump_hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		dock->emergency_dump_now();
}

void create_dock()
{
	if (dock)
		return;

	auto *panel = new active_delay::ActiveDelayDock(session, nullptr);
	if (!obs_frontend_add_dock_by_id("active_delay_dock", "Active Live Delay", panel)) {
		blog(LOG_ERROR, "[active-live-delay] %s",
			active_delay::diagnostic_error(active_delay::DiagnosticCode::DockRegistrationFailed,
				"OBS refused to register the Active Live Delay dock")
				.c_str());
		delete panel;
		return;
	}

	dock = panel;
}

void frontend_event_callback(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		create_dock();
		load_hotkey_bindings();
		return;
	}
	if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
		load_hotkey_bindings();
	if (dock && event != OBS_FRONTEND_EVENT_EXIT)
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
	active_delay::register_obs_holding_outputs();
#if ACTIVE_DELAY_ENABLE_SCENE_PROBE
	active_delay::register_scene_capture_probe();
#endif
	obs_frontend_add_event_callback(frontend_event_callback, nullptr);
	enable_hotkey = obs_hotkey_register_frontend("active_delay_enable", obs_module_text("Hotkey.StartDelay"),
		enable_hotkey_callback, nullptr);
	return_live_hotkey = obs_hotkey_register_frontend("active_delay_return_live", obs_module_text("Hotkey.ReturnLive"),
		return_live_hotkey_callback, nullptr);
	dump_hotkey = obs_hotkey_register_frontend("active_delay_emergency_dump", obs_module_text("Hotkey.EmergencyDump"),
		dump_hotkey_callback, nullptr);
	if (enable_hotkey == OBS_INVALID_HOTKEY_ID || return_live_hotkey == OBS_INVALID_HOTKEY_ID ||
		dump_hotkey == OBS_INVALID_HOTKEY_ID) {
		blog(LOG_WARNING, "[active-live-delay] %s",
			active_delay::diagnostic_error(active_delay::DiagnosticCode::HotkeyRegistrationFailed,
				"OBS could not register one or more Active Live Delay hotkeys; dock controls remain available")
				.c_str());
	}
	return true;
}

void obs_module_unload()
{
	session.reset();
}
