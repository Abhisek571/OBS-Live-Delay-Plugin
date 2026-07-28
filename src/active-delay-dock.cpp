#include "active-delay-dock.hpp"
#include "active-delay-output.hpp"
#include "diagnostic-error.hpp"
#include "delayed-output-watchdog.hpp"
#include "multistream-preflight.hpp"
#include "scene-switch-policy.hpp"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
}

#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <utility>

namespace active_delay {
namespace {
constexpr const char *kMultistreamSection = "ActiveLiveDelayMultistream";

QString locale_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *safe_config_string(config_t *profile, const std::string &key)
{
	const auto *value = config_get_string(profile, kMultistreamSection, key.c_str());
	return value ? value : "";
}

DestinationCardText destination_card_text()
{
	return {locale_text("Multistream.Enabled"), locale_text("Multistream.Platform"),
		locale_text("Multistream.DisplayName"), locale_text("Multistream.ServerUrl"),
		locale_text("Multistream.StreamKey"), locale_text("Multistream.HoldReveal"),
		{locale_text("Multistream.Platform.Custom"), locale_text("Multistream.Platform.Twitch"),
			locale_text("Multistream.Platform.YouTube"), locale_text("Multistream.Platform.Kick")},
		{locale_text("Multistream.Guidance.Custom"), locale_text("Multistream.Guidance.Twitch"),
			locale_text("Multistream.Guidance.YouTube"), locale_text("Multistream.Guidance.Kick")},
		{locale_text("Multistream.Placeholder.Custom"), locale_text("Multistream.Placeholder.Twitch"),
			locale_text("Multistream.Placeholder.YouTube"), locale_text("Multistream.Placeholder.Kick")},
		locale_text("Multistream.Placeholder.Key")};
}

QString format_bytes(std::uint64_t bytes)
{
	if (bytes >= 1024 * 1024)
		return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MiB";
	if (bytes >= 1024)
		return QString::number(bytes / 1024.0, 'f', 1) + " KiB";
	return QString::number(bytes) + " B";
}

QString diagnostic_prefix(const std::string &error)
{
	const auto end = error.find(']');
	if (!error.starts_with("[ALD-E") || end == std::string::npos)
		return {};
	return QString::fromStdString(error.substr(0, end + 1));
}

QString preflight_issue_text(PreflightIssueKind kind)
{
	switch (kind) {
	case PreflightIssueKind::H264Required: return locale_text("Multistream.Preflight.H264");
	case PreflightIssueKind::AacRequired: return locale_text("Multistream.Preflight.AAC");
	case PreflightIssueKind::CbrRequired: return locale_text("Multistream.Preflight.CBR");
	case PreflightIssueKind::Maximum1080p: return locale_text("Multistream.Preflight.Maximum1080p");
	case PreflightIssueKind::Maximum60Fps: return locale_text("Multistream.Preflight.Maximum60Fps");
	case PreflightIssueKind::Maximum8000Kbps: return locale_text("Multistream.Preflight.Maximum8000Kbps");
	case PreflightIssueKind::TwoSecondKeyframesRequired:
		return locale_text("Multistream.Preflight.TwoSecondKeyframesRequired");
	case PreflightIssueKind::MaximumFourSecondKeyframes:
		return locale_text("Multistream.Preflight.MaximumFourSecondKeyframes");
	case PreflightIssueKind::TwoSecondKeyframesRecommended:
		return locale_text("Multistream.Preflight.TwoSecondKeyframesRecommended");
	case PreflightIssueKind::TwitchKeyframeGuidance:
		return locale_text("Multistream.Preflight.TwitchKeyframeGuidance");
	}
	return locale_text("Multistream.Status.Unknown");
}

bool encoder_matches(const char *id, obs_encoder_type type, const char *codec)
{
	const auto *actual_codec = id ? obs_get_encoder_codec(id) : nullptr;
	return actual_codec && obs_get_encoder_type(id) == type && std::strcmp(actual_codec, codec) == 0;
}

const char *first_matching_encoder(obs_encoder_type type, const char *codec)
{
	const char *id = nullptr;
	for (std::size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
		if (encoder_matches(id, type, codec))
			return id;
	}
	return nullptr;
}

const char *simple_h264_encoder(const char *selection)
{
	if (!selection)
		return nullptr;
	if (std::strcmp(selection, "nvenc") == 0) {
		if (encoder_matches("obs_nvenc_h264_tex", OBS_ENCODER_VIDEO, "h264"))
			return "obs_nvenc_h264_tex";
		if (encoder_matches("ffmpeg_nvenc", OBS_ENCODER_VIDEO, "h264"))
			return "ffmpeg_nvenc";
		return nullptr;
	}
	if (std::strcmp(selection, "qsv") == 0)
		return encoder_matches("obs_qsv11_v2", OBS_ENCODER_VIDEO, "h264") ? "obs_qsv11_v2" : nullptr;
	if (std::strcmp(selection, "amd") == 0)
		return encoder_matches("h264_texture_amf", OBS_ENCODER_VIDEO, "h264") ? "h264_texture_amf" : nullptr;
	if (std::strcmp(selection, "x264") == 0 || std::strcmp(selection, "x264_lowcpu") == 0)
		return encoder_matches("obs_x264", OBS_ENCODER_VIDEO, "h264") ? "obs_x264" : nullptr;
	return encoder_matches(selection, OBS_ENCODER_VIDEO, "h264") ? selection : nullptr;
}

const char *simple_preset_key(const char *selection)
{
	if (std::strcmp(selection, "nvenc") == 0)
		return "NVENCPreset2";
	if (std::strcmp(selection, "qsv") == 0)
		return "QSVPreset";
	if (std::strcmp(selection, "amd") == 0)
		return "AMDPreset";
	return "Preset";
}

QString state_text(DelayState state)
{
	switch (state) {
	case DelayState::Live: return "● " + locale_text("Delay.Status.Live");
	case DelayState::BuildingDelay: return "● " + locale_text("Delay.Status.Building");
	case DelayState::Delayed: return "● " + locale_text("Delay.Status.Delayed");
	case DelayState::ReturningLive: return "● " + locale_text("Delay.Status.ReturningLive");
	case DelayState::Error: return "● " + locale_text("Common.Status.Error");
	}
	return "● " + locale_text("Common.Status.Unknown");
}

const char *state_colour(DelayState state)
{
	switch (state) {
	case DelayState::Live:
	case DelayState::Delayed: return "#39b54a";
	case DelayState::BuildingDelay:
	case DelayState::ReturningLive: return "#e0a800";
	case DelayState::Error: return "#e5534b";
	}
	return "#8a8f98";
}

void show_state(QLabel *label, const QString &text, const char *colour)
{
	label->setText(text);
	label->setStyleSheet("font-size: 14px; font-weight: 600; color: " + QString::fromUtf8(colour) + ";");
}

QString dock_error(DiagnosticCode code, const QString &detail)
{
	return QString::fromStdString(diagnostic_error(code, detail.toStdString()));
}

QString sender_state_text(SenderState state)
{
	switch (state) {
	case SenderState::Stopped: return locale_text("Multistream.Status.Stopped");
	case SenderState::Starting: return locale_text("Multistream.Status.Connecting");
	case SenderState::Running: return locale_text("Multistream.Status.Active");
	case SenderState::Reconnecting: return locale_text("Multistream.Status.Reconnecting");
	case SenderState::Stopping: return locale_text("Multistream.Status.Stopping");
	case SenderState::Failed: return locale_text("Multistream.Status.Failed");
	}
	return locale_text("Multistream.Status.Unknown");
}

} // namespace

ActiveDelayDock::ActiveDelayDock(std::shared_ptr<ActiveDelaySession> session, QWidget *parent)
	: QWidget(parent), session_(std::move(session))
{
	auto *layout = new QVBoxLayout(this);
	status_ = new QLabel(this);
	status_->setObjectName("ald_delay_status");
	status_->setWordWrap(true);
	output_status_ = new QLabel(this);
	output_status_->setObjectName("ald_broadcast_status");
	output_status_->setWordWrap(true);
	current_delay_ = new QLabel(this);
	target_status_ = new QLabel(this);
	holding_scene_ = new QComboBox(this);
	holding_scene_->setObjectName("ald_holding_scene");
	target_seconds_ = new QSpinBox(this);
	target_seconds_->setObjectName("ald_delay_seconds");
	target_seconds_->setRange(1, 600);
	target_seconds_->setValue(30);

	auto *guidance = new QLabel(locale_text("Broadcast.Guidance"), this);
	guidance->setObjectName("ald_broadcast_guidance");
	guidance->setWordWrap(true);
	layout->addWidget(guidance);

	start_output_button_ = new QPushButton(locale_text("Broadcast.Start"), this);
	start_output_button_->setObjectName("ald_start_broadcast");
	start_output_button_->setToolTip(locale_text("Broadcast.StartHelp"));
	stop_output_button_ = new QPushButton(locale_text("Broadcast.End"), this);
	stop_output_button_->setObjectName("ald_end_broadcast");
	stop_output_button_->setToolTip(locale_text("Broadcast.EndHelp"));
	auto *broadcast_group = new QGroupBox(locale_text("Broadcast.Group"), this);
	broadcast_group->setObjectName("ald_broadcast_group");
	auto *broadcast_layout = new QVBoxLayout(broadcast_group);
	broadcast_layout->addWidget(output_status_);
	auto *broadcast_buttons = new QHBoxLayout();
	broadcast_buttons->addWidget(start_output_button_);
	broadcast_buttons->addWidget(stop_output_button_);
	broadcast_layout->addLayout(broadcast_buttons);
	layout->addWidget(broadcast_group);

	enable_button_ = new QPushButton(locale_text("Delay.Start"), this);
	enable_button_->setObjectName("ald_start_delay");
	enable_button_->setToolTip(locale_text("Delay.StartHelp"));
	return_live_button_ = new QPushButton(locale_text("Delay.ReturnLive"), this);
	return_live_button_->setObjectName("ald_return_live");
	return_live_button_->setToolTip(locale_text("Delay.ReturnLiveHelp"));
	auto *delay_group = new QGroupBox(locale_text("Delay.Group"), this);
	delay_group->setObjectName("ald_delay_group");
	auto *delay_layout = new QVBoxLayout(delay_group);
	auto *delay_form = new QFormLayout();
	delay_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	delay_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	delay_form->addRow(locale_text("Delay.State"), status_);
	delay_form->addRow(locale_text("Delay.Current"), current_delay_);
	delay_form->addRow(locale_text("Delay.LengthSeconds"), target_seconds_);
	delay_form->addRow(locale_text("Delay.HoldingScene"), holding_scene_);
	delay_layout->addLayout(delay_form);
	delay_layout->addWidget(enable_button_);
	delay_layout->addWidget(return_live_button_);
	layout->addWidget(delay_group);

	auto *destination_group = new QGroupBox(locale_text("Multistream.Targets"), this);
	destination_group->setObjectName("ald_destinations_group");
	auto *destination_group_layout = new QVBoxLayout(destination_group);
	auto *destination_summary = new QFormLayout();
	destination_summary->addRow(locale_text("Multistream.EnabledDestinationsTitle"), target_status_);
	destination_group_layout->addLayout(destination_summary);
	auto *warning = new QLabel(locale_text("Multistream.ExperimentalWarning"), destination_group);
	warning->setWordWrap(true);
	preflight_status_ = new QLabel(this);
	preflight_status_->setObjectName("ald_multistream_preflight");
	preflight_status_->setWordWrap(true);
	destination_group_layout->addWidget(warning);
	destination_group_layout->addWidget(preflight_status_);

	auto *destination_container = new QWidget(this);
	auto *destination_layout = new QVBoxLayout(destination_container);
	destination_layout->setContentsMargins(0, 0, 0, 0);
	destination_layout->setSpacing(6);
	auto *primary_card = new QGroupBox(locale_text("Multistream.PrimaryTitle"), destination_container);
	primary_card->setObjectName("ald_destination_primary");
	auto *primary_layout = new QVBoxLayout(primary_card);
	primary_target_status_ = new QLabel(locale_text("Multistream.Status.Stopped"), primary_card);
	primary_target_status_->setObjectName("ald_primary_status");
	primary_target_status_->setWordWrap(true);
	auto *primary_guidance = new QLabel(locale_text("Multistream.PrimaryGuidance"), primary_card);
	primary_guidance->setWordWrap(true);
	primary_layout->addWidget(primary_target_status_);
	primary_layout->addWidget(primary_guidance);
	destination_layout->addWidget(primary_card);
	secondary_cards_[0] = new DestinationCardWidget("secondary_2", locale_text("Multistream.Secondary2Title"),
		destination_card_text(), destination_container);
	secondary_cards_[1] = new DestinationCardWidget("secondary_3", locale_text("Multistream.Secondary3Title"),
		destination_card_text(), destination_container);
	destination_layout->addWidget(secondary_cards_[0]);
	destination_layout->addWidget(secondary_cards_[1]);

	auto *destination_scroll = new QScrollArea(this);
	destination_scroll->setObjectName("ald_destination_scroll");
	destination_scroll->setWidgetResizable(true);
	destination_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	destination_scroll->setWidget(destination_container);
	destination_group_layout->addWidget(destination_scroll, 1);
	layout->addWidget(destination_group, 1);

	refresh_scenes();
	load_multistream_settings();
	connect(start_output_button_, &QPushButton::clicked, this, &ActiveDelayDock::start_delayed_output);
	connect(stop_output_button_, &QPushButton::clicked, this, &ActiveDelayDock::stop_delayed_output);
	connect(enable_button_, &QPushButton::clicked, this, &ActiveDelayDock::enable_delay);
	connect(return_live_button_, &QPushButton::clicked, this, &ActiveDelayDock::return_live);
	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, &ActiveDelayDock::refresh_status);
	timer_->start();
	refresh_status();
}

