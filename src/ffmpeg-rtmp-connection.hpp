#pragma once

#include "rtmp-connection.hpp"

#include <atomic>

struct AVIOContext;

namespace active_delay {

class FfmpegRtmpConnection final : public IRtmpConnection {
public:
	FfmpegRtmpConnection() = default;
	~FfmpegRtmpConnection() override;

	FfmpegRtmpConnection(const FfmpegRtmpConnection &) = delete;
	FfmpegRtmpConnection &operator=(const FfmpegRtmpConnection &) = delete;

	bool connect(const RtmpTarget &target, std::string &error) override;
	bool send(std::span<const std::uint8_t> bytes, std::string &error) override;
	void interrupt() noexcept override;
	void close() noexcept override;

private:
	static int interrupt_callback(void *opaque) noexcept;
	[[nodiscard]] static std::string ffmpeg_error(int code);
	void begin_operation(std::chrono::milliseconds timeout) noexcept;

	AVIOContext *context_ = nullptr;
	std::atomic_bool interrupted_ = false;
	std::atomic<std::int64_t> deadline_ns_ = 0;
	std::chrono::milliseconds io_timeout_{10'000};
};

} // namespace active_delay
