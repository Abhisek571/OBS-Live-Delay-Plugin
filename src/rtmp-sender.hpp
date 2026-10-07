#pragma once

#include "bounded-sender-queue.hpp"
#include "rtmp-connection.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace active_delay {

enum class SenderState { Stopped, Starting, Running, Reconnecting, Stopping, Failed };

struct SenderConfig {
	SenderQueueLimits queue;
	std::size_t reconnect_attempts = 3;
	std::chrono::milliseconds reconnect_delay{500};
	std::chrono::milliseconds startup_timeout{15'000};
};

struct SenderStatus {
	SenderState state = SenderState::Stopped;
	std::size_t queued_tags = 0;
	std::size_t queued_bytes = 0;
	std::uint64_t sent_bytes = 0;
	std::uint64_t reconnect_count = 0;
	std::string error;
	std::uint64_t acknowledged_epoch = 0;
	std::uint64_t published_epoch = 0;
	std::uint64_t delivered_epoch = 0, delivery_ticket = 0;
};

// Factories allocate a fresh transport; they must not perform network I/O.
using RtmpConnectionFactory = std::function<std::unique_ptr<IRtmpConnection>()>;
// Runs on the sender worker. Keep callbacks short; they may request stop but
// must not destroy/restart the owner. External stop joins callbacks before
// releasing transport and callback ownership (no detached workers).
using SenderErrorCallback = std::function<void(const std::string &)>;

class RtmpSender {
public:
	explicit RtmpSender(RtmpConnectionFactory factory, SenderConfig config);
	~RtmpSender();

	RtmpSender(const RtmpSender &) = delete;
	RtmpSender &operator=(const RtmpSender &) = delete;

	bool start(RtmpTarget target, std::vector<FlvTag> sequence_headers, SenderErrorCallback on_error,
		std::string &error);
	bool enqueue(std::vector<FlvTag> tags, std::string &error);
	bool enqueue_epoch(std::vector<FlvTag> tags, std::uint64_t epoch,
		std::vector<FlvTag> headers, std::string &error);
	void invalidate_epoch(std::uint64_t epoch) noexcept;
	void request_stop() noexcept;
	void stop() noexcept;
	[[nodiscard]] SenderStatus status() const;

private:
	void stop_locked() noexcept;
	void run() noexcept;
	bool connect_and_prime(std::string &error);
	bool send_tag(const FlvTag &tag, std::string &error);
	bool send_bytes(std::span<const std::uint8_t> bytes, std::optional<std::uint64_t> epoch, std::string &error);
	void close_transport() noexcept;
	bool take_transport_cancellation() noexcept;
	void end_transition_resume(bool discard_pending) noexcept;
	bool replace_cancelled_transport(std::string &error);
	bool reconnect(std::string &error);
	void fail(std::string error) noexcept;
	[[nodiscard]] bool stop_requested() const noexcept;

	RtmpConnectionFactory factory_;
	SenderConfig config_;
	BoundedSenderQueue queue_;
	std::mutex lifecycle_mutex_;
	mutable std::mutex mutex_;
	std::condition_variable state_changed_;
	std::thread worker_;
	std::shared_ptr<IRtmpConnection> connection_;
	std::thread::id worker_id_;
	RtmpTarget target_;
	std::vector<FlvTag> sequence_headers_;
	SenderErrorCallback on_error_;
	std::uint64_t epoch_ = 0;
	std::uint64_t acknowledged_epoch_ = 0;
	std::uint64_t published_epoch_ = 0;
	std::uint64_t delivered_epoch_ = 0, delivery_ticket_ = 0;
	bool writing_ = false;
	bool transition_cancelled_transport_ = false;
	bool resume_after_transition_ = false;
	std::uint64_t header_revision_ = 0;
	std::uint64_t primed_revision_ = 0;
	SenderState state_ = SenderState::Stopped;
	std::string error_;
	std::atomic_bool stop_requested_ = false;
	std::atomic_bool startup_timed_out_ = false;
	std::chrono::steady_clock::time_point startup_deadline_;
	std::atomic<std::uint64_t> sent_bytes_ = 0;
	std::atomic<std::uint64_t> reconnect_count_ = 0;
};

} // namespace active_delay
