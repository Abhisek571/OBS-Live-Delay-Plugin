#include "released-packet-dispatcher.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <limits>

namespace active_delay {

void ReleasedPacketDispatcher::observe_epoch(std::uint64_t epoch) const noexcept
{
	auto previous=publication_epoch_.load();
	while(previous<epoch && !publication_epoch_.compare_exchange_weak(previous,epoch)) {}
}

std::uint64_t ReleasedPacketDispatcher::reserve_epoch(std::uint64_t after)
{
	auto previous=publication_epoch_.load();
	for(;;) {
		const auto base=std::max(previous,after);
		if(base==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("TRANSITION_EPOCH_OVERFLOW");
		if(publication_epoch_.compare_exchange_weak(previous,base+1))return base+1;
	}
}

bool ReleasedPacketDispatcher::advance_epoch(std::uint64_t expected)
{
	if(expected==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("TRANSITION_EPOCH_OVERFLOW");
	return publication_epoch_.compare_exchange_strong(expected,expected+1);
}

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
			if(batch->epoch<publication_epoch_.load())break;
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
	observe_epoch(event.epoch);
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

bool ReleasedPacketDispatcher::delivered(std::uint64_t epoch, std::uint64_t ticket) const
{
 if(epoch!=publication_epoch_.load())return false;
 for(const auto &[id,consumer]:snapshot_consumers())
  if(!consumer->delivered(epoch,ticket))return false;
 return true;
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
