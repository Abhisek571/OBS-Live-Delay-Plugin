#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace active_delay {

struct RtmpTarget {
	std::string server_url;
	std::string stream_key;
	std::string username;
	std::string password;
	std::chrono::milliseconds io_timeout{10'000};
};

class IRtmpConnection {
public:
	virtual ~IRtmpConnection() = default;

	virtual bool connect(const RtmpTarget &target, std::string &error) = 0;
	virtual bool send(std::span<const std::uint8_t> bytes, std::string &error) = 0;
	virtual void interrupt() noexcept = 0;
	virtual void close() noexcept = 0;
};

[[nodiscard]] bool build_rtmp_publish_url(const RtmpTarget &target, std::string &url, std::string &error);

} // namespace active_delay
