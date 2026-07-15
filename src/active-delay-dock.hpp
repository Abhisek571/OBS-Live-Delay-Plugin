#pragma once

#include "active-delay-session.hpp"

#include <chrono>
#include <memory>

extern "C" {
#include <obs-frontend-api.h>
#include <obs.h>
}

#include <QWidget>

#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace active_delay {

class ActiveDelayDock final : public QWidget {
public:
	explicit ActiveDelayDock(std::shared_ptr<ActiveDelaySession> session, QWidget *parent = nullptr);
	~ActiveDelayDock() override;

	void enable_delay();
	void return_live();
	void emergency_dump();
	void handle_frontend_event(obs_frontend_event event);
	void shutdown();

private:
	enum class OutputFlowState { Stopped, CapturingNormal, WaitingForNormalStop, DelayedOutput };

	static void normal_packet_callback(obs_output_t *, encoder_packet *packet, encoder_packet_time *, void *data);
	void start_delayed_output();
	void stop_delayed_output();
	void complete_delayed_handoff();
	void restart_normal_streaming();
	bool check_delayed_output_health();
	void recover_from_delayed_output_failure(const QString &error);
	bool start_delayed_output_from(obs_output_t *source, bool preserve_delay, QString &error);
	bool prepare_normal_capture(QString &error);
	void detach_normal_capture();
	void release_normal_output();
	void cancel_handoff(const QString &error);
	void refresh_scenes();
	void refresh_status();
	void switch_to_holding_scene();
	void restore_program_scene();

	std::shared_ptr<ActiveDelaySession> session_;
	QLabel *status_ = nullptr;
	QLabel *output_status_ = nullptr;
	QLabel *current_delay_ = nullptr;
	QComboBox *holding_scene_ = nullptr;
	QSpinBox *target_seconds_ = nullptr;
	QSpinBox *dump_seconds_ = nullptr;
	QPushButton *enable_button_ = nullptr;
	QPushButton *return_live_button_ = nullptr;
	QPushButton *start_output_button_ = nullptr;
	QPushButton *stop_output_button_ = nullptr;
	QTimer *timer_ = nullptr;
	QTimer *handoff_timer_ = nullptr;
	QTimer *normal_restart_timer_ = nullptr;
	obs_source_t *original_scene_ = nullptr;
	obs_output_t *normal_output_ = nullptr;
	obs_output_t *delayed_output_ = nullptr;
	OutputFlowState output_flow_state_ = OutputFlowState::Stopped;
	bool normal_packet_callback_attached_ = false;
	bool shutting_down_ = false;
	bool restart_normal_on_delayed_failure_ = false;
	int handoff_start_attempts_ = 0;
	std::chrono::steady_clock::time_point delayed_output_started_at_{};
	QString persistent_output_error_;
};

} // namespace active_delay
