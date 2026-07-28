#include "destination-card-widget.hpp"

#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QVariant>
#include <QVBoxLayout>

#include <utility>

namespace active_delay {
namespace {
int platform_index(DestinationPlatform platform)
{
	return static_cast<int>(platform);
}
} // namespace

DestinationCardWidget::DestinationCardWidget(QString slot_id, QString title, DestinationCardText text, QWidget *parent)
	: QGroupBox(std::move(title), parent), slot_id_(std::move(slot_id)), text_(std::move(text))
{
	setObjectName("ald_destination_" + slot_id_);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	enabled_ = new QCheckBox(text_.enabled, this);
	enabled_->setObjectName("ald_" + slot_id_ + "_enabled");
	platform_ = new QComboBox(this);
	platform_->setObjectName("ald_" + slot_id_ + "_platform");
	for (int index = 0; index < static_cast<int>(text_.platform_names.size()); ++index)
		platform_->addItem(text_.platform_names[static_cast<std::size_t>(index)], index);
	name_ = new QLineEdit(this);
	name_->setObjectName("ald_" + slot_id_ + "_name");
	server_ = new QLineEdit(this);
	server_->setObjectName("ald_" + slot_id_ + "_server");
	key_ = new QLineEdit(this);
	key_->setObjectName("ald_" + slot_id_ + "_key");
	key_->setEchoMode(QLineEdit::Password);
	key_->setPlaceholderText(text_.key_placeholder);
	reveal_ = new QPushButton(text_.reveal_key, this);
	reveal_->setObjectName("ald_" + slot_id_ + "_reveal");
	reveal_->setAutoDefault(false);
	guidance_ = new QLabel(this);
	guidance_->setObjectName("ald_" + slot_id_ + "_guidance");
	guidance_->setWordWrap(true);
	guidance_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	status_ = new QLabel(this);
	status_->setObjectName("ald_" + slot_id_ + "_status");
	status_->setWordWrap(true);

	auto *key_row = new QWidget(this);
	auto *key_layout = new QHBoxLayout(key_row);
	key_layout->setContentsMargins(0, 0, 0, 0);
	key_layout->addWidget(key_, 1);
	key_layout->addWidget(reveal_);
	auto *form = new QFormLayout();
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->addRow(text_.platform, platform_);
	form->addRow(text_.display_name, name_);
	form->addRow(text_.server_url, server_);
	form->addRow(text_.stream_key, key_row);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(4);
	layout->addWidget(status_);
	layout->addWidget(enabled_);
	layout->addLayout(form);
	layout->addWidget(guidance_);

	connect(enabled_, &QCheckBox::toggled, this, [this] { update_editability(); });
	connect(platform_, &QComboBox::currentIndexChanged, this, [this] { update_platform_guidance(); });
	connect(reveal_, &QPushButton::pressed, this, [this] { key_->setEchoMode(QLineEdit::Normal); });
	connect(reveal_, &QPushButton::released, this, [this] { key_->setEchoMode(QLineEdit::Password); });
	update_platform_guidance();
	update_editability();
}

MultistreamDestination DestinationCardWidget::destination() const
{
	const auto selected = static_cast<DestinationPlatform>(platform_->currentData().toInt());
	return {slot_id_.toStdString(), name_->text().trimmed().toStdString(),
		{server_->text().trimmed().toStdString(), key_->text().toStdString()}, selected, enabled_->isChecked()};
}

void DestinationCardWidget::set_destination(const MultistreamDestination &destination)
{
	enabled_->setChecked(destination.enabled);
	platform_->setCurrentIndex(platform_index(destination.platform));
	name_->setText(QString::fromStdString(destination.name));
	server_->setText(QString::fromStdString(destination.target.server_url));
	key_->setText(QString::fromStdString(destination.target.stream_key));
	update_platform_guidance();
	update_editability();
}

void DestinationCardWidget::set_editable(bool editable)
{
	editable_ = editable;
	update_editability();
}

void DestinationCardWidget::set_status_text(const QString &status)
{
	status_->setText(status);
}

void DestinationCardWidget::refresh_secret_mask()
{
	if (!reveal_->isDown())
		key_->setEchoMode(QLineEdit::Password);
}

void DestinationCardWidget::update_platform_guidance()
{
	const auto index = std::clamp(platform_->currentIndex(), 0, static_cast<int>(text_.guidance.size()) - 1);
	guidance_->setText(text_.guidance[static_cast<std::size_t>(index)]);
	server_->setPlaceholderText(text_.server_placeholders[static_cast<std::size_t>(index)]);
	if (name_->text().trimmed().isEmpty() ||
		name_->text() == text_.platform_names[static_cast<std::size_t>(previous_platform_index_)])
		name_->setText(text_.platform_names[static_cast<std::size_t>(index)]);
	previous_platform_index_ = index;
}

void DestinationCardWidget::update_editability()
{
	enabled_->setEnabled(editable_);
	const auto fields_enabled = editable_ && enabled_->isChecked();
	platform_->setEnabled(fields_enabled);
	name_->setEnabled(fields_enabled);
	server_->setEnabled(fields_enabled);
	key_->setEnabled(fields_enabled);
	reveal_->setEnabled(fields_enabled);
	if (!fields_enabled)
		key_->setEchoMode(QLineEdit::Password);
}

} // namespace active_delay
