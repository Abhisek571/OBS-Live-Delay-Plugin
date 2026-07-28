#include "multistream-config.hpp"

#include "diagnostic-error.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>

namespace active_delay {
namespace {
bool valid_identifier(const std::string &value)
{
	return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
		return std::isalnum(character) != 0 || character == '-' || character == '_';
	});
}

bool build_destination_identity(const RtmpTarget &target, std::string &identity)
{
	auto target_without_transport_credentials = target;
	target_without_transport_credentials.username.clear();
	target_without_transport_credentials.password.clear();
	std::string ignored_error;
	return build_rtmp_publish_url(target_without_transport_credentials, identity, ignored_error);
}

bool validate_configuration(const MultistreamConfiguration &configuration, const RtmpTarget *primary,
	std::string &error)
{
	error.clear();
	if (configuration.version != MultistreamConfiguration::kCurrentVersion) {
		error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"The saved multistream destination format is not supported by this plugin version");
		return false;
	}
	if (configuration.secondary_destinations.size() > 2) {
		error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"Native Multistream supports exactly two optional secondary destination slots");
		return false;
	}

	std::set<std::string> ids;
	std::set<std::string> publish_urls;
	if (primary) {
		std::string primary_url;
		if (build_destination_identity(*primary, primary_url))
			publish_urls.insert(std::move(primary_url));
	}
	for (const auto &destination : configuration.secondary_destinations) {
		if (!destination.enabled)
			continue;
		if (!valid_identifier(destination.id) || !ids.insert(destination.id).second) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"Each enabled secondary destination needs a unique safe identifier");
			return false;
		}
		if (destination.name.empty() || destination.name.size() > 80) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"Each enabled secondary destination needs a display name of 1 to 80 characters");
			return false;
		}
		std::string publish_url;
		std::string target_error;
		if (!build_rtmp_publish_url(destination.target, publish_url, target_error)) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"A secondary RTMP destination is incomplete or unsupported");
			return false;
		}
		std::string destination_identity;
		if (!build_destination_identity(destination.target, destination_identity)) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"A secondary RTMP destination is incomplete or unsupported");
			return false;
		}
		if (!publish_urls.insert(std::move(destination_identity)).second) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"Each enabled destination must use a different server and stream key");
			return false;
		}
	}
	if (ids.empty()) {
		error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"Native Multistream needs at least one enabled secondary destination");
		return false;
	}
	return true;
}
} // namespace

bool validate_multistream_configuration(const MultistreamConfiguration &configuration, std::string &error)
{
	return validate_configuration(configuration, nullptr, error);
}

bool validate_multistream_configuration(const MultistreamConfiguration &configuration, const RtmpTarget &primary,
	std::string &error)
{
	return validate_configuration(configuration, &primary, error);
}

MultistreamConfiguration migrate_multistream_v1(const LegacyMultistreamSettingsV1 &legacy)
{
	MultistreamConfiguration configuration;
	configuration.secondary_destinations.push_back(
		{"secondary_2", legacy.name, legacy.target, DestinationPlatform::CustomRtmp, legacy.enabled});
	return configuration;
}

std::string_view destination_platform_id(DestinationPlatform platform) noexcept
{
	switch (platform) {
	case DestinationPlatform::CustomRtmp: return "custom";
	case DestinationPlatform::Twitch: return "twitch";
	case DestinationPlatform::YouTube: return "youtube";
	case DestinationPlatform::Kick: return "kick";
	}
	return "custom";
}

std::string_view destination_platform_name(DestinationPlatform platform) noexcept
{
	switch (platform) {
	case DestinationPlatform::CustomRtmp: return "Custom RTMP";
	case DestinationPlatform::Twitch: return "Twitch";
	case DestinationPlatform::YouTube: return "YouTube";
	case DestinationPlatform::Kick: return "Kick";
	}
	return "Custom RTMP";
}

bool parse_destination_platform(std::string_view value, DestinationPlatform &platform) noexcept
{
	for (const auto candidate : {DestinationPlatform::CustomRtmp, DestinationPlatform::Twitch,
		     DestinationPlatform::YouTube, DestinationPlatform::Kick}) {
		if (value == destination_platform_id(candidate)) {
			platform = candidate;
			return true;
		}
	}
	return false;
}

std::string safe_destination_label(const MultistreamDestination &destination)
{
	const auto fallback = std::string(destination_platform_name(destination.platform));
	if (destination.name.empty() || destination.name.find("://") != std::string::npos)
		return fallback;
	for (const auto *secret : {&destination.target.stream_key, &destination.target.username,
		     &destination.target.password}) {
		if (!secret->empty() && destination.name.find(*secret) != std::string::npos)
			return fallback;
	}
	return destination.name == fallback ? fallback : fallback + " - " + destination.name;
}

} // namespace active_delay
