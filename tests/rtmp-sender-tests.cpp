#include "bounded-sender-queue.hpp"
#include "flv-muxer.hpp"
#include "rtmp-sender.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
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
	std::size_t closes = 0;
	std::size_t fail_on_send = 0;
	std::size_t failed_connects = 0;
	bool interrupted = false;
	bool block_connect = false;
	bool block_reconnect = false;
	bool block_write = false;
	bool write_entered = false;
	bool throw_connect = false;
};

class FakeConnection final : public IRtmpConnection {
public:
	explicit FakeConnection(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

	bool connect(const RtmpTarget &, std::string &) override
	{
		std::unique_lock lock(state_->mutex);
		++state_->connects;
		state_->changed.notify_all();
		if (state_->throw_connect)
			throw std::runtime_error("injected connect exception");
		if (state_->failed_connects != 0) {
			--state_->failed_connects;
			return false;
		}
		if (state_->block_connect) {
			state_->changed.wait_for(lock, 3s, [&] { return !state_->block_connect || state_->interrupted; });
			return !state_->block_connect && !state_->interrupted;
		}
		if (state_->connects > 1 && state_->block_reconnect)
			return state_->changed.wait_for(lock, 2s, [&] { return !state_->block_reconnect || state_->interrupted; }) &&
				!state_->interrupted;
		return true;
	}

	bool send(std::span<const std::uint8_t> bytes, std::string &error) override
	{
		std::unique_lock lock(state_->mutex);
		const auto ordinal = state_->writes.size() + 1;
		if (state_->block_write) {
			state_->write_entered = true;
			state_->changed.notify_all();
			state_->changed.wait_for(lock, 3s, [&] { return state_->interrupted; });
			return false;
		}
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

	void close() noexcept override
	{
		std::scoped_lock lock(state_->mutex);
		++state_->closes;
		state_->changed.notify_all();
	}

private:
	std::shared_ptr<FakeState> state_;
};

bool wait_for_writes(const std::shared_ptr<FakeState> &state, std::size_t count)
{
	std::unique_lock lock(state->mutex);
	return state->changed.wait_for(lock, 2s, [&] { return state->writes.size() >= count; });
}

void wait_until_running(const RtmpSender &sender)
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (sender.status().state != SenderState::Running && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	require(sender.status().state == SenderState::Running, "sender must become ready for media");
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

void sender_start_does_not_wait_for_connection()
{
	auto state = std::make_shared<FakeState>();
	state->block_connect = true;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 1, 1ms, 1s});
	std::string error;
	const auto begin = std::chrono::steady_clock::now();
	const bool accepted = sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error);
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	const auto stop_begin = std::chrono::steady_clock::now();
	sender.stop();
	require(accepted, "start should accept a connection attempt without waiting for network completion");
	require(elapsed < 100ms, "start must not block on network connection");
	require(std::chrono::steady_clock::now() - stop_begin < 200ms, "stop must interrupt a pending connection");
}

void sender_drops_media_while_starting()
{
	auto state = std::make_shared<FakeState>();
	state->block_connect = true;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 1, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "start should be accepted");
	require(sender.enqueue(std::vector<FlvTag>(64, video(true)), error),
		"starting media must be dropped without overflowing the queue");
	require(sender.status().queued_tags == 0, "starting must not retain stale media");
	{
		std::scoped_lock lock(state->mutex);
		state->block_connect = false;
		state->changed.notify_all();
	}
	wait_until_running(sender);
	require(sender.enqueue({audio(0x61), video(false, 0x62), video(true, 0x63), audio(0x64)}, error), "fresh startup media must enqueue");
	require(wait_for_writes(state, 5), "startup must resume with a fresh keyframe and audio");
	sender.stop();
	std::scoped_lock lock(state->mutex);
	require(state->writes.size() == 5 && state->writes[3] == serialize_flv_tag(video(true, 0x63)) &&
		state->writes[4] == serialize_flv_tag(audio(0x64)), "startup must never replay pre-connection media");
}

