#include "bounded-sender-queue.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace active_delay {

BoundedSenderQueue::BoundedSenderQueue(SenderQueueLimits limits) : limits_(limits)
{
	if (limits_.max_tags == 0 || limits_.max_bytes == 0)
		throw std::invalid_argument("Sender queue limits must be non-zero");
}

bool BoundedSenderQueue::try_push(std::vector<FlvTag> tags)
{
	std::size_t added_bytes = 0;
	for (const auto &tag : tags) {
		const auto size = tag.wire_size();
		if (size > std::numeric_limits<std::size_t>::max() - added_bytes)
			return false;
		added_bytes += size;
	}

	std::scoped_lock lock(mutex_);
	if (closed_ || tags.size() > limits_.max_tags - std::min(limits_.max_tags, tags_.size()) ||
	    added_bytes > limits_.max_bytes - std::min(limits_.max_bytes, bytes_))
		return false;
	for (auto &tag : tags)
		tags_.emplace_back(std::move(tag));
	bytes_ += added_bytes;
	available_.notify_one();
	return true;
}

std::optional<FlvTag> BoundedSenderQueue::wait_pop()
{
	std::unique_lock lock(mutex_);
	available_.wait(lock, [this] { return closed_ || !tags_.empty(); });
	if (tags_.empty())
		return std::nullopt;
	auto tag = std::move(tags_.front());
	tags_.pop_front();
	bytes_ -= tag.wire_size();
	return tag;
}

void BoundedSenderQueue::close(bool discard_pending) noexcept
{
	{
		std::scoped_lock lock(mutex_);
		closed_ = true;
		if (discard_pending) {
			tags_.clear();
			bytes_ = 0;
		}
	}
	available_.notify_all();
}

void BoundedSenderQueue::reset() noexcept
{
	std::scoped_lock lock(mutex_);
	tags_.clear();
	bytes_ = 0;
	closed_ = false;
}

SenderQueueStatus BoundedSenderQueue::status() const
{
	std::scoped_lock lock(mutex_);
	return {tags_.size(), bytes_, closed_};
}

} // namespace active_delay