ActiveDelayDock::~ActiveDelayDock()
{
	shutdown();
	release_scene_switch_refs();
}

void ActiveDelayDock::enable_delay()
{
	std::string error;
	const auto target = std::chrono::seconds(target_seconds_->value());
	if (delayed_output_ && obs_output_active(delayed_output_)) {
		const auto state = session_->controller.delay.status().state;
		if (state == DelayState::BuildingDelay || state == DelayState::Delayed) {
			report_operational_error(dock_error(DiagnosticCode::DelayControlUnavailable,
				"Delay is already active; use Return Live before choosing a new delay length"),
				LOG_WARNING);
			return;
		}
		QString scene_error;
		if (!switch_to_holding_scene(scene_error)) {
			report_operational_error(scene_error, LOG_WARNING);
			return;
		}
		if (!session_->controller.delay.set_target(target, &error)) {
			restore_program_scene();
			report_operational_error(QString::fromStdString(error), LOG_WARNING);
			return;
		}
		persistent_output_error_.clear();
		return;
	}

	if (obs_frontend_streaming_active()) {
		report_operational_error(dock_error(DiagnosticCode::OutputControlConflict,
			"Normal OBS streaming is active; stop it, then use Start Broadcast in this dock"),
			LOG_WARNING);
	} else {
		report_operational_error(dock_error(DiagnosticCode::DelayControlUnavailable,
			"Start Broadcast before starting the delay"),
			LOG_WARNING);
	}
}

