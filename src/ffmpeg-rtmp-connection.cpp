#include "ffmpeg-rtmp-connection.hpp"

#include "diagnostic-error.hpp"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <climits>

namespace active_delay {
namespace {
class Dictionary final {
public:
	~Dictionary() { av_dict_free(&value_); }
	Dictionary(const Dictionary &) = delete;
	Dictionary &operator=(const Dictionary &) = delete;
	Dictionary() = default;

	void set(const char *key, const std::string &value) { av_dict_set(&value_, key, value.c_str(), 0); }
	void set(const char *key, const char *value) { av_dict_set(&value_, key, value, 0); }
	AVDictionary **address() noexcept { return &value_; }

private:
	AVDictionary *value_ = nullptr;
};

} // namespace

FfmpegRtmpConnection::~FfmpegRtmpConnection()
{
	close();
}

bool FfmpegRtmpConnection::connect(const RtmpTarget &target, std::string &error)
{
	close();
	// A sender creates a fresh connection for each start. Cancellation remains
	// set across reconnect attempts so a racing stop cannot be lost here.
	if (interrupted_.load(std::memory_order_acquire)) {
		error = diagnostic_error(DiagnosticCode::RtmpConnectionFailed, "RTMP connection was cancelled");
		return false;
	}

	std::string url;
	if (!build_rtmp_publish_url(target, url, error))
		return false;
	io_timeout_ = std::max(target.io_timeout, std::chrono::milliseconds{1});
	// Cooperative FFmpeg deadline, not a hard OS-call bound: this SDK's TCP
	// getaddrinfo and Schannel certificate validation do not poll our callback.
	begin_operation(io_timeout_);

	Dictionary options;
	options.set("rw_timeout", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(io_timeout_).count()));
	options.set("rtmp_live", "live");
	options.set("tcp_nodelay", "1");
	options.set("tcp_keepalive", "1");
	// FFmpeg TLS defaults to no peer verification, including Schannel. Keep
	// the original URL hostname for both SNI and certificate identity checks.
	options.set("tls_verify", "1");
	AVIOInterruptCB callback = {interrupt_callback, this};
	const auto result = avio_open2(&context_, url.c_str(), AVIO_FLAG_WRITE, &callback, options.address());
	if (result < 0) {
		error = diagnostic_error(DiagnosticCode::RtmpConnectionFailed,
			"Unable to connect to the RTMP server: " + ffmpeg_error(result) +
			(url.starts_with("rtmps://") ? ". Check network reachability and server certificate trust and hostname." : ""));
		context_ = nullptr;
		return false;
	}
	return true;
}

bool FfmpegRtmpConnection::send(std::span<const std::uint8_t> bytes, std::string &error)
{
	if (!context_) {
		error = diagnostic_error(DiagnosticCode::RtmpWriteFailed, "RTMP connection is not open");
		return false;
	}
	begin_operation(io_timeout_);
	while (!bytes.empty()) {
		if (interrupt_callback(this)) {
			error = diagnostic_error(DiagnosticCode::RtmpWriteFailed, "RTMP write was cancelled or timed out");
			return false;
		}
		const auto count = static_cast<int>(std::min<std::size_t>(bytes.size(), INT_MAX));
		avio_write(context_, bytes.data(), count);
		bytes = bytes.subspan(static_cast<std::size_t>(count));
	}
	avio_flush(context_);
	if (context_->error < 0 || interrupt_callback(this)) {
		error = diagnostic_error(DiagnosticCode::RtmpWriteFailed,
			context_->error < 0 ? "RTMP write failed: " + ffmpeg_error(context_->error)
				: "RTMP write was cancelled or timed out");
		return false;
	}
	return true;
}

void FfmpegRtmpConnection::interrupt() noexcept
{
	interrupted_.store(true, std::memory_order_release);
}

void FfmpegRtmpConnection::close() noexcept
{
	if (context_) {
		// closep flushes pending bytes and closes the protocol. Keep its final
		// writes bounded too; never clear sticky cancellation during teardown.
		begin_operation(std::min(io_timeout_, std::chrono::milliseconds{2'000}));
		avio_closep(&context_);
	}
}

void FfmpegRtmpConnection::begin_operation(std::chrono::milliseconds timeout) noexcept
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	deadline_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count(),
		std::memory_order_release);
}

int FfmpegRtmpConnection::interrupt_callback(void *opaque) noexcept
{
	const auto &connection = *static_cast<FfmpegRtmpConnection *>(opaque);
	if (connection.interrupted_.load(std::memory_order_acquire))
		return 1;
	const auto deadline = connection.deadline_ns_.load(std::memory_order_acquire);
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return deadline != 0 && now >= deadline ? 1 : 0;
}

std::string FfmpegRtmpConnection::ffmpeg_error(int code)
{
	std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
	if (av_strerror(code, buffer.data(), buffer.size()) == 0)
		return buffer.data();
	return "FFmpeg error " + std::to_string(code);
}

} // namespace active_delay
