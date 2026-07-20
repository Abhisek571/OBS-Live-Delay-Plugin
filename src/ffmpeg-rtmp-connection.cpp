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
	interrupted_.store(false, std::memory_order_release);

	std::string url;
	if (!build_rtmp_publish_url(target, url, error))
		return false;

	Dictionary options;
	options.set("rw_timeout", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(target.io_timeout).count()));
	options.set("rtmp_live", "live");
	options.set("tcp_nodelay", "1");
	options.set("tcp_keepalive", "1");
	AVIOInterruptCB callback = {interrupt_callback, this};
	const auto result = avio_open2(&context_, url.c_str(), AVIO_FLAG_WRITE, &callback, options.address());
	if (result < 0) {
		error = diagnostic_error(DiagnosticCode::RtmpConnectionFailed,
			"Unable to connect to the RTMP server: " + ffmpeg_error(result));
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
	while (!bytes.empty()) {
		const auto count = static_cast<int>(std::min<std::size_t>(bytes.size(), INT_MAX));
		avio_write(context_, bytes.data(), count);
		bytes = bytes.subspan(static_cast<std::size_t>(count));
	}
	avio_flush(context_);
	if (context_->error < 0) {
		error = diagnostic_error(DiagnosticCode::RtmpWriteFailed,
			"RTMP write failed: " + ffmpeg_error(context_->error));
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
	if (context_)
		avio_closep(&context_);
}

int FfmpegRtmpConnection::interrupt_callback(void *opaque) noexcept
{
	return static_cast<FfmpegRtmpConnection *>(opaque)->interrupted_.load(std::memory_order_acquire) ? 1 : 0;
}

std::string FfmpegRtmpConnection::ffmpeg_error(int code)
{
	std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
	if (av_strerror(code, buffer.data(), buffer.size()) == 0)
		return buffer.data();
	return "FFmpeg error " + std::to_string(code);
}

} // namespace active_delay
