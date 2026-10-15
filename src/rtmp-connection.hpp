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

	// connect/send/close belong to one worker and must have finite I/O budgets.
	// interrupt is sticky, nonblocking and safe concurrently with those calls.
	// A new sender session gets a fresh connection object.
	virtual bool connect(const RtmpTarget &target, std::string &error) = 0;
	virtual bool send(std::span<const std::uint8_t> bytes, std::string &error) = 0;
	virtual void interrupt() noexcept = 0;
	// Cancels only an operation still blocked when the grace period ends, so a
	// prompt write completes untouched; clear disarms it once that write is done.
	// Connections without a deadline cancel at once.
	virtual void interrupt_after(std::chrono::milliseconds) noexcept { interrupt(); }
	virtual void clear_interrupt_after() noexcept {}
	virtual void close() noexcept = 0;
};

[[nodiscard]] bool build_rtmp_publish_url(const RtmpTarget &target, std::string &url, std::string &error);

} // namespace active_delay