void ActiveDelayDock::return_live()
{
	if (!delayed_output_ || !obs_output_active(delayed_output_)) {
		report_operational_error(dock_error(DiagnosticCode::DelayControlUnavailable,
			"Return Live is available only while this dock is broadcasting"),
			LOG_WARNING);
		return;
	}
	session_->controller.delay.return_live();
	persistent_output_error_.clear();
	restore_program_scene();
}

void ActiveDelayDock::refresh_scenes()
{
	const auto selected_name = active_holding_scene_
		? QString::fromUtf8(obs_source_get_name(active_holding_scene_))
		: holding_scene_->currentText();
	holding_scene_->clear();
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i)
		holding_scene_->addItem(QString::fromUtf8(obs_source_get_name(scenes.sources.array[i])));
	obs_frontend_source_list_free(&scenes);
	const auto selected_index = holding_scene_->findText(selected_name);
	if (selected_index >= 0) {
		holding_scene_->setCurrentIndex(selected_index);
		return;
	}

	auto *program_scene = obs_frontend_get_current_scene();
	for (int index = 0; index < holding_scene_->count(); ++index) {
		auto *candidate = obs_get_source_by_name(holding_scene_->itemText(index).toUtf8().constData());
		const auto differs_from_program = candidate && candidate != program_scene;
		if (candidate)
			obs_source_release(candidate);
		if (differs_from_program) {
			holding_scene_->setCurrentIndex(index);
			break;
		}
	}
	if (program_scene)
		obs_source_release(program_scene);
}

