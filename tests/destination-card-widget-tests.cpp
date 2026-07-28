#include "destination-card-widget.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace active_delay;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

DestinationCardText text()
{
	return {"Enabled", "Platform", "Name", "Server URL", "Stream Key", "Hold to reveal",
		{"Custom RTMP", "Twitch", "YouTube", "Kick"},
		{"Custom guidance", "Twitch guidance", "YouTube guidance", "Kick guidance"},
		{"Custom server", "Twitch server", "YouTube server", "Kick server"}, "Paste key"};
}

void compact_card_has_stable_controls_and_safe_key_behavior()
{
	DestinationCardWidget card("secondary_2", "2 Secondary", text(), nullptr);
	card.resize(320, 420);
	require(card.minimumSizeHint().width() <= 320, "destination card must fit a 320-pixel narrow dock");
	require(card.findChild<QComboBox *>("ald_secondary_2_platform"), "platform selector needs a stable ID");
	auto *key = card.findChild<QLineEdit *>("ald_secondary_2_key");
	auto *reveal = card.findChild<QPushButton *>("ald_secondary_2_reveal");
	require(key && reveal, "key controls need stable IDs");
	require(key->echoMode() == QLineEdit::Password, "stream key must be masked by default");
	reveal->pressed();
	require(key->echoMode() == QLineEdit::Normal, "press must reveal the key intentionally");
	reveal->released();
	require(key->echoMode() == QLineEdit::Password, "release must mask the key immediately");
}

void platform_selection_changes_guidance_without_inventing_credentials()
{
	DestinationCardWidget card("secondary_3", "3 Secondary", text(), nullptr);
	auto *platform = card.findChild<QComboBox *>("ald_secondary_3_platform");
	auto *server = card.findChild<QLineEdit *>("ald_secondary_3_server");
	auto *name = card.findChild<QLineEdit *>("ald_secondary_3_name");
	auto *guidance = card.findChild<QLabel *>("ald_secondary_3_guidance");
	require(platform && server && name && guidance, "platform guidance controls need stable IDs");
	platform->setCurrentIndex(static_cast<int>(DestinationPlatform::Kick));
	require(server->placeholderText() == "Kick server" && guidance->text() == "Kick guidance",
		"platform selector must update instructions and placeholders");
	require(name->text() == "Kick", "untouched default display name must follow the platform selection");
	require(server->text().isEmpty(), "platform selector must never invent a publish URL");
}
} // namespace

int main(int argc, char **argv)
{
	QApplication application(argc, argv);
	try {
		compact_card_has_stable_controls_and_safe_key_behavior();
		platform_selection_changes_guidance_without_inventing_credentials();
		std::cout << "destination card widget tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "destination card widget test failure: " << error.what() << '\n';
		return 1;
	}
}