void sender_retries_initial_connection_without_blocking_capture()
{
	auto state = std::make_shared<FakeState>();
	state->failed_connects = 1;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 2, 1ms, 1s});
	std::atomic_uint failures = 0;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(),
		[&](const std::string &) { ++failures; }, error), "initial connection should be asynchronous");
	require(wait_for_writes(state, 3), "initial connection failure should retry and prime the stream");
	wait_until_running(sender);
	require(sender.enqueue({video(true, 0x41), audio(0x42)}, error), "recovered sender should accept media");
	require(wait_for_writes(state, 5), "recovered connection should deliver media");
	sender.stop();
	require(failures == 0, "a recovered connection must not signal terminal failure");
	require(state->connects == 2, "one initial failure should require one reconnect");
}

void sender_primes_connection_and_starts_at_keyframe()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{16, 4'096}, 1, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "fake sender should start");
	wait_until_running(sender);
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

void sender_rejects_late_audio_before_the_fresh_keyframe()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "start must succeed");
	wait_until_running(sender);
	auto boundary = video(true, 0x51);
	boundary.timestamp_ms = 100;
	auto early = audio(0x52);
	early.timestamp_ms = 99;
	auto aligned = audio(0x53);
	aligned.timestamp_ms = 100;
	require(sender.enqueue({boundary, early, aligned}, error), "boundary batch must enqueue");
	require(wait_for_writes(state, 5), "boundary and aligned audio must be written");
	sender.stop();
	std::scoped_lock lock(state->mutex);
	require(state->writes.size() == 5 && state->writes[4] == serialize_flv_tag(aligned),
		"late audio timestamped before the fresh keyframe must be discarded");
}

void startup_deadline_interrupts_blocked_header_write()
{
	auto state = std::make_shared<FakeState>();
	state->block_write = true;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 3, 1ms, 80ms});
	std::atomic_uint callbacks = 0;
	std::string error;
	const auto begin = std::chrono::steady_clock::now();
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), [&](const std::string &) { ++callbacks; }, error),
		"bounded startup must be asynchronous");
	const auto deadline = begin + 600ms;
	while (callbacks == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	const bool failed_in_budget = callbacks == 1 && sender.status().state == SenderState::Failed;
	sender.stop();
	require(failed_in_budget, "startup deadline must interrupt blocked priming and report one failure within 600ms");
	std::cout << "startup failure bound ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() << '\n';
}

void sender_reconnects_resends_headers_and_realigns_to_keyframe()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); },
		{{32, 8'192}, 2, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "fake sender should start");
	wait_until_running(sender);
	{
		std::scoped_lock lock(state->mutex);
		state->fail_on_send = 5;
		state->block_reconnect = true;
	}
	require(sender.enqueue({video(true, 0x20), audio(0x21), video(false, 0x22), audio(0x23), video(true, 0x24)}, error),
		"reconnect test batch should be accepted");
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->connects == 2; }), "reconnect must reach the blocked transport");
	}
	require(sender.enqueue(std::vector<FlvTag>(64, video(true, 0x25)), error),
		"disconnected media should be discarded rather than exhaust the sender queue");
	require(sender.status().queued_tags == 0, "reconnect must invalidate the disconnected backlog");
	{
		std::scoped_lock lock(state->mutex);
		state->block_reconnect = false;
		state->changed.notify_all();
	}
	require(wait_for_writes(state, 7), "reconnect should resend headers");
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (sender.status().state != SenderState::Running && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	require(sender.status().state == SenderState::Running, "reconnected sender must become ready");
	auto boundary = video(true, 0x28); boundary.timestamp_ms = 100;
	auto early = audio(0x29); early.timestamp_ms = 99;
	auto aligned = audio(0x2a); aligned.timestamp_ms = 100;
	require(sender.enqueue({audio(0x26), video(false, 0x27), boundary, early, aligned}, error), "fresh media should be accepted");
	require(wait_for_writes(state, 9), "reconnect should resume at a fresh keyframe with aligned audio");
	sender.stop();

	std::scoped_lock lock(state->mutex);
	require(state->connects == 2, "one failed write should cause one reconnect");
	require(sender.status().reconnect_count == 1, "sender status should report the reconnect attempt");
	require(state->writes[4] == make_flv_header(), "reconnect should begin a new FLV stream");
	require(state->writes.size() == 9, "reconnect must not replay stale queued media or pre-boundary audio");
	require(state->writes[7] == serialize_flv_tag(boundary) && state->writes[8] == serialize_flv_tag(aligned),
		"reconnect should discard media until a fresh keyframe and enforce its audio cutoff");
}