void ActiveDelayDock::refresh_status()
{
	if (check_delayed_output_health())
		return;
	if (check_scene_switch_health())
		return;

	const auto value = session_->controller.delay.status();
	show_state(status_, state_text(value.state), state_colour(value.state));
	current_delay_->setText(QString::number(value.current_delay.count() / 1'000'000.0, 'f', 1) + " sec");
	if (!value.error.empty()) {
		show_state(status_, QString::fromStdString(value.error), "#e5534b");
	} else if (!persistent_output_error_.isEmpty()) {
		show_state(status_, persistent_output_error_, "#e5534b");
	}
	const bool delay_active = value.state == DelayState::BuildingDelay || value.state == DelayState::Delayed;
	enable_button_->setEnabled(!delay_active);
	target_seconds_->setEnabled(!delay_active);
	const bool destination_editable = output_flow_state_ == OutputFlowState::Stopped &&
		(!delayed_output_ || !obs_output_active(delayed_output_));
	for (auto *card : secondary_cards_) {
		card->set_editable(destination_editable);
		card->refresh_secret_mask();
	}
	refresh_target_status();
	refresh_preflight_summary();

	if (delayed_output_ && obs_output_active(delayed_output_)) {
		show_state(output_status_, "● " + locale_text("Broadcast.Status.Active"), "#39b54a");
	} else {
		if (delayed_output_) {
			const auto *last_error = obs_output_get_last_error(delayed_output_);
			if (last_error && *last_error)
				persistent_output_error_ = dock_error(DiagnosticCode::OutputStoppedUnexpectedly,
					QString::fromUtf8(last_error));
		}
		if (persistent_output_error_.isEmpty())
			show_state(output_status_, "● " + locale_text("Broadcast.Status.Stopped"), "#8a8f98");
		else
			show_state(output_status_, "● " + locale_text("Common.Status.Error") + ": " +
				persistent_output_error_, "#e5534b");
	}
	const auto output_active = delayed_output_ && obs_output_active(delayed_output_);
	start_output_button_->setEnabled(!output_active && output_flow_state_ == OutputFlowState::Stopped);
	stop_output_button_->setEnabled(output_active);
	return_live_button_->setEnabled(output_active);

	if (value.state == DelayState::Delayed && output_flow_state_ == OutputFlowState::DelayedOutput)
		restore_program_scene();
}

bool ActiveDelayDock::check_scene_switch_health()
{
	if (scene_switch_action_in_progress_)
		return false;
	const auto transition_active = original_scene_ || active_holding_scene_;
	if (!transition_active)
		return false;

	auto *current_scene = obs_frontend_get_current_scene();
	const auto delay_building = session_->controller.delay.status().state == DelayState::BuildingDelay;
	const auto program_available = original_scene_ && !obs_source_removed(original_scene_) &&
		obs_scene_from_source(original_scene_);
	const auto holding_available = active_holding_scene_ && !obs_source_removed(active_holding_scene_) &&
		obs_scene_from_source(active_holding_scene_);
	const auto health = evaluate_scene_switch_health(true, delay_building, program_available, holding_available,
		current_scene == active_holding_scene_);
	if (current_scene)
		obs_source_release(current_scene);
	if (health == SceneSwitchHealth::Healthy || health == SceneSwitchHealth::Inactive)
		return false;

	session_->controller.delay.return_live();
	switch (health) {
	case SceneSwitchHealth::HoldingUnavailable:
		persistent_output_error_ = dock_error(DiagnosticCode::HoldingSceneInvalid,
			"The selected Holding Scene was removed while delay was building; delay returned live");
		restore_program_scene();
		break;
	case SceneSwitchHealth::HoldingInterrupted:
		persistent_output_error_ = dock_error(DiagnosticCode::HoldingSceneInterrupted,
			"The Program Scene changed before delay finished building; delay returned live");
		release_scene_switch_refs();
		break;
	case SceneSwitchHealth::ProgramUnavailable:
		persistent_output_error_ = dock_error(DiagnosticCode::ProgramSceneUnavailable,
			"The saved Program Scene was removed while delay was building; delay returned live");
		release_scene_switch_refs();
		break;
	case SceneSwitchHealth::Inactive:
	case SceneSwitchHealth::Healthy: return false;
	}
	blog(LOG_WARNING, "[active-live-delay] %s", persistent_output_error_.toUtf8().constData());
	show_state(status_, persistent_output_error_, "#e5534b");
	return true;
}

void ActiveDelayDock::handle_frontend_event(obs_frontend_event event)
{
	if (scene_switch_action_in_progress_)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		check_scene_switch_health();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED:
		refresh_scenes();
		check_scene_switch_health();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		if (original_scene_ || active_holding_scene_) {
			session_->controller.delay.return_live();
			persistent_output_error_ = dock_error(DiagnosticCode::ProgramSceneUnavailable,
				"The scene collection changed while delay was building; delay returned live");
			release_scene_switch_refs();
			blog(LOG_WARNING, "[active-live-delay] %s", persistent_output_error_.toUtf8().constData());
			show_state(status_, persistent_output_error_, "#e5534b");
		}
		break;
	default: break;
	}
}

