#include "ffmpeg-rtmp-connection.hpp"
#include "flv-muxer.hpp"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
}

#include <winsock2.h>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class SilentReceiver {
public:
	SilentReceiver()
	{
		WSADATA data{};
		require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup failed");
		listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		require(listener_ != INVALID_SOCKET, "socket failed");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		require(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind failed");
		int size = sizeof(address);
		require(getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) == 0, "getsockname failed");
		port_ = ntohs(address.sin_port);
		require(listen(listener_, 1) == 0, "listen failed");
	}
	~SilentReceiver()
	{
		if (peer_ != INVALID_SOCKET)
			closesocket(peer_);
		closesocket(listener_);
		WSACleanup();
	}
	RtmpTarget target() const
	{
		return {"rtmp://127.0.0.1:" + std::to_string(port_) + "/live", "synthetic-test", {}, {}, 5s};
	}
	void release_listener()
	{
		closesocket(listener_);
		listener_ = INVALID_SOCKET;
	}
	void accept_client()
	{
		WSAPOLLFD descriptor{listener_, POLLRDNORM, 0};
		require(WSAPoll(&descriptor, 1, 2000) > 0, "client did not connect");
		peer_ = accept(listener_, nullptr, nullptr);
		require(peer_ != INVALID_SOCKET, "accept failed");
	}
	void trickle_handshake(const std::atomic_bool &finished)
	{
		char byte = 3;
		while (!finished.load()) {
			if (::send(peer_, &byte, 1, 0) != 1)
				break;
			byte = 0;
			std::this_thread::sleep_for(20ms);
		}
	}

private:
	SOCKET listener_ = INVALID_SOCKET;
	SOCKET peer_ = INVALID_SOCKET;
	unsigned short port_ = 0;
};

void handshake_has_a_wall_clock_deadline()
{
	SilentReceiver receiver;
	FfmpegRtmpConnection connection;
	auto target = receiver.target();
	target.io_timeout = 120ms;
	const auto begin = std::chrono::steady_clock::now();
	auto operation = std::async(std::launch::async, [&] {
		std::string error;
		return connection.connect(target, error);
	});
	receiver.accept_client();
	std::atomic_bool finished = false;
	std::jthread trickle([&] { receiver.trickle_handshake(finished); });
	const bool bounded = operation.wait_for(700ms) == std::future_status::ready;
	if (!bounded)
		connection.interrupt();
	const bool connected = operation.get();
	finished = true;
	trickle.join();
	require(!connected, "trickling handshake must fail");
	require(bounded, "handshake must observe the 120ms wall-clock operation deadline within 700ms");
	std::cout << "handshake deadline ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() << '\n';
}

