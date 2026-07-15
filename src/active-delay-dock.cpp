#include "active-delay-dock.hpp"
#include "active-delay-output.hpp"
#include "delayed-output-watchdog.hpp"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <cstring>
#include <utility>

namespace active_delay {
namespace {
constexpr int kHandoffSettleDelayMs = 150;
constexpr int kHandoffRetryDelayMs = 250;
constexpr int kNormalRestartDelayMs = 150;
constexpr int kMaxEncoderStartAttempts = 3;

QString state_text(DelayState state)
{
	switch (state) {
	case DelayState::Live: return "● LIVE";
	case DelayState::BuildingDelay: return "● BUILDING DELAY";
	case DelayState::Delayed: return "● DELAYED";
	case DelayState::ReturningLive: return "● RETURNING LIVE";
	case DelayState::Error: return "● ERROR";
	}
	return "● UNKNOWN";
}

} // namespace

ActiveDelayDock::ActiveDelayDock(std::shared_ptr<ActiveDelaySession> session, QWidget *parent)
	: QWidget(parent), session_(std::move(session))
{
	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout();
	status_ = new QLabel(this);
	output_status_ = new QLabel(this);
	current_delay_ = new QLabel(this);
	holding_scene_ = new QComboBox(this);
	target_seconds_ = new QSpinBox(this);
	target_seconds_->setRange(1, 600);
	target_seconds_->setValue(30);
	dump_seconds_ = new QSpinBox(this);
	dump_seconds_->setRange(1, 120);
	dump_seconds_->setValue(10);
	form->addRow("Status", status_);
	form->addRow("Delayed Output", output_status_);
	form->addRow("Current Delay", current_delay_);
	form->addRow("Target Delay (sec)", target_seconds_);
	form->addRow("Holding Scene", holding_scene_);
	form->addRow("Emergency Dump (sec)", dump_seconds_);
	layout->addLayout(form);

	start_output_button_ = new QPushButton("Start Delayed Output", this);
	stop_output_button_ = new QPushButton("Stop Delayed Output", this);
	enable_button_ = new QPushButton("Enable / Set Delay", this);
	return_live_button_ = new QPushButton("Return Live", this);
	auto *dump = new QPushButton("Emergency Dump", this);
	layout->addWidget(start_output_button_);
	layout->addWidget(stop_output_button_);
	layout->addWidget(enable_button_);
	layout->addWidget(return_live_button_);
	layout->addWidget(dump);

	refresh_scenes();
	connect(start_output_button_, &QPushButton::clicked, this, &ActiveDelayDock::start_delayed_output);
	connect(stop_output_button_, &QPushButton::clicked, this, &ActiveDelayDock::stop_delayed_output);
	connect(enable_button_, &QPushButton::clicked, this, &ActiveDelayDock::enable_delay);
	connect(return_live_button_, &QPushButton::clicked, this, &ActiveDelayDock::return_live);
	connect(dump, &QPushButton::clicked, this, &ActiveDelayDock::emergency_dump);
	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, &ActiveDelayDock::refresh_status);
	timer_->start();
	handoff_timer_ = new QTimer(this);
	handoff_timer_->setSingleShot(true);
	connect(handoff_timer_, &QTimer::timeout, this, &ActiveDelayDock::complete_delayed_handoff);
	normal_restart_timer_ = new QTimer(this);
	normal_restart_timer_->setSingleShot(true);
	connect(normal_restart_timer_, &QTimer::timeout, this, &ActiveDelayDock::restart_normal_streaming);
	refresh_status();
}

ActiveDelayDock::~ActiveDelayDock()
{
	shutdown();
	if (original_scene_)
		obs_source_release(original_scene_);
}