void ActiveDelayDock::load_multistream_settings()
{
	auto *profile = obs_frontend_get_profile_config();
	if (!profile) {
		report_operational_error(dock_error(DiagnosticCode::DirectProfileUnavailable,
			"OBS did not expose the active profile for destination settings"),
			LOG_WARNING);
		return;
	}
	const auto version = config_get_uint(profile, kMultistreamSection, "Version");
	if (version > MultistreamConfiguration::kCurrentVersion) {
		report_operational_error(dock_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"The saved Native Multistream settings use an unsupported format"),
			LOG_WARNING);
		return;
	}

	MultistreamConfiguration configuration;
	if (version <= 1) {
		LegacyMultistreamSettingsV1 legacy;
		legacy.enabled = config_get_bool(profile, kMultistreamSection, "SecondaryEnabled");
		legacy.name = safe_config_string(profile, "SecondaryName");
		legacy.target.server_url = safe_config_string(profile, "SecondaryServer");
		legacy.target.stream_key = safe_config_string(profile, "SecondaryStreamKey");
		configuration = migrate_multistream_v1(legacy);
		if (version == 1 || legacy.enabled || !legacy.name.empty() || !legacy.target.server_url.empty() ||
			!legacy.target.stream_key.empty())
		{
			QString save_error;
			if (!save_multistream_settings(configuration, save_error))
				report_operational_error(save_error, LOG_WARNING);
		}
	} else {
		for (const auto slot : {2, 3}) {
			const auto prefix = "Destination" + std::to_string(slot);
			DestinationPlatform platform = DestinationPlatform::CustomRtmp;
			const auto platform_value = std::string(safe_config_string(profile, prefix + "Platform"));
			if (!platform_value.empty() && !parse_destination_platform(platform_value, platform)) {
				report_operational_error(dock_error(DiagnosticCode::MultistreamConfigurationInvalid,
					"A saved destination has an unsupported platform selection"),
					LOG_WARNING);
				platform = DestinationPlatform::CustomRtmp;
			}
			configuration.secondary_destinations.push_back({"secondary_" + std::to_string(slot),
				safe_config_string(profile, prefix + "Name"),
				{safe_config_string(profile, prefix + "Server"), safe_config_string(profile, prefix + "StreamKey")},
				platform, config_get_bool(profile, kMultistreamSection, (prefix + "Enabled").c_str())});
		}
	}
	while (configuration.secondary_destinations.size() < secondary_cards_.size()) {
		const auto slot = configuration.secondary_destinations.size() + 2;
		configuration.secondary_destinations.push_back(
			{"secondary_" + std::to_string(slot), "", {}, DestinationPlatform::CustomRtmp, false});
	}
	for (std::size_t index = 0; index < secondary_cards_.size(); ++index)
		secondary_cards_[index]->set_destination(configuration.secondary_destinations[index]);
}

bool ActiveDelayDock::save_multistream_settings(const MultistreamConfiguration &configuration, QString &error)
{
	auto *profile = obs_frontend_get_profile_config();
	if (!profile) {
		error = dock_error(DiagnosticCode::DirectProfileUnavailable,
			"OBS did not expose the active profile for destination settings");
		return false;
	}
	config_set_uint(profile, kMultistreamSection, "Version", MultistreamConfiguration::kCurrentVersion);
	for (const auto &destination : configuration.secondary_destinations) {
		const auto slot = destination.id == "secondary_3" ? 3 : 2;
		const auto prefix = "Destination" + std::to_string(slot);
		config_set_bool(profile, kMultistreamSection, (prefix + "Enabled").c_str(), destination.enabled);
		config_set_string(profile, kMultistreamSection, (prefix + "Platform").c_str(),
			destination_platform_id(destination.platform).data());
		config_set_string(profile, kMultistreamSection, (prefix + "Name").c_str(), destination.name.c_str());
		config_set_string(profile, kMultistreamSection, (prefix + "Server").c_str(),
			destination.target.server_url.c_str());
		config_set_string(profile, kMultistreamSection, (prefix + "StreamKey").c_str(),
			destination.target.stream_key.c_str());
	}
	if (config_save(profile) != CONFIG_SUCCESS) {
		error = dock_error(DiagnosticCode::SettingsSaveFailed,
			"OBS could not save the Active Live Delay destination settings to the active profile");
		return false;
	}
	return true;
}

MultistreamConfiguration ActiveDelayDock::multistream_configuration_from_cards() const
{
	MultistreamConfiguration configuration;
	for (const auto *card : secondary_cards_)
		configuration.secondary_destinations.push_back(card->destination());
	return configuration;
}

bool ActiveDelayDock::configure_multistream_mode(QString &error)
{
	const auto configuration = multistream_configuration_from_cards();
	const auto enabled = std::any_of(configuration.secondary_destinations.begin(),
		configuration.secondary_destinations.end(), [](const MultistreamDestination &destination) {
			return destination.enabled;
		});
	if (!enabled) {
		std::string mode_error;
		if (!session_->set_operating_mode(OperatingMode::DirectSingle, mode_error)) {
			error = dock_error(DiagnosticCode::OperatingModeConflict, QString::fromStdString(mode_error));
			return false;
		}
		session_->multistream.set({});
		if (!save_multistream_settings(configuration, error))
			return false;
		return true;
	}

	std::string validation_error;
	if (!validate_multistream_configuration(configuration, validation_error)) {
		error = dock_error(DiagnosticCode::MultistreamConfigurationInvalid,
			QString::fromStdString(validation_error));
		return false;
	}
	std::string mode_error;
	if (!session_->set_operating_mode(OperatingMode::NativeMultistream, mode_error)) {
		error = dock_error(DiagnosticCode::OperatingModeConflict, QString::fromStdString(mode_error));
		return false;
	}
	session_->multistream.set(configuration);
	if (!save_multistream_settings(configuration, error))
		return false;
	return true;
}