void blocked_publication_write_is_bounded(bool cancel)
{
	SilentReceiver reservation;
	auto target = reservation.target();
	target.io_timeout = cancel ? 5s : 300ms;
	std::string url, error;
	require(build_rtmp_publish_url(target, url, error), "synthetic receiver URL must build");
	reservation.release_listener();
	std::atomic_bool receiver_stopped = false;
	std::atomic_bool receiver_open = false;
	std::atomic_bool receiver_done = false;
	const auto receiver_deadline = std::chrono::steady_clock::now() + 5s;
	auto receiver = std::async(std::launch::async, [&] {
		struct InterruptState { std::atomic_bool *stopped; std::chrono::steady_clock::time_point deadline; } state{&receiver_stopped, receiver_deadline};
		AVIOInterruptCB callback{[](void *opaque) {
			const auto &s = *static_cast<InterruptState *>(opaque);
			return s.stopped->load() || std::chrono::steady_clock::now() >= s.deadline ? 1 : 0;
		}, &state};
		AVDictionary *options = nullptr;
		av_dict_set(&options, "rtmp_listen", "1", 0);
		av_dict_set(&options, "timeout", "2", 0);
		AVIOContext *input = nullptr;
		const auto result = avio_open2(&input, url.c_str(), AVIO_FLAG_READ, &callback, &options);
		av_dict_free(&options);
		receiver_open = result >= 0;
		while (result >= 0 && !receiver_stopped && std::chrono::steady_clock::now() < receiver_deadline)
			std::this_thread::sleep_for(1ms); // deliberately never drain media
		if (input)
			avio_closep(&input);
		receiver_done = true;
		return result;
	});
	FfmpegRtmpConnection connection;
	bool connected = false;
	for (int attempt = 0; attempt < 10 && !connected; ++attempt) {
		connected = connection.connect(target, error);
		if (!connected)
			std::this_thread::sleep_for(10ms);
	}
	if (!connected) {
		receiver_stopped = true;
		receiver.get();
		require(false, "local FFmpeg RTMP publication must connect");
	}
	require(connection.send(make_flv_header(), error), "publication must send FLV header");
	for (const auto &header : FlvMuxer({{0x01}, {0x12, 0x10}}).sequence_headers())
		require(connection.send(serialize_flv_tag(header), error), "publication must prime codec headers");
	const auto ready_deadline = std::chrono::steady_clock::now() + 1s;
	while (!receiver_open && !receiver_done && std::chrono::steady_clock::now() < ready_deadline)
		std::this_thread::sleep_for(1ms);
	if (!receiver_open) {
		connection.interrupt(); connection.close(); receiver_stopped = true;
		std::cout << "receiver open result=" << receiver.get() << '\n';
		require(false, "local receiver must be ready before blocked media");
	}
	FlvTag media{FlvTagType::Video, 0, std::vector<std::uint8_t>(12 * 1024 * 1024, 0x55), true};
	media.payload[0] = 0x17; media.payload[1] = 1;
	const auto bytes = serialize_flv_tag(media);
	const auto begin = std::chrono::steady_clock::now();
	auto write = std::async(std::launch::async, [&] { std::string write_error; return connection.send(bytes, write_error); });
	const bool initially_blocked = write.wait_for(50ms) == std::future_status::timeout;
	const auto cancel_begin = std::chrono::steady_clock::now();
	if (cancel)
		connection.interrupt();
	const bool bounded = write.wait_for(1500ms) == std::future_status::ready;
	if (!bounded)
		connection.interrupt();
	const bool sent = write.get();
	const auto write_elapsed = std::chrono::steady_clock::now() - (cancel ? cancel_begin : begin);
	const auto close_begin = std::chrono::steady_clock::now();
	connection.close();
	const auto close_elapsed = std::chrono::steady_clock::now() - close_begin;
	receiver_stopped = true;
	const auto receiver_result = receiver.get();
	require(receiver_result >= 0 && receiver_open && receiver_done, "local receiver must open and close cleanly");
	require(initially_blocked && bounded && !sent, "a stalled publication write must be interrupted or time out");
	require(write_elapsed < 2s && close_elapsed < 2s, "write cancellation/deadline and close must each finish within two seconds");
	std::cout << (cancel ? "write cancellation" : "write deadline") << " ms="
		<< std::chrono::duration_cast<std::chrono::milliseconds>(write_elapsed).count()
		<< " close ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(close_elapsed).count() << '\n';
}

void cancellation_before_connect_is_sticky()
{
	SilentReceiver receiver;
	FfmpegRtmpConnection connection;
	connection.interrupt();
	std::string error;
	const auto started = std::chrono::steady_clock::now();
	require(!connection.connect(receiver.target(), error), "cancelled connect must fail");
	require(std::chrono::steady_clock::now() - started < 500ms, "connect must not clear an earlier cancellation");
}

void cancellation_interrupts_a_stalled_handshake()
{
	SilentReceiver receiver;
	FfmpegRtmpConnection connection;
	auto operation = std::async(std::launch::async, [&] {
		std::string error;
		return connection.connect(receiver.target(), error);
	});
	receiver.accept_client();
	connection.interrupt();
	require(operation.wait_for(1s) == std::future_status::ready, "cancelled handshake must finish within one second");
	require(!operation.get(), "cancelled handshake must fail");
}
} // namespace

int main()
{
	try {
		cancellation_before_connect_is_sticky();
		handshake_has_a_wall_clock_deadline();
		cancellation_interrupts_a_stalled_handshake();
		blocked_publication_write_is_bounded(false);
		blocked_publication_write_is_bounded(true);
		std::cout << "FFmpeg cancellation tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}