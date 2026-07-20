#pragma once

#include "delay-controller.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace active_delay {

// A batch owns encoded payloads once. Consumers receive a shared immutable view,
// so adding a destination does not copy delayed media at the fan-out boundary.
struct ReleasedPacketBatch {
	std::uint64_t epoch = 0;
	std::vector<EncodedPacket> packets;
};

struct PacketDiscontinuity {
	std::uint64_t epoch = 0;
	std::string reason;
};

class ReleasedPacketConsumer {
public:
	virtual ~ReleasedPacketConsumer() = default;
	virtual void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) = 0;
	virtual void discontinuity(const PacketDiscontinuity &event) = 0;
	virtual void stop() noexcept = 0;
};

struct ConsumerFailure {
	std::uint64_t consumer_id = 0;
	std::string error;
};

class ReleasedPacketDispatcher {
public:
	using ConsumerId = std::uint64_t;

	ConsumerId add_consumer(std::shared_ptr<ReleasedPacketConsumer> consumer);
	void remove_consumer(ConsumerId id) noexcept;
	[[nodiscard]] std::vector<ConsumerFailure> dispatch(std::shared_ptr<const ReleasedPacketBatch> batch) const;
	[[nodiscard]] std::vector<ConsumerFailure> dispatch_discontinuity(PacketDiscontinuity event) const;
	void stop_all() noexcept;
	[[nodiscard]] std::size_t consumer_count() const noexcept;

private:
	using ConsumerEntry = std::pair<ConsumerId, std::shared_ptr<ReleasedPacketConsumer>>;
	[[nodiscard]] std::vector<ConsumerEntry> snapshot_consumers() const;

	mutable std::mutex mutex_;
	std::vector<ConsumerEntry> consumers_;
	ConsumerId next_consumer_id_ = 1;
};

} // namespace active_delay