void requested_stop_is_truthful_and_rejects_new_media()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 0, 1ms, 1s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "start must succeed");
	wait_until_running(sender);
	sender.request_stop();
	const auto stopping = sender.status().state;
	const bool accepted = sender.enqueue({video(true)}, error);
	sender.stop();
	require(stopping == SenderState::Stopping, "request_stop must publish Stopping rather than Running");
	require(!accepted, "requested stop must reject new media");
	require(sender.status().state == SenderState::Stopped, "joined worker must report Stopped");
}

void terminal_failure_closes_transport_before_callback()
{
	auto state = std::make_shared<FakeState>();
	state->throw_connect = true;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 0, 1ms, 1s});
	std::atomic_bool callback_seen = false;
	std::atomic_bool closed_before_callback = false;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), [&](const std::string &) {
		{
			std::scoped_lock lock(state->mutex);
			closed_before_callback = state->closes > 0;
		}
		sender.stop(); // worker callback may request stop, but must never join itself
		callback_seen = true;
	}, error), "start must succeed");
	const auto deadline = std::chrono::steady_clock::now() + 1s;
	while (!callback_seen && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	sender.stop();
	require(callback_seen && closed_before_callback, "terminal failure must release transport before owner callback");
}

void repeated_sessions_allow_concurrent_stop_and_enqueue()
{
	std::shared_ptr<FakeState> state;
	RtmpSender sender([&state] { return std::make_unique<FakeConnection>(state); }, {{128, 32768}, 0, 1ms, 1s});
	for (int session = 0; session < 8; ++session) {
		state = std::make_shared<FakeState>();
		std::string error;
		require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "session must start");
		wait_until_running(sender);
		std::barrier ready(4);
		std::jthread first([&] { ready.arrive_and_wait(); sender.stop(); });
		std::jthread second([&] { ready.arrive_and_wait(); sender.stop(); });
		std::jthread producer([&] {
			ready.arrive_and_wait();
			for (int packet = 0; packet < 32; ++packet) {
				std::string enqueue_error;
				sender.enqueue({video(true)}, enqueue_error);
				(void)sender.status();
			}
		});
		ready.arrive_and_wait();
		first.join(); second.join(); producer.join();
		require(sender.status().state == SenderState::Stopped && sender.status().queued_tags == 0,
			"concurrent stop must join once and leave no queued media");
	}
}

void cancellation_wakes_a_long_retry_delay()
{
	auto state = std::make_shared<FakeState>();
	state->failed_connects = 100;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 3, 30s, 60s});
	std::atomic_uint callbacks = 0;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), [&](const std::string &) { ++callbacks; }, error), "start must succeed");
	const auto ready_deadline = std::chrono::steady_clock::now() + 1s;
	while (sender.status().state != SenderState::Reconnecting && std::chrono::steady_clock::now() < ready_deadline)
		std::this_thread::sleep_for(1ms);
	require(sender.status().state == SenderState::Reconnecting, "failed startup must enter retry delay");
	const auto begin = std::chrono::steady_clock::now();
	sender.stop();
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	require(elapsed < 2s && callbacks == 0, "stop must wake retry delay without signaling terminal failure");
	std::cout << "retry cancellation ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}

void stop_interrupts_a_blocked_media_write()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 3, 1ms, 1s});
	std::atomic_uint callbacks = 0;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), [&](const std::string &) { ++callbacks; }, error), "start must succeed");
	wait_until_running(sender);
	{
		std::scoped_lock lock(state->mutex);
		state->block_write = true;
	}
	require(sender.enqueue({video(true)}, error), "blocked media must enqueue");
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 1s, [&] { return state->write_entered; }), "media must enter blocked write");
	}
	const auto begin = std::chrono::steady_clock::now();
	sender.stop();
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	require(elapsed < 2s && callbacks == 0 && sender.status().state == SenderState::Stopped,
		"blocked-write stop must interrupt, join and suppress cancellation callbacks");
	std::cout << "blocked-media stop ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}

