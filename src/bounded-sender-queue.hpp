#pragma once

#include "flv-muxer.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace active_delay {

struct SenderQueueLimits {
	std::size_t max_tags = 2'048;
	std::size_t max_bytes = 32ULL * 1024ULL * 1024ULL;
};

struct SenderQueueStatus {
	std::size_t tags = 0;
	std::size_t bytes = 0;
	bool closed = false;
};

class BoundedSenderQueue {
public:
	explicit BoundedSenderQueue(SenderQueueLimits limits);

	// Leaves tags untouched when the whole batch does not fit.
	bool try_push(std::vector<FlvTag> &&tags);
	[[nodiscard]] std::optional<FlvTag> wait_pop();
	// Congestion relief: removes queued media video (never codec headers) and
	// moves each removed delivery ticket onto the next kept tag, so a ticket is
	// never acknowledged ahead of the media before it. Returns how many tags
	// were removed; carried_ticket receives a ticket no kept tag could carry.
	std::size_t drop_video(std::uint64_t &carried_ticket) noexcept;
	[[nodiscard]] std::optional<std::uint32_t> front_timestamp_ms() const;
	void close(bool discard_pending) noexcept;
	void discard_pending() noexcept;
	void reset() noexcept;
	[[nodiscard]] SenderQueueStatus status() const;

private:
	SenderQueueLimits limits_;
	mutable std::mutex mutex_;
	std::condition_variable available_;
	std::deque<FlvTag> tags_;
	std::size_t bytes_ = 0;
	bool closed_ = false;
};

} // namespace active_delay
