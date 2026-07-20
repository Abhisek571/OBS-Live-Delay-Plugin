#include "rtmp-connection.hpp"

#include "diagnostic-error.hpp"

#include <cctype>
#include <string_view>
#include <utility>

namespace active_delay {
namespace {
bool is_unreserved(unsigned char value)
{
	return std::isalnum(value) != 0 || value == '-' || value == '.' || value == '_' || value == '~';
}

std::string percent_encode(std::string_view value)
{
	constexpr char digits[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (is_unreserved(byte)) {
			result.push_back(character);
		} else {
			result.push_back('%');
			result.push_back(digits[byte >> 4]);
			result.push_back(digits[byte & 0x0f]);
		}
	}
	return result;
}
} // namespace

bool build_rtmp_publish_url(const RtmpTarget &target, std::string &url, std::string &error)
{
	error.clear();
	url.clear();
	if (target.server_url.empty()) {
		error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "RTMP server URL is empty");
		return false;
	}
	if (target.stream_key.empty()) {
		error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "RTMP stream key is empty");
		return false;
	}

	const auto scheme_end = target.server_url.find("://");
	if (scheme_end == std::string::npos) {
		error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "RTMP server URL has no scheme");
		return false;
	}
	const auto scheme = target.server_url.substr(0, scheme_end);
	if (scheme != "rtmp" && scheme != "rtmps") {
		error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "Only rtmp:// and rtmps:// server URLs are supported");
		return false;
	}

	auto server = target.server_url;
	while (server.size() > scheme_end + 3 && server.back() == '/')
		server.pop_back();
	if (!target.username.empty()) {
		const auto authority_end = server.find('/', scheme_end + 3);
		const auto existing_at = server.find('@', scheme_end + 3);
		if (existing_at != std::string::npos && (authority_end == std::string::npos || existing_at < authority_end)) {
			error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "RTMP URL already contains credentials");
			return false;
		}
		std::string credentials = percent_encode(target.username);
		if (!target.password.empty())
			credentials += ':' + percent_encode(target.password);
		credentials.push_back('@');
		server.insert(scheme_end + 3, credentials);
	}

	auto key = target.stream_key;
	while (!key.empty() && key.front() == '/')
		key.erase(key.begin());
	if (key.empty()) {
		error = diagnostic_error(DiagnosticCode::RtmpTargetInvalid, "RTMP stream key is empty");
		return false;
	}
	url = std::move(server) + '/' + std::move(key);
	return true;
}

} // namespace active_delay