void teardown_joins_an_active_failure_callback()
{
	auto state = std::make_shared<FakeState>();
	state->throw_connect = true;
	RtmpSender sender([state] { return std::make_unique<FakeConnection>(state); }, {{16, 4096}, 0, 1ms, 1s});
	std::promise<void> entered, release;
	auto released = release.get_future();
	std::atomic_bool finished = false;
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), [&](const std::string &) {
		entered.set_value();
		released.wait();
		sender.stop(); // must not wait for the owner's lifecycle lock while it joins us
		finished = true;
	}, error), "start must succeed");
	const bool callback_entered = entered.get_future().wait_for(1s) == std::future_status::ready;
	if (!callback_entered)
		release.set_value();
	require(callback_entered, "failure callback must enter");
	auto teardown = std::async(std::launch::async, [&] { sender.stop(); });
	const bool joined_callback = teardown.wait_for(50ms) == std::future_status::timeout;
	release.set_value();
	require(teardown.wait_for(2s) == std::future_status::ready, "owner teardown and callback self-stop must not deadlock");
	teardown.get();
	require(joined_callback && finished && sender.status().state == SenderState::Stopped,
		"owner stop must not return until the active callback has quiesced");
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
	wait_until_running(sender);
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

// Mirrors FFmpeg: once interrupted, this transport instance fails every later
// connect and write. Only a replacement connection can recover.
class StickyConnection final : public IRtmpConnection {
public:
	explicit StickyConnection(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

	bool connect(const RtmpTarget &, std::string &) override
	{
		std::unique_lock lock(state_->mutex);
		++state_->connects;
		state_->changed.notify_all();
		if (interrupted_)
			return false;
		if (state_->connects > 1 && state_->block_reconnect) {
			state_->changed.wait_for(lock, 2s, [&] { return interrupted_ || !state_->block_reconnect; });
			if (interrupted_) {
				state_->block_reconnect = false;
				return false;
			}
		}
		return true;
	}

	bool send(std::span<const std::uint8_t> bytes, std::string &error) override
	{
		std::unique_lock lock(state_->mutex);
		if (interrupted_) {
			error = "transport was cancelled";
			return false;
		}
		if (state_->block_write) {
			state_->write_entered = true;
			state_->changed.notify_all();
			state_->changed.wait_for(lock, 3s, [&] { return interrupted_; });
			error = "transport was cancelled";
			return false;
		}
		if (state_->fail_on_send != 0 && state_->writes.size() + 1 == state_->fail_on_send) {
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
		interrupted_ = true;
		state_->interrupted = true;
		state_->changed.notify_all();
	}

	void close() noexcept override
	{
		std::scoped_lock lock(state_->mutex);
		++state_->closes;
		state_->changed.notify_all();
	}

private:
	std::shared_ptr<FakeState> state_;
	bool interrupted_ = false;
};

bool wait_until_state(const RtmpSender &sender, SenderState expected, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (sender.status().state != expected && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	return sender.status().state == expected;
}

void transition_during_reconnect_replaces_the_cancelled_transport()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<StickyConnection>(state); },
		{{16, 4'096}, 2, 1ms, 5s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "sticky sender should start");
	wait_until_running(sender);
	{
		std::scoped_lock lock(state->mutex);
		state->fail_on_send = state->writes.size() + 1;
		state->block_reconnect = true;
	}
	require(sender.enqueue_epoch({video(true, 0x40)}, 1, {}, error), "media should enter the sender queue");
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->connects == 2; }),
			"failed write should reach a blocked reconnect attempt");
	}
	// Return Live / Emergency Dump lands while the reconnect is mid-connect.
	sender.invalidate_epoch(2);
	require(wait_until_state(sender, SenderState::Running, 2s),
		"a transition during reconnect must not exhaust retries on a cancelled transport");
	std::scoped_lock lock(state->mutex);
	require(state->connects >= 3, "recovery must connect through a replacement transport");
}