void ActiveDelayDock::enable_delay()
{
	if (output_flow_state_ == OutputFlowState::CapturingNormal ||
	    output_flow_state_ == OutputFlowState::WaitingForNormalStop) {
		persistent_output_error_ = "A delayed-output handoff is already in progress";
		return;
	}

	std::string error;
	const auto target = std::chrono::seconds(target_seconds_->value());
	if (delayed_output_ && obs_output_active(delayed_output_)) {
		if (!session_->controller.set_target(target, &error)) {
			status_->setText(QString::fromStdString(error));
			return;
		}
		persistent_output_error_.clear();
		switch_to_holding_scene();
		return;
	}

	if (!obs_frontend_streaming_active()) {
		persistent_output_error_ =
			"Start Delayed Output first, or start normal OBS streaming and then enable the delay";
		return;
	}

	const auto answer = QMessageBox::question(
		this, "Switch to Active Live Delay",
		"OBS will keep the current stream connected while the delay buffer builds, then briefly reconnect "
		"through Active Live Delay. Continue?");
	if (answer != QMessageBox::Yes)
		return;

	QString preparation_error;
	if (!prepare_normal_capture(preparation_error)) {
		persistent_output_error_ = preparation_error;
		return;
	}

	session_->controller.return_live();
	if (!session_->controller.set_target(target, &error)) {
		detach_normal_capture();
		release_normal_output();
		status_->setText(QString::fromStdString(error));
		return;
	}
	output_flow_state_ = OutputFlowState::CapturingNormal;
	persistent_output_error_.clear();
	switch_to_holding_scene();
}

void ActiveDelayDock::return_live()
{
	if (output_flow_state_ == OutputFlowState::CapturingNormal) {
		detach_normal_capture();
		release_normal_output();
		output_flow_state_ = OutputFlowState::Stopped;
	}
	session_->controller.return_live();
	restore_program_scene();
}

void ActiveDelayDock::emergency_dump()
{
	std::string error;
	if (!session_->controller.emergency_dump(std::chrono::seconds(dump_seconds_->value()), &error))
		status_->setText(QString::fromStdString(error));
}

void ActiveDelayDock::refresh_scenes()
{
	holding_scene_->clear();
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i)
		holding_scene_->addItem(QString::fromUtf8(obs_source_get_name(scenes.sources.array[i])));
	obs_frontend_source_list_free(&scenes);
}

void ActiveDelayDock::refresh_status()
{
	if (check_delayed_output_health())
		return;

	const auto value = session_->controller.status();
	status_->setText(state_text(value.state));
	current_delay_->setText(QString::number(value.current_delay.count() / 1'000'000.0, 'f', 1) + " sec");
	if (!value.error.empty())
		status_->setText(QString::fromStdString(value.error));

	if (output_flow_state_ == OutputFlowState::CapturingNormal && value.state == DelayState::Delayed) {
		std::string header_error;
		if (!cache_active_codec_headers(normal_output_, *session_, header_error)) {
			cancel_handoff(QString::fromStdString(header_error));
			return;
		}
		detach_normal_capture();
		handoff_start_attempts_ = 0;
		output_flow_state_ = OutputFlowState::WaitingForNormalStop;
		blog(LOG_INFO, "[active-live-delay] Delay buffer is ready; stopping the normal OBS output for handoff");
		obs_frontend_streaming_stop();
	}
	if (output_flow_state_ == OutputFlowState::CapturingNormal && value.state == DelayState::Error)
		cancel_handoff(QString::fromStdString(value.error));

	if (output_flow_state_ == OutputFlowState::CapturingNormal)
		output_status_->setText("BUILDING FROM OBS STREAM");
	else if (output_flow_state_ == OutputFlowState::WaitingForNormalStop)
		output_status_->setText("SWITCHING OUTPUTS");
	else if (delayed_output_ && obs_output_active(delayed_output_))
		output_status_->setText("ACTIVE");
	else {
		if (delayed_output_) {
			const auto *last_error = obs_output_get_last_error(delayed_output_);
			if (last_error && *last_error)
				persistent_output_error_ = QString::fromUtf8(last_error);
		}
		output_status_->setText(persistent_output_error_.isEmpty() ? "STOPPED"
									     : "ERROR: " + persistent_output_error_);
	}

	if (value.state == DelayState::Delayed && output_flow_state_ == OutputFlowState::DelayedOutput)
		restore_program_scene();
}

void ActiveDelayDock::normal_packet_callback(obs_output_t *, encoder_packet *packet, encoder_packet_time *,
	void *data)
{
	auto *self = static_cast<ActiveDelayDock *>(data);
	if (!self || !packet || (packet->type == OBS_ENCODER_AUDIO && packet->track_idx != 0))
		return;
	self->session_->controller.ingest(copy_encoder_packet(*packet));
}

