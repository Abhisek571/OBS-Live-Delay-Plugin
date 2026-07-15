#include "active-delay-dock.hpp"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>

namespace active_delay {
namespace {
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

ActiveDelayDock::ActiveDelayDock(QWidget *parent) : QDockWidget("OBS Active Live Delay", parent)
{
	auto *content = new QWidget(this);
	auto *layout = new QVBoxLayout(content);
	auto *form = new QFormLayout();
	status_ = new QLabel(content);
	current_delay_ = new QLabel(content);
	holding_scene_ = new QComboBox(content);
	target_seconds_ = new QSpinBox(content);
	target_seconds_->setRange(1, 600);
	target_seconds_->setValue(30);
	dump_seconds_ = new QSpinBox(content);
	dump_seconds_->setRange(1, 120);
	dump_seconds_->setValue(10);
	form->addRow("Status", status_);
	form->addRow("Current Delay", current_delay_);
	form->addRow("Target Delay (sec)", target_seconds_);
	form->addRow("Holding Scene", holding_scene_);
	form->addRow("Emergency Dump (sec)", dump_seconds_);
	layout->addLayout(form);

	enable_button_ = new QPushButton("Enable / Set Delay", content);
	return_live_button_ = new QPushButton("Return Live", content);
	auto *dump = new QPushButton("Emergency Dump", content);
	layout->addWidget(enable_button_);
	layout->addWidget(return_live_button_);
	layout->addWidget(dump);
	setWidget(content);

	refresh_scenes();
	connect(enable_button_, &QPushButton::clicked, this, &ActiveDelayDock::enable_delay);
	connect(return_live_button_, &QPushButton::clicked, this, &ActiveDelayDock::return_live);
	connect(dump, &QPushButton::clicked, this, &ActiveDelayDock::emergency_dump);
	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, &ActiveDelayDock::refresh_status);
	timer_->start();
	refresh_status();
}

ActiveDelayDock::~ActiveDelayDock()
{
	if (original_scene_)
		obs_source_release(original_scene_);
}

void ActiveDelayDock::enable_delay()
{
	std::string error;
	if (!controller_.set_target(std::chrono::seconds(target_seconds_->value()), &error)) {
		status_->setText(QString::fromStdString(error));
		return;
	}
	switch_to_holding_scene();
}

void ActiveDelayDock::return_live()
{
	controller_.return_live();
	restore_program_scene();
}

void ActiveDelayDock::emergency_dump()
{
	std::string error;
	if (!controller_.emergency_dump(std::chrono::seconds(dump_seconds_->value()), &error))
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
	const auto value = controller_.status();
	status_->setText(state_text(value.state));
	current_delay_->setText(QString::number(value.current_delay.count() / 1'000'000.0, 'f', 1) + " sec");
	if (value.state == DelayState::Error && !value.error.empty())
		status_->setText(QString::fromStdString(value.error));
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