void ActiveDelayDock::refresh_target_status()
{
	const auto configuration = multistream_configuration_from_cards();
	const auto enabled_secondaries = std::count_if(configuration.secondary_destinations.begin(),
		configuration.secondary_destinations.end(), [](const MultistreamDestination &destination) {
			return destination.enabled;
		});
	target_status_->setText(QString::number(enabled_secondaries + 1) + " / 3 " +
		locale_text("Multistream.EnabledDestinations"));
	const auto status = session_->multistream.status_snapshot();
	if (status.destinations.empty()) {
		primary_target_status_->setText(locale_text("Multistream.Status.Stopped"));
		for (std::size_t index = 0; index < secondary_cards_.size(); ++index) {
			const auto &destination = configuration.secondary_destinations[index];
			secondary_cards_[index]->set_status_text(destination.enabled
				? locale_text("Multistream.Status.Configured")
				: locale_text("Multistream.Status.Disabled"));
		}
		return;
	}
	for (const auto &destination : status.destinations) {
		auto row = QString::fromStdString(destination.name) + " | " + sender_state_text(destination.sender.state) +
			" | " + locale_text("Multistream.Reconnects") + " " +
			QString::number(destination.sender.reconnect_count) + " | " + locale_text("Multistream.Queue") + " " +
			QString::number(destination.sender.queued_tags) + " / " + format_bytes(destination.sender.queued_bytes) +
			" | " + locale_text("Multistream.Sent") + " " + format_bytes(destination.sender.sent_bytes);
		const auto error_code = diagnostic_prefix(destination.sender.error);
		if (!error_code.isEmpty())
			row += " | " + locale_text("Multistream.Error") + " " + error_code;
		if (destination.primary)
			primary_target_status_->setText(row);
		else if (destination.id == "secondary_2")
			secondary_cards_[0]->set_status_text(row);
		else if (destination.id == "secondary_3")
			secondary_cards_[1]->set_status_text(row);
	}
}

void ActiveDelayDock::refresh_preflight_summary()
{
	auto *profile = obs_frontend_get_profile_config();
	if (!profile)
		return;
	obs_video_info video_info = {};
	obs_get_video_info(&video_info);
	const StreamRendition rendition{"h264", "aac", "CBR", video_info.output_width, video_info.output_height,
		video_info.fps_den == 0 ? 0.0 : static_cast<double>(video_info.fps_num) / video_info.fps_den,
		static_cast<std::uint32_t>(config_get_uint(profile, "SimpleOutput", "VBitrate")),
		static_cast<std::uint32_t>(config_get_uint(profile, "SimpleOutput", "ABitrate")), 2};
	const auto preflight = evaluate_multistream_preflight(multistream_configuration_from_cards(), rendition);
	QString summary = locale_text("Multistream.Preflight") + ": " +
		QString::number(preflight.enabled_destination_count) + " " + locale_text("Multistream.Destinations") +
		", ~" + QString::number(preflight.estimated_upload_kbps) + " kbps";
	for (const auto &issue : preflight.issues)
		summary += "\n" + QString::fromStdString(std::string(destination_platform_name(issue.platform))) + ": " +
			preflight_issue_text(issue.kind);
	preflight_status_->setText(summary);
}

bool ActiveDelayDock::validate_multistream_preflight(config_t *profile, obs_data_t *video_settings, QString &error)
{
	if (session_->operating_mode() != OperatingMode::NativeMultistream)
		return true;
	obs_video_info video_info = {};
	obs_get_video_info(&video_info);
	const StreamRendition rendition{"h264", "aac", "CBR", video_info.output_width, video_info.output_height,
		video_info.fps_den == 0 ? 0.0 : static_cast<double>(video_info.fps_num) / video_info.fps_den,
		static_cast<std::uint32_t>(config_get_uint(profile, "SimpleOutput", "VBitrate")),
		static_cast<std::uint32_t>(config_get_uint(profile, "SimpleOutput", "ABitrate")),
		static_cast<std::uint32_t>(obs_data_get_int(video_settings, "keyint_sec"))};
	const auto preflight = evaluate_multistream_preflight(session_->multistream.snapshot(), rendition);
	QStringList issues;
	for (const auto &issue : preflight.issues) {
		issues.push_back(QString::fromStdString(std::string(destination_platform_name(issue.platform))) + ": " +
			preflight_issue_text(issue.kind));
	}
	preflight_status_->setText(locale_text("Multistream.Preflight") + ": " +
		QString::number(preflight.enabled_destination_count) + " " + locale_text("Multistream.Destinations") +
		", ~" + QString::number(preflight.estimated_upload_kbps) + " kbps" +
		(issues.empty() ? QString{} : "\n" + issues.join("\n")));
	if (preflight.can_start())
		return true;
	error = dock_error(DiagnosticCode::MultistreamPreflightFailed,
		locale_text("Multistream.PreflightBlocked") + ": " + issues.join("; "));
	return false;
}

bool ActiveDelayDock::start_delayed_output_with(obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder,
	obs_service_t *service, QString &error)
{
	if (!video_encoder || !audio_encoder) {
		error = dock_error(DiagnosticCode::DirectEncoderCreationFailed,
			"The delayed output needs H.264 video and AAC audio encoders");
		return false;
	}
	if (!service) {
		error = dock_error(DiagnosticCode::DirectServiceMissing, "No OBS streaming service is configured");
		return false;
	}

	auto *output = obs_output_create("active_delay_rtmp_output", "active_delay_stream", nullptr, nullptr);
	if (!output) {
		error = dock_error(DiagnosticCode::DirectOutputCreationFailed,
			"OBS could not create the Active Live Delay output");
		return false;
	}
	obs_output_set_video_encoder(output, video_encoder);
	obs_output_set_audio_encoder(output, audio_encoder, 0);
	obs_output_set_service(output, service);
	if (obs_output_get_service(output) != service) {
		error = dock_error(DiagnosticCode::DirectServiceAttachFailed,
			"OBS could not attach the configured streaming service to Active Live Delay");
		obs_output_release(output);
		return false;
	}
	if (!obs_output_start(output)) {
		const auto *last_error = obs_output_get_last_error(output);
		error = dock_error(DiagnosticCode::DirectOutputStartFailed,
			last_error && *last_error ? QString::fromUtf8(last_error) : "The Active Live Delay output failed to start");
		obs_output_release(output);
		return false;
	}
	delayed_output_ = output;
	output_flow_state_ = OutputFlowState::DelayedOutput;
	delayed_output_started_at_ = std::chrono::steady_clock::now();
	persistent_output_error_.clear();
	blog(LOG_INFO, "[active-live-delay] Delayed RTMP output started successfully");
	return true;
}