bool ActiveDelayDock::prepare_normal_capture(QString &error)
{
	clear_cached_codec_headers(*session_);
	release_normal_output();
	normal_output_ = obs_frontend_get_streaming_output();
	if (!normal_output_ || !obs_output_active(normal_output_)) {
		error = "OBS did not expose an active streaming output";
		release_normal_output();
		return false;
	}
	if (!obs_output_get_video_encoder(normal_output_) || !obs_output_get_audio_encoder(normal_output_, 0)) {
		error = "The current OBS stream does not expose both video and audio encoders";
		release_normal_output();
		return false;
	}
	obs_output_add_packet_callback(normal_output_, normal_packet_callback, this);
	normal_packet_callback_attached_ = true;
	return true;
}

void ActiveDelayDock::detach_normal_capture()
{
	if (!normal_output_ || !normal_packet_callback_attached_)
		return;
	obs_output_remove_packet_callback(normal_output_, normal_packet_callback, this);
	normal_packet_callback_attached_ = false;
}

void ActiveDelayDock::release_normal_output()
{
	detach_normal_capture();
	if (normal_output_) {
		obs_output_release(normal_output_);
		normal_output_ = nullptr;
	}
}

bool ActiveDelayDock::start_delayed_output_from(obs_output_t *source, bool preserve_delay, QString &error)
{
	if (!source) {
		error = "OBS has not prepared a streaming output yet";
		return false;
	}
	auto *video_encoder = obs_output_get_video_encoder(source);
	auto *audio_encoder = obs_output_get_audio_encoder(source, 0);
	// Enhanced Broadcasting uses a temporary multitrack service that cannot be
	// reused after its output stops. Prefer the persistent frontend service.
	auto *service = obs_frontend_get_streaming_service();
	if (!service)
		service = obs_output_get_service(source);
	if (!video_encoder || !audio_encoder) {
		error = "The OBS streaming output needs H.264 video and AAC audio encoders";
		return false;
	}
	const auto *video_codec = obs_encoder_get_codec(video_encoder);
	const auto *audio_codec = obs_encoder_get_codec(audio_encoder);
	if (!video_codec || std::strcmp(video_codec, "h264") != 0 || !audio_codec || std::strcmp(audio_codec, "aac") != 0) {
		error = "Active Live Delay requires the OBS stream encoders to use H.264 video and AAC audio";
		return false;
	}
	if (!service) {
		error = "No OBS streaming service is configured";
		return false;
	}

	// Twitch Enhanced Broadcasting synchronizes all of its video renditions
	// through one encoder group. Reusing only rendition 0 while it is still in
	// that group leaves it waiting forever for the other renditions to start:
	// OBS draws frames, but the encoder emits no packets. The normal output has
	// stopped before this method is called, so detach the retained primary
	// encoder from the now-inactive multitrack group. A false result means OBS
	// is still completing group teardown and the bounded handoff retry should
	// run instead.
	if (!obs_encoder_set_group(video_encoder, nullptr)) {
		error = "The Twitch multitrack video encoder group is still stopping";
		return false;
	}
	blog(LOG_INFO, "[active-live-delay] Primary video encoder detached from any multitrack encoder group");

	auto *output = obs_output_create("active_delay_rtmp_output", "active_delay_stream", nullptr, nullptr);
	if (!output) {
		error = "OBS could not create the Active Live Delay output";
		return false;
	}
	obs_output_set_video_encoder(output, video_encoder);
	obs_output_set_audio_encoder(output, audio_encoder, 0);
	obs_output_set_service(output, service);
	if (obs_output_get_service(output) != service) {
		error = "OBS could not attach the configured streaming service to Active Live Delay";
		obs_output_release(output);
		return false;
	}
	session_->preserve_controller_on_next_output_start.store(preserve_delay, std::memory_order_release);
	if (!obs_output_start(output)) {
		session_->preserve_controller_on_next_output_start.store(false, std::memory_order_release);
		const auto *last_error = obs_output_get_last_error(output);
		error = last_error && *last_error ? QString::fromUtf8(last_error)
							 : "The Active Live Delay output failed to start";
		obs_output_release(output);
		return false;
	}
	delayed_output_ = output;
	output_flow_state_ = OutputFlowState::DelayedOutput;
	delayed_output_started_at_ = std::chrono::steady_clock::now();
	restart_normal_on_delayed_failure_ = preserve_delay;
	persistent_output_error_.clear();
	blog(LOG_INFO, "[active-live-delay] Delayed RTMP output started successfully");
	return true;
}

