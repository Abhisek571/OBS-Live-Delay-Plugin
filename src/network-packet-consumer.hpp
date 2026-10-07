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
	void request_stop() noexcept;
	void stop() noexcept override;
	[[nodiscard]] SenderStatus status() const;
	bool boundary_delivered(std::uint64_t epoch) const override { return sender_.status().published_epoch >= epoch; }
	bool delivered(std::uint64_t epoch, std::uint64_t ticket) const override {
	 const auto s=sender_.status();return s.state==SenderState::Running && s.delivered_epoch==epoch && s.delivery_ticket>=ticket;
	}

private:
	mutable std::mutex mutex_;
	std::unique_ptr<FlvMuxer> muxer_;
	RtmpSender sender_;
	std::uint64_t epoch_ = 0;
	bool needs_headers_ = false;
};

} // namespace active_delay
