#include "released-packet-dispatcher.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace active_delay {

ReleasedPacketDispatcher::ConsumerId ReleasedPacketDispatcher::add_consumer(std::shared_ptr<ReleasedPacketConsumer> consumer)
{
	if (!consumer)
		throw std::invalid_argument("Released packet consumer is null");
	std::scoped_lock lock(mutex_);
	const auto id = next_consumer_id_++;
	consumers_.emplace_back(id, std::move(consumer));
	return id;
}

void ReleasedPacketDispatcher::remove_consumer(ConsumerId id) noexcept
{
	std::shared_ptr<ReleasedPacketConsumer> removed;
	{
		std::scoped_lock lock(mutex_);
		const auto found = std::find_if(consumers_.begin(), consumers_.end(),
			[id](const auto &entry) { return entry.first == id; });
		if (found == consumers_.end())
			return;
		removed = std::move(found->second);
		consumers_.erase(found);
	}
	if (removed)
		removed->stop();
}

std::vector<ConsumerFailure> ReleasedPacketDispatcher::dispatch(std::shared_ptr<const ReleasedPacketBatch> batch) const
{
	if (!batch)
		throw std::invalid_argument("Released packet batch is null");
	std::vector<ConsumerFailure> failures;
	for (const auto &[id, consumer] : snapshot_consumers()) {
		try {
			consumer->consume(batch);
		} catch (const std::exception &exception) {
			failures.push_back({id, exception.what()});
		} catch (...) {
			failures.push_back({id, "Packet consumer failed with an unknown error"});
		}
	}
	return failures;
}

std::vector<ConsumerFailure> ReleasedPacketDispatcher::dispatch_discontinuity(PacketDiscontinuity event) const
{
	std::vector<ConsumerFailure> failures;
	for (const auto &[id, consumer] : snapshot_consumers()) {
		try {
			consumer->discontinuity(event);
		} catch (const std::exception &exception) {
			failures.push_back({id, exception.what()});
		} catch (...) {
			failures.push_back({id, "Packet consumer discontinuity handling failed"});
		}
	}
	return failures;
}

void ReleasedPacketDispatcher::stop_all() noexcept
{
	std::vector<ConsumerEntry> consumers;
	{
		std::scoped_lock lock(mutex_);
		consumers.swap(consumers_);
	}
	for (auto &[id, consumer] : consumers) {
		(void)id;
		if (consumer)
			consumer->stop();
	}
}

std::size_t ReleasedPacketDispatcher::consumer_count() const noexcept
{
	std::scoped_lock lock(mutex_);
	return consumers_.size();
}

std::vector<ReleasedPacketDispatcher::ConsumerEntry> ReleasedPacketDispatcher::snapshot_consumers() const
{
	std::scoped_lock lock(mutex_);
	return consumers_;
}

} // namespace active_delay