bool ActiveDelayDock::start_delayed_output_direct(QString &error)
{
	auto *profile = obs_frontend_get_profile_config();
	if (!profile) {
		error = dock_error(DiagnosticCode::DirectProfileUnavailable, "OBS did not expose the active output profile");
		return false;
	}
	const auto *mode = config_get_string(profile, "Output", "Mode");
	if (!mode || std::strcmp(mode, "Simple") != 0) {
		error = dock_error(DiagnosticCode::DirectOutputModeUnsupported,
			"Direct delayed output currently requires OBS Output Mode: Simple");
		return false;
	}

	auto *service = obs_frontend_get_streaming_service();
	if (!service) {
		error = dock_error(DiagnosticCode::DirectServiceMissing, "No OBS streaming service is configured");
		return false;
	}
	const auto *video_selection = config_get_string(profile, "SimpleOutput", "StreamEncoder");
	const auto *audio_selection = config_get_string(profile, "SimpleOutput", "StreamAudioEncoder");
	const auto *video_id = simple_h264_encoder(video_selection);
	if (!video_id) {
		error = dock_error(DiagnosticCode::DirectVideoEncoderUnavailable,
			"The selected Simple Output video encoder is not an available H.264 encoder");
		return false;
	}
	if (!audio_selection || std::strcmp(audio_selection, "aac") != 0) {
		error = dock_error(DiagnosticCode::DirectAudioEncoderUnsupported,
			"Direct delayed output requires AAC in OBS Simple Output settings");
		return false;
	}
	const char *audio_id = encoder_matches("ffmpeg_aac", OBS_ENCODER_AUDIO, "aac")
		? "ffmpeg_aac"
		: first_matching_encoder(OBS_ENCODER_AUDIO, "aac");
	if (!audio_id) {
		error = dock_error(DiagnosticCode::DirectAudioEncoderUnavailable, "No AAC encoder is available in OBS");
		return false;
	}

	auto *video_settings = obs_data_create();
	auto *audio_settings = obs_data_create();
	if (!video_settings || !audio_settings) {
		obs_data_release(video_settings);
		obs_data_release(audio_settings);
		error = dock_error(DiagnosticCode::DirectEncoderSettingsUnavailable,
			"OBS could not allocate direct-output encoder settings");
		return false;
	}
	const auto video_bitrate = config_get_uint(profile, "SimpleOutput", "VBitrate");
	const auto audio_bitrate = config_get_uint(profile, "SimpleOutput", "ABitrate");
	const auto *preset_key = simple_preset_key(video_selection);
	const auto *preset = config_get_string(profile, "SimpleOutput", preset_key);
	obs_data_set_string(video_settings,
		std::strncmp(video_id, "ffmpeg_", 7) == 0 && std::strcmp(preset_key, "NVENCPreset2") == 0
			? "preset2"
			: "preset",
		preset ? preset : "");
	obs_data_set_string(video_settings, "rate_control", "CBR");
	obs_data_set_int(video_settings, "bitrate", static_cast<long long>(video_bitrate));
	if (config_get_bool(profile, "SimpleOutput", "UseAdvanced")) {
		const auto *custom = config_get_string(profile, "SimpleOutput", "x264Settings");
		obs_data_set_string(video_settings, "x264opts", custom ? custom : "");
	}
	obs_data_set_string(audio_settings, "rate_control", "CBR");
	obs_data_set_int(audio_settings, "bitrate", static_cast<long long>(audio_bitrate));
	obs_service_apply_encoder_settings(service, video_settings, audio_settings);
	if (config_get_bool(profile, "Stream1", "IgnoreRecommended")) {
		obs_data_set_int(video_settings, "bitrate", static_cast<long long>(video_bitrate));
		obs_data_set_int(audio_settings, "bitrate", static_cast<long long>(audio_bitrate));
	}
	if (session_->operating_mode() == OperatingMode::NativeMultistream)
		obs_data_set_int(video_settings, "keyint_sec", 2);
	if (!validate_multistream_preflight(profile, video_settings, error)) {
		obs_data_release(video_settings);
		obs_data_release(audio_settings);
		return false;
	}

	auto *video_encoder = obs_video_encoder_create(video_id, "active_delay_direct_video", video_settings, nullptr);
	auto *audio_encoder = obs_audio_encoder_create(audio_id, "active_delay_direct_audio", audio_settings, 0, nullptr);
	obs_data_release(video_settings);
	obs_data_release(audio_settings);
	if (!video_encoder || !audio_encoder) {
		obs_encoder_release(video_encoder);
		obs_encoder_release(audio_encoder);
		error = dock_error(DiagnosticCode::DirectEncoderCreationFailed,
			"OBS could not create the direct H.264/AAC streaming encoders");
		return false;
	}
	obs_encoder_set_video(video_encoder, obs_get_video());
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	const auto started = start_delayed_output_with(video_encoder, audio_encoder, service, error);
	obs_encoder_release(video_encoder);
	obs_encoder_release(audio_encoder);
	if (started)
		blog(LOG_INFO, "[active-live-delay] Plugin-owned direct delayed output started");
	return started;
}

bool ActiveDelayDock::check_delayed_output_health()
{
	if (output_flow_state_ != OutputFlowState::DelayedOutput || !delayed_output_)
		return false;

	const auto active_for = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - delayed_output_started_at_);
	const auto health = evaluate_delayed_output_health(obs_output_active(delayed_output_),
		obs_output_get_total_frames(delayed_output_), active_for, std::chrono::seconds(5));
	if (health == DelayedOutputHealth::Progressing)
		return false;
	if (health == DelayedOutputHealth::WaitingForVideo)
		return false;

	QString error;
	if (health == DelayedOutputHealth::Stopped) {
		const auto *last_error = obs_output_get_last_error(delayed_output_);
		error = dock_error(DiagnosticCode::OutputStoppedUnexpectedly,
			last_error && *last_error ? QString::fromUtf8(last_error) : "The delayed output stopped unexpectedly");
	} else {
		error = dock_error(DiagnosticCode::OutputNoFrames,
			"The delayed output produced no encoded video frames within 5 seconds");
	}
	recover_from_delayed_output_failure(error);
	return true;
}

