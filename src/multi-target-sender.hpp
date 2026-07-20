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

	explicit MultiTargetSender(RtmpConnectionFactory factory, SenderConfig config = {});
	bool start(RtmpTarget primary, std::string primary_name, MultistreamConfiguration configuration,
		FlvCodecHeaders headers, PrimaryFailureCallback on_primary_failure, std::string &error);
	void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) override;
	void discontinuity(const PacketDiscontinuity &event) override;
	void stop() noexcept override;
	[[nodiscard]] MultiTargetStatus status() const;

private:
	struct Worker {
		std::string id;
		std::string name;
		bool primary = false;
		std::shared_ptr<NetworkPacketConsumer> consumer;
		std::string isolated_error;
	};

	RtmpConnectionFactory factory_;
	SenderConfig config_;
	mutable std::mutex mutex_;
	std::vector<Worker> workers_;
};

} // namespace active_delay
