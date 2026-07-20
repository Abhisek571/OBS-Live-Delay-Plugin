#include "multistream-config.hpp"

#include "diagnostic-error.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace active_delay {
namespace {
bool valid_identifier(const std::string &value)
{
	return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
		return std::isalnum(character) != 0 || character == '-' || character == '_';
	});
}
} // namespace

bool validate_multistream_configuration(const MultistreamConfiguration &configuration, std::string &error)
{
	error.clear();
	if (configuration.version != MultistreamConfiguration::kCurrentVersion) {
		error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"The saved multistream destination format is not supported by this plugin version");
		return false;
	}
	if (configuration.secondary_destinations.empty()) {
		error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
			"Native Multistream needs at least one enabled secondary destination");
		return false;
	}

	std::set<std::string> ids;
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
		std::string ignored_url;
		std::string target_error;
		if (!build_rtmp_publish_url(destination.target, ignored_url, target_error)) {
			error = diagnostic_error(DiagnosticCode::MultistreamConfigurationInvalid,
				"A secondary RTMP destination is incomplete or unsupported");
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

std::string safe_destination_label(const MultistreamDestination &destination)
{
	return destination.name.empty() ? "Unnamed destination" : destination.name;
}

} // namespace active_delay