void ActiveDelayDock::complete_delayed_handoff()
{
	if (shutting_down_ || output_flow_state_ != OutputFlowState::WaitingForNormalStop)
		return;

	++handoff_start_attempts_;
	blog(LOG_INFO, "[active-live-delay] Starting delayed output after OBS cleanup (attempt %d/%d)",
		handoff_start_attempts_, kMaxEncoderStartAttempts);
	QString error;
	if (start_delayed_output_from(normal_output_, true, error)) {
		release_normal_output();
		restore_program_scene();
		return;
	}

	blog(LOG_ERROR, "[active-live-delay] Delayed-output handoff attempt %d failed: %s",
		handoff_start_attempts_, error.toUtf8().constData());
	const auto encoder_teardown_race =
		error.contains("encoder", Qt::CaseInsensitive) || error.contains("initialize", Qt::CaseInsensitive);
	if (encoder_teardown_race && handoff_start_attempts_ < kMaxEncoderStartAttempts) {
		persistent_output_error_ = QString("Waiting for OBS encoder cleanup after attempt %1: %2")
						   .arg(handoff_start_attempts_)
						   .arg(error);
		handoff_timer_->start(kHandoffRetryDelayMs);
		return;
	}

	cancel_handoff(error + "; restarting normal OBS streaming");
	if (!shutting_down_)
		normal_restart_timer_->start(kNormalRestartDelayMs);
}

void ActiveDelayDock::restart_normal_streaming()
{
	if (shutting_down_ || output_flow_state_ != OutputFlowState::Stopped || obs_frontend_streaming_active())
		return;
	blog(LOG_INFO, "[active-live-delay] Restarting the normal OBS streaming output after failed handoff");
	obs_frontend_streaming_start();
}

bool ActiveDelayDock::check_delayed_output_health()
{
	if (output_flow_state_ != OutputFlowState::DelayedOutput || !delayed_output_)
		return false;

	const auto active_for = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - delayed_output_started_at_);
	const auto health = evaluate_delayed_output_health(obs_output_active(delayed_output_),
		obs_output_get_total_frames(delayed_output_), active_for);
	if (health == DelayedOutputHealth::Progressing)
		return false;
	if (health == DelayedOutputHealth::WaitingForVideo)
		return false;

	QString error;
	if (health == DelayedOutputHealth::Stopped) {
		const auto *last_error = obs_output_get_last_error(delayed_output_);
		error = last_error && *last_error ? QString::fromUtf8(last_error)
						  : "The delayed output stopped unexpectedly";
	} else {
		error = "The delayed output produced no encoded video frames within 5 seconds";
	}
	recover_from_delayed_output_failure(error);
	return true;
}

void ActiveDelayDock::recover_from_delayed_output_failure(const QString &error)
{
	if (output_flow_state_ != OutputFlowState::DelayedOutput)
		return;

	const auto restart_normal = restart_normal_on_delayed_failure_;
	blog(LOG_ERROR, "[active-live-delay] Delayed output failed after startup: %s%s", error.toUtf8().constData(),
		restart_normal ? "; restarting normal OBS streaming" : "");
	restart_normal_on_delayed_failure_ = false;
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
	handoff_start_attempts_ = 0;
	session_->preserve_controller_on_next_output_start.store(false, std::memory_order_release);
	clear_cached_codec_headers(*session_);
	session_->controller.return_live();
	persistent_output_error_ = error;
	if (restart_normal)
		persistent_output_error_ += "; restarting normal OBS streaming";
	restore_program_scene();
	if (!shutting_down_ && restart_normal)
		normal_restart_timer_->start(kNormalRestartDelayMs);
}

