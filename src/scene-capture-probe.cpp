#include "scene-capture-probe.hpp"

#include "diagnostic-error.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

extern "C" {
#include <obs-module.h>
}

namespace active_delay {
namespace {
constexpr char kInputSceneSetting[] = "input_scene";

struct SceneCaptureProbe {
	obs_source_t *source = nullptr;
	std::mutex mutex;
	obs_source_t *input_scene = nullptr;
	std::atomic_bool invalid_input_reported = false;
	std::atomic_bool recursion_reported = false;
};

void report_once(std::atomic_bool &reported, DiagnosticCode code, const char *message)
{
	if (!reported.exchange(true, std::memory_order_acq_rel))
		blog(LOG_WARNING, "[active-live-delay] %s", diagnostic_error(code, message).c_str());
}

obs_source_t *input_scene_ref(SceneCaptureProbe *probe)
{
	std::scoped_lock lock(probe->mutex);
	return probe->input_scene ? obs_source_get_ref(probe->input_scene) : nullptr;
}

struct DescendantCheck {
	obs_source_t *target = nullptr;
	bool found = false;
};

void find_descendant(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *check = static_cast<DescendantCheck *>(param);
	if (parent == check->target || child == check->target)
		check->found = true;
}

bool contains_source(obs_source_t *root, obs_source_t *target)
{
	if (root == target)
		return true;
	DescendantCheck check{target};
	obs_source_enum_full_tree(root, find_descendant, &check);
	return check.found;
}

bool probe_has_recursion(SceneCaptureProbe *probe, obs_source_t *input_scene)
{
	return !input_scene || contains_source(input_scene, probe->source);
}

void replace_input_scene(SceneCaptureProbe *probe, obs_source_t *replacement)
{
	obs_source_t *previous = nullptr;
	{
		std::scoped_lock lock(probe->mutex);
		previous = probe->input_scene;
		probe->input_scene = replacement;
	}
	if (previous)
		obs_source_release(previous);
}

void scene_capture_probe_update(void *data, obs_data_t *settings)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	const auto *name = obs_data_get_string(settings, kInputSceneSetting);
	if (!name || !*name) {
		replace_input_scene(probe, nullptr);
		return;
	}
	obs_source_t *selected = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!selected || !obs_scene_from_source(selected)) {
		if (selected)
			obs_source_release(selected);
		replace_input_scene(probe, nullptr);
		report_once(probe->invalid_input_reported, DiagnosticCode::SceneProbeInputInvalid,
			"The P4 scene-capture probe requires an existing Delay Input scene");
		return;
	}

	if (probe_has_recursion(probe, selected)) {
		obs_source_release(selected);
		replace_input_scene(probe, nullptr);
		report_once(probe->recursion_reported, DiagnosticCode::SceneProbeRecursionDetected,
			"The selected Delay Input contains this probe directly or through nested scenes");
		return;
	}

	probe->invalid_input_reported.store(false, std::memory_order_release);
	probe->recursion_reported.store(false, std::memory_order_release);
	replace_input_scene(probe, selected);
}

void *scene_capture_probe_create(obs_data_t *settings, obs_source_t *source)
{
	auto *probe = new SceneCaptureProbe;
	probe->source = source;
	scene_capture_probe_update(probe, settings);
	return probe;
}

void scene_capture_probe_destroy(void *data)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	replace_input_scene(probe, nullptr);
	delete probe;
}

void scene_capture_probe_load(void *data, obs_data_t *settings)
{
	// Scene collections can create sources before the referenced scene exists.
	scene_capture_probe_update(data, settings);
}

void scene_capture_probe_save(void *data, obs_data_t *settings)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	auto *input_scene = input_scene_ref(probe);
	if (!input_scene) {
		obs_data_set_string(settings, kInputSceneSetting, "");
		return;
	}
	obs_data_set_string(settings, kInputSceneSetting, obs_source_get_name(input_scene));
	obs_source_release(input_scene);
}

