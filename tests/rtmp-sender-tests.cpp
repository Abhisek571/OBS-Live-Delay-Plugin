#include "bounded-sender-queue.hpp"
#include "flv-muxer.hpp"
#include "rtmp-sender.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

FlvTag video(bool keyframe, std::uint8_t value);
FlvTag audio(std::uint8_t value);

FlvTag video(bool keyframe)
{
	return video(keyframe, 0x01);
}

FlvTag video(bool keyframe, std::uint8_t value)
{
	return {FlvTagType::Video, 0, {static_cast<std::uint8_t>(keyframe ? 0x17 : 0x27), 0x01, 0, 0, 0, value}, keyframe};
}

FlvTag audio()
{
	return audio(0x01);
}

FlvTag audio(std::uint8_t value)
{
	return {FlvTagType::Audio, 0, {0xaf, 0x01, value}, false};
}

struct FakeState {
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::vector<std::uint8_t>> writes;
	std::size_t connects = 0;
	std::size_t fail_on_send = 0;
	bool interrupted = false;
};

class FakeConnection final : public IRtmpConnection {
public:
	explicit FakeConnection(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

	bool connect(const RtmpTarget &, std::string &) override
	{
		std::scoped_lock lock(state_->mutex);
		++state_->connects;
		state_->interrupted = false;
		state_->changed.notify_all();
		return true;
	}

	bool send(std::span<const std::uint8_t> bytes, std::string &error) override
	{
		std::scoped_lock lock(state_->mutex);
		const auto ordinal = state_->writes.size() + 1;
		if (state_->fail_on_send != 0 && ordinal == state_->fail_on_send) {
			state_->fail_on_send = 0;
			error = "injected write failure";
			return false;
		}
		state_->writes.emplace_back(bytes.begin(), bytes.end());
		state_->changed.notify_all();
		return true;
	}

	void interrupt() noexcept override
	{
		std::scoped_lock lock(state_->mutex);
		state_->interrupted = true;
		state_->changed.notify_all();
	}

	void close() noexcept override {}

private:
	std::shared_ptr<FakeState> state_;
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, 2s, [&] { return state->writes.size() >= count; });
}

bool wait_for_failure(const std::shared_ptr<FakeState> &state)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, 2s, [&] { return state->interrupted; });
}

std::vector<FlvTag> headers()
{
	return FlvMuxer({{0x01}, {0x12, 0x10}}).sequence_headers();
}

void queue_accepts_or_rejects_whole_batches()
{
	BoundedSenderQueue queue({2, 1'024});
	require(!queue.try_push({video(true), audio(), video(false)}), "oversized tag batch should be rejected");
	require(queue.status().tags == 0, "a rejected batch must not be partially enqueued");
	require(queue.try_push({video(true), audio()}), "batch within both bounds should be accepted");
	require(queue.status().tags == 2, "accepted tags should be accounted for");
	require(!queue.try_push({audio()}), "queue should reject another tag at capacity");
	require(queue.wait_pop().has_value(), "queued tag should be available");
	queue.close(true);
	require(!queue.wait_pop().has_value(), "closed discarded queue should unblock consumers");
}

void publish_url_carries_service_credentials_and_key()
{
	std::string url;
	std::string error;
	require(build_rtmp_publish_url({"rtmps://example.test/live/", "/stream-key?token=abc", "user name", "p@ss"},
		url, error), "valid RTMPS target should build");
	require(url == "rtmps://user%20name:p%40ss@example.test/live/stream-key?token=abc",
		"credentials should be escaped without altering the stream key");
	require(!build_rtmp_publish_url({"https://example.test/live", "key"}, url, error),
		"non-RTMP schemes should be rejected");
	require(!error.empty(), "invalid publish target should explain the failure");
}

void sender_primes_connection_and_starts_at_keyframe()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 1, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "fake sender should start");
	require(sender.enqueue({audio(0x10), video(false, 0x11), video(true, 0x12), audio(0x13)}, error),
		"media batch should be accepted");
	require(wait_for_writes(state, 5), "headers and keyframe-aligned media should be written");
	sender.stop();

	std::scoped_lock lock(state->mutex);
	require(state->writes[0] == make_flv_header(), "connection should begin with the FLV header");
	require(state->writes.size() == 5, "audio and inter-frame video before the first keyframe should be dropped");
	require(state->writes[3] == serialize_flv_tag(video(true, 0x12)), "first media write should be a keyframe");
	require(state->writes[4] == serialize_flv_tag(audio(0x13)), "audio after the keyframe should be preserved");
}

void sender_reconnects_resends_headers_and_realigns_to_keyframe()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{32, 8'192}, 2, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "fake sender should start");
	{
		std::scoped_lock lock(state->mutex);
		state->fail_on_send = 5;
	}
	require(sender.enqueue({video(true, 0x20), audio(0x21), video(false, 0x22), audio(0x23), video(true, 0x24)}, error),
		"reconnect test batch should be accepted");
	require(wait_for_writes(state, 8), "reconnect should resend headers and resume at a later keyframe");
	sender.stop();

	std::scoped_lock lock(state->mutex);
	require(state->connects == 2, "one failed write should cause one reconnect");
	require(sender.status().reconnect_count == 1, "sender status should report the reconnect attempt");
	require(state->writes[4] == make_flv_header(), "reconnect should begin a new FLV stream");
	require(state->writes[7] == serialize_flv_tag(video(true, 0x24)), "reconnect should discard media until the next keyframe");
}

void sender_reports_one_terminal_output_failure()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 0, 1ms, 1s});
	std::atomic_uint callback_count = 0;
	std::string callback_error;
	std::mutex callback_mutex;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(),
		[&](const std::string &message) {
			std::scoped_lock lock(callback_mutex);
			callback_error = message;
			++callback_count;
			std::scoped_lock state_lock(state->mutex);
			state->interrupted = true;
			state->changed.notify_all();
		},
		error), "fake sender should start");
	{
		std::scoped_lock lock(state->mutex);
		state->fail_on_send = 4;
	}
	require(sender.enqueue({video(true, 0x30)}, error), "failing media write should enter the sender queue");
	require(wait_for_failure(state), "terminal runtime failure should notify the output owner");

	const auto status = sender.status();
	require(status.state == SenderState::Failed, "exhausted reconnects must leave the sender failed");
	require(status.error.find("injected write failure") != std::string::npos,
		"terminal failure should retain the connection error");
	require(callback_count == 1, "the output failure callback must be signaled exactly once");
	{
		std::scoped_lock lock(callback_mutex);
		require(callback_error == status.error, "the callback should receive the terminal sender error");
	}
	sender.stop();
}
} // namespace

int main()
{
	try {
		queue_accepts_or_rejects_whole_batches();
		publish_url_carries_service_credentials_and_key();
		sender_primes_connection_and_starts_at_keyframe();
		sender_reconnects_resends_headers_and_realigns_to_keyframe();
		sender_reports_one_terminal_output_failure();
		std::cout << "RTMP sender tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "RTMP sender test failure: " << error.what() << '\n';
		return 1;
	}
}
