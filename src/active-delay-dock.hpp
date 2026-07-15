#pragma once

#include "active-delay-session.hpp"

#include <memory>

extern "C" {
#include <obs.h>
}

#include <QDockWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace active_delay {

class ActiveDelayDock final : public QDockWidget {
public:
	explicit ActiveDelayDock(std::shared_ptr<ActiveDelaySession> session, QWidget *parent = nullptr);
	~ActiveDelayDock() override;

	void enable_delay();
	void return_live();
	void emergency_dump();

private:
	void refresh_scenes();
	void refresh_status();
	void switch_to_holding_scene();
	void restore_program_scene();

	std::shared_ptr<ActiveDelaySession> session_;
	QLabel *status_ = nullptr;
	QLabel *current_delay_ = nullptr;
	QComboBox *holding_scene_ = nullptr;
	QSpinBox *target_seconds_ = nullptr;
	QSpinBox *dump_seconds_ = nullptr;
	QPushButton *enable_button_ = nullptr;
	QPushButton *return_live_button_ = nullptr;
	QTimer *timer_ = nullptr;
	obs_source_t *original_scene_ = nullptr;
};

} // namespace active_delay
