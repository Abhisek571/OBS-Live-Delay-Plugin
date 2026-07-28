#pragma once

#include "rtmp-connection.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace active_delay {

enum class DestinationPlatform { CustomRtmp, Twitch, YouTube, Kick };

// Versioned, credential-bearing configuration.  It is deliberately kept out
// of status strings and diagnostics; callers persist it in the OBS profile.
struct MultistreamDestination {
	std::string id;
	std::string name;
	RtmpTarget target;
	DestinationPlatform platform = DestinationPlatform::CustomRtmp;
	bool enabled = true;
};

struct MultistreamConfiguration {
	static constexpr unsigned int kCurrentVersion = 2;
	unsigned int version = kCurrentVersion;
	std::vector<MultistreamDestination> secondary_destinations;
};

struct LegacyMultistreamSettingsV1 {
	bool enabled = false;
	std::string name;
	RtmpTarget target;
};

[[nodiscard]] bool validate_multistream_configuration(const MultistreamConfiguration &configuration,
	std::string &error);
[[nodiscard]] bool validate_multistream_configuration(const MultistreamConfiguration &configuration,
	const RtmpTarget &primary, std::string &error);
[[nodiscard]] MultistreamConfiguration migrate_multistream_v1(const LegacyMultistreamSettingsV1 &legacy);
[[nodiscard]] std::string_view destination_platform_id(DestinationPlatform platform) noexcept;
[[nodiscard]] std::string_view destination_platform_name(DestinationPlatform platform) noexcept;
[[nodiscard]] bool parse_destination_platform(std::string_view value, DestinationPlatform &platform) noexcept;
// A safe target label for dock status/logging. It never contains a URL, key,
// username, or password.
[[nodiscard]] std::string safe_destination_label(const MultistreamDestination &destination);

} // namespace active_delay
