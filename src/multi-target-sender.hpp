#pragma once

#include "multistream-config.hpp"
#include "network-packet-consumer.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace active_delay {

struct MultiTargetDestinationStatus {
	std::string id;
	std::string name;
	bool primary = false;
	SenderStatus sender;
};

struct MultiTargetStatus {
	std::vector<MultiTargetDestinationStatus> destinations;
	SenderState aggregate_state = SenderState::Stopped;
	std::uint64_t sent_bytes = 0;
};

// One immutable released-packet batch fans out to independent FLV muxers and
// bounded sender queues.  Secondary errors are contained here; only the
// primary callback is allowed to stop the OBS output.
class MultiTargetSender final : public ReleasedPacketConsumer {
public:
	using PrimaryFailureCallback = std::function<void(const std::string &)>;

	explicit MultiTargetSender(RtmpConnectionFactory factory, SenderConfig config);
	~MultiTargetSender() override;
	bool start(RtmpTarget primary, std::string primary_name, MultistreamConfiguration configuration,
		FlvCodecHeaders headers, PrimaryFailureCallback on_primary_failure, std::string &error);
	void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) override;
	void discontinuity(const PacketDiscontinuity &event) override;
	void stop() noexcept override;
	[[nodiscard]] MultiTargetStatus status() const;
	bool boundary_delivered(std::uint64_t epoch) const override;
	bool delivered(std::uint64_t epoch, std::uint64_t ticket) const override;

private:
	struct Worker {
		std::string id;
		std::string name;
		bool primary = false;
		std::shared_ptr<NetworkPacketConsumer> consumer;
		mutable std::string isolated_error;
		struct DeliveryWait {
			std::uint64_t epoch=0,progress=0;
			std::optional<std::chrono::steady_clock::time_point> since;
			std::chrono::steady_clock::time_point polled{};
		};
		mutable DeliveryWait ticket_wait,boundary_wait;
	};
	bool delivery_ready(std::uint64_t epoch, std::uint64_t ticket, bool boundary) const;

	RtmpConnectionFactory factory_;
	SenderConfig config_;
	mutable std::mutex mutex_;
	std::vector<Worker> workers_;
	std::size_t stops_in_progress_ = 0;
};

} // namespace active_delay
