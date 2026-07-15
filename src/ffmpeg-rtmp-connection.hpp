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

	AVIOContext *context_ = nullptr;
	std::atomic_bool interrupted_ = false;
};

} // namespace active_delay
