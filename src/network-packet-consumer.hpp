#pragma once

#include "released-packet-dispatcher.hpp"
#include "rtmp-sender.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace active_delay {

// Direct Single's existing FLV/RTMP work, expressed as a dispatcher consumer.
// Native fan-out can construct one instance per target without changing capture
// or delay-controller ownership.
class NetworkPacketConsumer final : public ReleasedPacketConsumer {
public:
	using FailureCallback = std::function<void(const std::string &)>;

	explicit NetworkPacketConsumer(RtmpConnectionFactory factory, SenderConfig config);

	bool start(RtmpTarget target, FlvCodecHeaders headers, FailureCallback on_failure, std::string &error);
	void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) override;
	void discontinuity(const PacketDiscontinuity &event) override;
	void stop() noexcept override;
	[[nodiscard]] SenderStatus status() const;

private:
	mutable std::mutex mutex_;
	std::unique_ptr<FlvMuxer> muxer_;
	RtmpSender sender_;
};

} // namespace active_delay