void transition_during_drain_write_reconnects_instead_of_failing()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<StickyConnection>(state); },
		{{16, 4'096}, 2, 1ms, 5s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "sticky sender should start");
	wait_until_running(sender);
	{
		std::scoped_lock lock(state->mutex);
		state->block_write = true;
	}
	FlvTag drain = audio(0x00);
	drain.audio_drain = true;
	require(sender.enqueue_epoch({drain}, 1, {}, error), "drain guard should enter the sender queue");
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->write_entered; }), "drain write should start");
		state->block_write = false;
	}
	// A second action supersedes the guard while it is still being written.
	sender.invalidate_epoch(2);
	{
		std::unique_lock lock(state->mutex);
		state->changed.wait_for(lock, 2s, [&] { return state->connects >= 2; });
	}
	require(wait_until_state(sender, SenderState::Running, 2s) && sender.status().error.empty(),
		"a superseded drain guard must reconnect, not fail the destination");
	std::scoped_lock lock(state->mutex);
	require(state->connects == 2, "the interrupted transport must be replaced exactly once");
}

void transition_reconnect_keeps_the_new_epoch_guard()
{
	auto state = std::make_shared<FakeState>();
	RtmpSender sender([state] { return std::make_unique<StickyConnection>(state); },
		{{16, 4'096}, 2, 1ms, 5s});
	std::string error;
	require(sender.start({"rtmp://localhost/live", "test"}, headers(), {}, error), "sticky sender should start");
	wait_until_running(sender);
	{
		std::scoped_lock lock(state->mutex);
		state->block_write = true;
		state->block_reconnect = true;
	}
	require(sender.enqueue_epoch({video(true, 0x50)}, 1, {}, error), "programme media should enter the sender queue");
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->write_entered; }), "programme write should start");
		state->block_write = false;
	}
	// The action interrupts the write; the coordinator then sends its drain guard.
	sender.invalidate_epoch(2);
	{
		std::unique_lock lock(state->mutex);
		require(state->changed.wait_for(lock, 2s, [&] { return state->connects == 2; }),
			"the interrupted write should start a transport replacement");
	}
	FlvTag drain = audio(0x00);
	drain.audio_drain = true;
	drain.delivery_ticket = 7;
	require(sender.enqueue_epoch({drain}, 2, headers(), error), "drain guard should be accepted during the replacement");
	{
		std::scoped_lock lock(state->mutex);
		state->block_reconnect = false;
		state->changed.notify_all();
	}
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (std::chrono::steady_clock::now() < deadline &&
		!(sender.status().delivered_epoch == 2 && sender.status().delivery_ticket == 7))
		std::this_thread::sleep_for(1ms);
	const auto status = sender.status();
	require(status.delivered_epoch == 2 && status.delivery_ticket == 7,
		"a transition's own reconnect must deliver its drain guard, or the coordinator waits forever");
}
} // namespace

int main()
{
	try {
		queue_accepts_or_rejects_whole_batches();
		publish_url_carries_service_credentials_and_key();
		sender_start_does_not_wait_for_connection();
		sender_drops_media_while_starting();
		sender_retries_initial_connection_without_blocking_capture();
		sender_primes_connection_and_starts_at_keyframe();
		sender_rejects_late_audio_before_the_fresh_keyframe();
		startup_deadline_interrupts_blocked_header_write();
		sender_reconnects_resends_headers_and_realigns_to_keyframe();
		requested_stop_is_truthful_and_rejects_new_media();
		terminal_failure_closes_transport_before_callback();
		repeated_sessions_allow_concurrent_stop_and_enqueue();
		cancellation_wakes_a_long_retry_delay();
		stop_interrupts_a_blocked_media_write();
		teardown_joins_an_active_failure_callback();
		sender_reports_one_terminal_output_failure();
		transition_during_reconnect_replaces_the_cancelled_transport();
		transition_during_drain_write_reconnects_instead_of_failing();
		transition_reconnect_keeps_the_new_epoch_guard();
		std::cout << "RTMP sender tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "RTMP sender test failure: " << error.what() << '\n';
		return 1;
	}
}
