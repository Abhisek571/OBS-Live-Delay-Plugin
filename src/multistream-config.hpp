#pragma once

#include "rtmp-connection.hpp"

#include <string>
#include <vector>

namespace active_delay {

// Versioned, credential-bearing configuration.  It is deliberately kept out
// of status strings and diagnostics; callers persist it in the OBS profile.
struct MultistreamDestination {
	std::string id;
	std::string name;
	RtmpTarget target;
	bool enabled = true;
};

struct MultistreamConfiguration {
	static constexpr unsigned int kCurrentVersion = 1;
	unsigned int version = kCurrentVersion;
	std::vector<MultistreamDestination> secondary_destinations;
};

[[nodiscard]] bool validate_multistream_configuration(const MultistreamConfiguration &configuration,
	std::string &error);
// A safe target label for dock status/logging. It never contains a URL, key,
// username, or password.
[[nodiscard]] std::string safe_destination_label(const MultistreamDestination &destination);

} // namespace active_delay
