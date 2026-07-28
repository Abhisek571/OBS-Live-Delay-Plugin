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

	QString slot_id_;
	DestinationCardText text_;
	bool editable_ = true;
	int previous_platform_index_ = 0;
	QCheckBox *enabled_ = nullptr;
	QComboBox *platform_ = nullptr;
	QLineEdit *name_ = nullptr;
	QLineEdit *server_ = nullptr;
	QLineEdit *key_ = nullptr;
	QPushButton *reveal_ = nullptr;
	QLabel *guidance_ = nullptr;
	QLabel *status_ = nullptr;
};

} // namespace active_delay