void ActiveDelayDock::recover_from_delayed_output_failure(const QString &error)
{
	if (output_flow_state_ != OutputFlowState::DelayedOutput)
		return;

	blog(LOG_ERROR, "[active-live-delay] Delayed output failed after startup: %s", error.toUtf8().constData());
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
	session_->controller.delay.return_live();
	persistent_output_error_ = error;
	restore_program_scene();
}

void ActiveDelayDock::start_delayed_output()
{
	if (delayed_output_ && obs_output_active(delayed_output_)) {
		report_operational_error(dock_error(DiagnosticCode::OutputControlConflict,
			"This dock is already broadcasting"),
			LOG_WARNING);
		return;
	}
	if (obs_frontend_streaming_active()) {
		report_operational_error(dock_error(DiagnosticCode::OutputControlConflict,
			"Normal OBS streaming is active; stop it, then use Start Broadcast in this dock"),
			LOG_WARNING);
		return;
	}

	if (delayed_output_) {
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	QString error;
	if (!configure_multistream_mode(error)) {
		report_operational_error(error, LOG_WARNING);
		return;
	}
	if (!start_delayed_output_direct(error))
		report_operational_error(error, LOG_ERROR);
}

void ActiveDelayDock::stop_delayed_output()
{
	if (delayed_output_ && obs_output_active(delayed_output_) &&
		QMessageBox::warning(this, locale_text("Multistream.StopConfirmTitle"),
			locale_text("Multistream.StopConfirmText"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
			QMessageBox::Yes)
		return;
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
	session_->controller.delay.return_live();
	persistent_output_error_.clear();
	restore_program_scene();
}

void ActiveDelayDock::shutdown()
{
	if (shutting_down_)
		return;
	shutting_down_ = true;
	if (timer_)
		timer_->stop();
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
}

bool ActiveDelayDock::switch_to_holding_scene(QString &error)
{
	if (original_scene_ || active_holding_scene_) {
		error = dock_error(DiagnosticCode::HoldingSceneInvalid,
			"A Holding Scene transition is already active");
		return false;
	}

	auto *program_scene = obs_frontend_get_current_scene();
	auto *holding_scene = obs_get_source_by_name(holding_scene_->currentText().toUtf8().constData());
	const auto program_available = program_scene && !obs_source_removed(program_scene) && obs_scene_from_source(program_scene);
	const auto holding_available = holding_scene && !obs_source_removed(holding_scene) && obs_scene_from_source(holding_scene);
	if (!program_available || !holding_available || program_scene == holding_scene) {
		if (program_scene)
			obs_source_release(program_scene);
		if (holding_scene)
			obs_source_release(holding_scene);
		error = dock_error(DiagnosticCode::HoldingSceneInvalid,
			"Select an existing Holding Scene that differs from the current Program Scene");
		return false;
	}

	original_scene_ = program_scene;
	active_holding_scene_ = holding_scene;
	scene_switch_action_in_progress_ = true;
	obs_frontend_set_current_scene(active_holding_scene_);
	scene_switch_action_in_progress_ = false;
	auto *selected_scene = obs_frontend_get_current_scene();
	const auto switched = selected_scene == active_holding_scene_;
	if (selected_scene)
		obs_source_release(selected_scene);
	if (!switched) {
		release_scene_switch_refs();
		error = dock_error(DiagnosticCode::HoldingSceneInvalid,
			"OBS did not activate the selected Holding Scene");
		return false;
	}
	return true;
}

void ActiveDelayDock::restore_program_scene()
{
	if (!original_scene_ && !active_holding_scene_)
		return;
	if (!original_scene_ || obs_source_removed(original_scene_) || !obs_scene_from_source(original_scene_)) {
		persistent_output_error_ = dock_error(DiagnosticCode::ProgramSceneUnavailable,
			"The saved Program Scene is no longer available; select the intended scene manually");
		blog(LOG_WARNING, "[active-live-delay] %s", persistent_output_error_.toUtf8().constData());
		release_scene_switch_refs();
		return;
	}

	scene_switch_action_in_progress_ = true;
	obs_frontend_set_current_scene(original_scene_);
	scene_switch_action_in_progress_ = false;
	auto *selected_scene = obs_frontend_get_current_scene();
	const auto restored = selected_scene == original_scene_;
	if (selected_scene)
		obs_source_release(selected_scene);
	if (!restored) {
		persistent_output_error_ = dock_error(DiagnosticCode::ProgramSceneUnavailable,
			"OBS did not restore the saved Program Scene; select it manually");
		blog(LOG_WARNING, "[active-live-delay] %s", persistent_output_error_.toUtf8().constData());
	}
	release_scene_switch_refs();
}

void ActiveDelayDock::release_scene_switch_refs()
{
	if (original_scene_) {
		obs_source_release(original_scene_);
		original_scene_ = nullptr;
	}
	if (active_holding_scene_) {
		obs_source_release(active_holding_scene_);
		active_holding_scene_ = nullptr;
	}
}

void ActiveDelayDock::report_operational_error(const QString &error, int log_level)
{
	persistent_output_error_ = error;
	show_state(status_, persistent_output_error_, "#e5534b");
	blog(log_level, "[active-live-delay] %s", persistent_output_error_.toUtf8().constData());
}

} // namespace active_delay
