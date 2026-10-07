#pragma once

#include "multistream-config.hpp"

#include <QGroupBox>
#include <QString>

#include <array>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace active_delay {

struct DestinationCardText {
	QString enabled;
	QString platform;
	QString display_name;
	QString server_url;
	QString stream_key;
	QString reveal_key;
	QString show_settings;
	QString hide_settings;
	QString unlock_settings;
	QString lock_settings;
	QString locked_hint;
	std::array<QString, 4> platform_names;
	std::array<QString, 4> guidance;
	std::array<QString, 4> server_placeholders;
	QString key_placeholder;
};

class DestinationCardWidget final : public QGroupBox {
public:
	DestinationCardWidget(QString slot_id, QString title, DestinationCardText text, QWidget *parent);

	[[nodiscard]] MultistreamDestination destination() const;
	void set_destination(const MultistreamDestination &destination);
	void set_editable(bool editable);
	void set_status_text(const QString &status);
	void refresh_secret_mask();

private:
	void update_platform_guidance();
	void update_editability();
	void update_disclosure_state();
	void toggle_lock();
	void toggle_details();

	QString slot_id_;
	DestinationCardText text_;
	bool editable_ = true;
	bool locked_ = true;
	bool expanded_ = false;
	int previous_platform_index_ = 0;
	QCheckBox *enabled_ = nullptr;
	QComboBox *platform_ = nullptr;
	QLineEdit *name_ = nullptr;
	QLineEdit *server_ = nullptr;
	QLineEdit *key_ = nullptr;
	QPushButton *reveal_ = nullptr;
	QPushButton *details_toggle_ = nullptr;
	QPushButton *lock_toggle_ = nullptr;
	QLabel *guidance_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *locked_hint_ = nullptr;
	QWidget *details_ = nullptr;
};

} // namespace active_delay