void ActiveDelayDock::start_delayed_output()
{
	if (delayed_output_ && obs_output_active(delayed_output_)) {
		persistent_output_error_ = "The delayed output is already active";
		return;
	}
	if (obs_frontend_streaming_active()) {
		persistent_output_error_ =
			"Normal OBS streaming is active; use Enable / Set Delay to build and hand off safely";
		return;
	}

	if (delayed_output_) {
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	auto *source = obs_frontend_get_streaming_output();
	QString error;
	if (!start_delayed_output_from(source, false, error))
		persistent_output_error_ = error;
	if (source)
		obs_output_release(source);
}

void ActiveDelayDock::stop_delayed_output()
{
	if (handoff_timer_)
		handoff_timer_->stop();
	if (normal_restart_timer_)
		normal_restart_timer_->stop();
	if (output_flow_state_ == OutputFlowState::CapturingNormal) {
		detach_normal_capture();
		release_normal_output();
	}
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
	restart_normal_on_delayed_failure_ = false;
	handoff_start_attempts_ = 0;
	session_->preserve_controller_on_next_output_start.store(false, std::memory_order_release);
	clear_cached_codec_headers(*session_);
	session_->controller.return_live();
	persistent_output_error_.clear();
	restore_program_scene();
}

void ActiveDelayDock::cancel_handoff(const QString &error)
{
	if (handoff_timer_)
		handoff_timer_->stop();
	detach_normal_capture();
	release_normal_output();
	output_flow_state_ = OutputFlowState::Stopped;
	restart_normal_on_delayed_failure_ = false;
	persistent_output_error_ = error;
	handoff_start_attempts_ = 0;
	blog(LOG_ERROR, "[active-live-delay] Handoff cancelled: %s", error.toUtf8().constData());
	session_->preserve_controller_on_next_output_start.store(false, std::memory_order_release);
	clear_cached_codec_headers(*session_);
	session_->controller.return_live();
	restore_program_scene();
}

void ActiveDelayDock::handle_frontend_event(obs_frontend_event event)
{
	if (event != OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		return;
	if (output_flow_state_ == OutputFlowState::WaitingForNormalStop) {
		// Twitch Enhanced Broadcasting releases its temporary encoders through a
		// queued main-thread callback. Starting a replacement output from inside
		// OBS_FRONTEND_EVENT_STREAMING_STOPPED races that cleanup, most commonly
		// leaving the AAC encoder unable to initialize. Let the event unwind and
		// the queued cleanup run before reusing the encoders.
		blog(LOG_INFO, "[active-live-delay] Normal OBS output stopped; waiting for encoder cleanup");
		handoff_timer_->start(kHandoffSettleDelayMs);
	} else if (output_flow_state_ == OutputFlowState::CapturingNormal) {
		cancel_handoff("The normal OBS stream stopped before the delay buffer was ready");
	}
}

void ActiveDelayDock::shutdown()
{
	if (shutting_down_)
		return;
	shutting_down_ = true;
	if (timer_)
		timer_->stop();
	if (handoff_timer_)
		handoff_timer_->stop();
	if (normal_restart_timer_)
		normal_restart_timer_->stop();
	detach_normal_capture();
	release_normal_output();
	if (delayed_output_) {
		if (obs_output_active(delayed_output_))
			obs_output_stop(delayed_output_);
		obs_output_release(delayed_output_);
		delayed_output_ = nullptr;
	}
	output_flow_state_ = OutputFlowState::Stopped;
	restart_normal_on_delayed_failure_ = false;
	handoff_start_attempts_ = 0;
	clear_cached_codec_headers(*session_);
}

void ActiveDelayDock::switch_to_holding_scene()
{
	if (original_scene_) {
		obs_source_release(original_scene_);
		original_scene_ = nullptr;
	}
	original_scene_ = obs_frontend_get_current_scene();
	auto *holding = obs_get_source_by_name(holding_scene_->currentText().toUtf8().constData());
	if (holding) {
		obs_frontend_set_current_scene(holding);
		obs_source_release(holding);
	}
}

void ActiveDelayDock::restore_program_scene()
{
	if (!original_scene_)
		return;
	obs_frontend_set_current_scene(original_scene_);
	obs_source_release(original_scene_);
	original_scene_ = nullptr;
}

} // namespace active_delay
