#pragma once

#include "active-delay-session.hpp"
#include "destination-card-widget.hpp"

#include <chrono>
#include <memory>
#include <array>

extern "C" {
#include <obs-frontend-api.h>
#include <obs.h>
}

#include <QWidget>

#include <QString>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace active_delay {

class ActiveDelayDock final : public QWidget {
public:
	explicit ActiveDelayDock(std::shared_ptr<ActiveDelaySession> session, QWidget *parent);
	~ActiveDelayDock() override;

	void enable_delay();
	void return_live();
	void handle_frontend_event(obs_frontend_event event);
	void shutdown();

private:
	enum class OutputFlowState { Stopped, DelayedOutput };

	void start_delayed_output();
	void stop_delayed_output();
	bool check_delayed_output_health();
	void recover_from_delayed_output_failure(const QString &error);
	bool start_delayed_output_with(obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder, obs_service_t *service,
		QString &error);
	bool start_delayed_output_direct(QString &error);
	void refresh_scenes();
	void refresh_status();
	bool check_scene_switch_health();
	void load_multistream_settings();
	bool save_multistream_settings(const MultistreamConfiguration &configuration, QString &error);
	[[nodiscard]] MultistreamConfiguration multistream_configuration_from_cards() const;
	bool configure_multistream_mode(QString &error);
	void refresh_target_status();
	void refresh_preflight_summary();
	bool validate_multistream_preflight(config_t *profile, obs_data_t *video_settings, QString &error);
	bool switch_to_holding_scene(QString &error);
	void restore_program_scene();
	void release_scene_switch_refs();
	void report_operational_error(const QString &error, int log_level);

	std::shared_ptr<ActiveDelaySession> session_;
	QLabel *status_ = nullptr;
	QLabel *output_status_ = nullptr;
	QLabel *current_delay_ = nullptr;
	QLabel *target_status_ = nullptr;
	QLabel *preflight_status_ = nullptr;
	QLabel *primary_target_status_ = nullptr;
	QComboBox *holding_scene_ = nullptr;
	QSpinBox *target_seconds_ = nullptr;
	std::array<DestinationCardWidget *, 2> secondary_cards_{};
	QPushButton *enable_button_ = nullptr;
	QPushButton *return_live_button_ = nullptr;
	QPushButton *start_output_button_ = nullptr;
	QPushButton *stop_output_button_ = nullptr;
	QTimer *timer_ = nullptr;
	obs_source_t *original_scene_ = nullptr;
	obs_source_t *active_holding_scene_ = nullptr;
	obs_output_t *delayed_output_ = nullptr;
	OutputFlowState output_flow_state_ = OutputFlowState::Stopped;
	bool scene_switch_action_in_progress_ = false;
	bool shutting_down_ = false;
	std::chrono::steady_clock::time_point delayed_output_started_at_{};
	QString persistent_output_error_;
};

} // namespace active_delay
