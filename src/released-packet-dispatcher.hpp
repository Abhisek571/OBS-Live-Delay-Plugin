#pragma once

#include "delay-controller.hpp"
#include "flv-muxer.hpp"
#include <optional>
#include <atomic>

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
	std::optional<FlvCodecHeaders> headers;
	std::uint64_t delivery_ticket = 0;
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
	// True only after the new source keyframe and post-cutoff audio are written.
	virtual bool boundary_delivered(std::uint64_t) const { return true; }
	virtual bool delivered(std::uint64_t, std::uint64_t) const { return true; }
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
	[[nodiscard]] bool delivered(std::uint64_t epoch, std::uint64_t ticket) const;
	[[nodiscard]] std::size_t consumer_count() const noexcept;
	// One publication floor spans owner requests and coordinator feed changes.
	// Reserve at action admission, before returning success to the caller.
	// An internal feed change must CAS its current epoch, never leap over a
	// concurrently accepted action. This fence cannot recall entered I/O.
	void observe_epoch(std::uint64_t epoch) const noexcept;
	[[nodiscard]] std::uint64_t publication_epoch() const noexcept { return publication_epoch_.load(); }
	[[nodiscard]] std::uint64_t reserve_epoch(std::uint64_t after);
	[[nodiscard]] bool advance_epoch(std::uint64_t expected);

private:
	using ConsumerEntry = std::pair<ConsumerId, std::shared_ptr<ReleasedPacketConsumer>>;
	[[nodiscard]] std::vector<ConsumerEntry> snapshot_consumers() const;

	mutable std::mutex mutex_;
	std::vector<ConsumerEntry> consumers_;
	ConsumerId next_consumer_id_ = 1;
	mutable std::atomic<std::uint64_t> publication_epoch_{0};
};

} // namespace active_delay
