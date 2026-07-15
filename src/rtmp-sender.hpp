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
	std::string error;
};

using RtmpConnectionFactory = std::function<std::unique_ptr<IRtmpConnection>()>;
using SenderErrorCallback = std::function<void(const std::string &)>;

class RtmpSender {
public:
	explicit RtmpSender(RtmpConnectionFactory factory, SenderConfig config = {});
	~RtmpSender();

	RtmpSender(const RtmpSender &) = delete;
	RtmpSender &operator=(const RtmpSender &) = delete;

	bool start(RtmpTarget target, std::vector<FlvTag> sequence_headers, SenderErrorCallback on_error,
		std::string &error);
	bool enqueue(std::vector<FlvTag> tags, std::string &error);
	void stop() noexcept;
	[[nodiscard]] SenderStatus status() const;

private:
	void run() noexcept;
	bool connect_and_prime(std::string &error);
	bool send_tag(const FlvTag &tag, std::string &error);
	bool reconnect(std::string &error);
	void fail(std::string error) noexcept;
	[[nodiscard]] bool stop_requested() const noexcept;

	RtmpConnectionFactory factory_;
	SenderConfig config_;
	BoundedSenderQueue queue_;
	mutable std::mutex mutex_;
	std::condition_variable state_changed_;
	std::thread worker_;
	std::unique_ptr<IRtmpConnection> connection_;
	RtmpTarget target_;
	std::vector<FlvTag> sequence_headers_;
	SenderErrorCallback on_error_;
	SenderState state_ = SenderState::Stopped;
	std::string error_;
	std::atomic_bool stop_requested_ = false;
	std::atomic<std::uint64_t> sent_bytes_ = 0;
};

} // namespace active_delay