void scene_capture_probe_render(void *data, gs_effect_t *)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	auto *input_scene = input_scene_ref(probe);
	if (!input_scene)
		return;
	if (probe_has_recursion(probe, input_scene)) {
		report_once(probe->recursion_reported, DiagnosticCode::SceneProbeRecursionDetected,
			"The selected Delay Input became recursive; scene capture is blocked");
		obs_source_release(input_scene);
		return;
	}
	obs_source_video_render(input_scene);
	obs_source_release(input_scene);
}

bool scene_capture_probe_audio_render(void *data, uint64_t *timestamp, obs_source_audio_mix *audio_output,
	uint32_t mixers, size_t channels, size_t)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	auto *input_scene = input_scene_ref(probe);
	if (!input_scene)
		return false;
	if (probe_has_recursion(probe, input_scene)) {
		report_once(probe->recursion_reported, DiagnosticCode::SceneProbeRecursionDetected,
			"The selected Delay Input became recursive; scene audio is blocked");
		obs_source_release(input_scene);
		return false;
	}
	if (obs_source_audio_pending(input_scene)) {
		obs_source_release(input_scene);
		return false;
	}
	const auto input_timestamp = obs_source_get_audio_timestamp(input_scene);
	if (!input_timestamp) {
		obs_source_release(input_scene);
		return false;
	}

	obs_source_audio_mix input_audio{};
	obs_source_get_audio_mix(input_scene, &input_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
		if ((mixers & (1U << mix)) == 0)
			continue;
		for (size_t channel = 0; channel < channels; ++channel) {
			std::memcpy(audio_output->output[mix].data[channel], input_audio.output[mix].data[channel],
				AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}
	*timestamp = input_timestamp;
	obs_source_release(input_scene);
	return true;
}

void scene_capture_probe_enum_sources(void *data, obs_source_enum_proc_t callback, void *param)
{
	auto *probe = static_cast<SceneCaptureProbe *>(data);
	std::scoped_lock lock(probe->mutex);
	if (probe->input_scene)
		callback(probe->source, probe->input_scene, param);
}

uint32_t scene_capture_probe_width(void *data)
{
	auto *input_scene = input_scene_ref(static_cast<SceneCaptureProbe *>(data));
	if (!input_scene)
		return 0;
	const auto width = obs_source_get_width(input_scene);
	obs_source_release(input_scene);
	return width;
}

uint32_t scene_capture_probe_height(void *data)
{
	auto *input_scene = input_scene_ref(static_cast<SceneCaptureProbe *>(data));
	if (!input_scene)
		return 0;
	const auto height = obs_source_get_height(input_scene);
	obs_source_release(input_scene);
	return height;
}

bool add_scene_to_list(void *param, obs_source_t *scene_source)
{
	auto *property = static_cast<obs_property_t *>(param);
	const auto *name = obs_source_get_name(scene_source);
	obs_property_list_add_string(property, name, name);
	return true;
}

obs_properties_t *scene_capture_probe_properties(void *)
{
	auto *properties = obs_properties_create();
	auto *input_scene = obs_properties_add_list(properties, kInputSceneSetting, "Delay Input scene",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(input_scene, "Select a scene", "");
	obs_enum_scenes(add_scene_to_list, input_scene);
	return properties;
}

const char *scene_capture_probe_name(void *)
{
	return "Active Live Delay: P4 Scene Capture Probe";
}

const obs_source_info scene_capture_probe_info = {
	.id = "active_delay_scene_capture_probe",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = scene_capture_probe_name,
	.create = scene_capture_probe_create,
	.destroy = scene_capture_probe_destroy,
	.get_width = scene_capture_probe_width,
	.get_height = scene_capture_probe_height,
	.get_properties = scene_capture_probe_properties,
	.update = scene_capture_probe_update,
	.video_render = scene_capture_probe_render,
	.enum_active_sources = scene_capture_probe_enum_sources,
	.save = scene_capture_probe_save,
	.load = scene_capture_probe_load,
	.audio_render = scene_capture_probe_audio_render,
	.enum_all_sources = scene_capture_probe_enum_sources,
};
} // namespace

void register_scene_capture_probe()
{
	obs_register_source(&scene_capture_probe_info);
	blog(LOG_INFO, "[active-live-delay] P4 isolated scene-capture probe registered");
}

} // namespace active_delay
