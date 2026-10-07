#include "network-packet-consumer.hpp"

#include <stdexcept>
#include <utility>

namespace active_delay {

NetworkPacketConsumer::NetworkPacketConsumer(RtmpConnectionFactory factory, SenderConfig config)
	: sender_(std::move(factory), config)
{
}

bool NetworkPacketConsumer::start(RtmpTarget target, FlvCodecHeaders headers, FailureCallback on_failure, std::string &error)
{
	try {
		auto muxer = std::make_unique<FlvMuxer>(std::move(headers));
		if (!sender_.start(std::move(target), muxer->sequence_headers(), std::move(on_failure), error))
			return false;
		std::scoped_lock lock(mutex_);
		muxer_ = std::move(muxer);
		epoch_ = 0;
		needs_headers_ = false;
		return true;
	} catch (const std::exception &exception) {
		error = std::string("Unable to initialize the network packet consumer: ") + exception.what();
		sender_.stop();
		return false;
	}
}

void NetworkPacketConsumer::consume(const std::shared_ptr<const ReleasedPacketBatch> &batch)
{
	if (!batch || batch->packets.empty())
		return;
	std::vector<FlvTag> tags;
	std::string error;
	{
		std::scoped_lock lock(mutex_);
		if (!muxer_)
			throw std::runtime_error("Network packet consumer is not started");
		if (batch->epoch < epoch_)
			return;
		if (epoch_ != 0 && batch->epoch > epoch_)
			throw std::runtime_error("TRANSITION_EPOCH_NOT_COORDINATED");
		epoch_ = batch->epoch;
		if (needs_headers_ && !batch->headers)
			throw std::runtime_error("TRANSITION_CODEC_HEADERS_MISSING");
		std::vector<FlvTag> headers;
		if (batch->headers) {
			muxer_->set_headers(*batch->headers);
			headers = muxer_->sequence_headers();
			needs_headers_ = false;
		}
		// The batch is immutable and shared by the dispatcher. FLV framing is the
		// per-network-consumer representation and necessarily owns its tag bytes.
		if (!muxer_->mux(batch->packets, tags, error))
			throw std::runtime_error(error);
		tags.back().delivery_ticket = batch->delivery_ticket;
		for (auto &header : headers)
			header.timestamp_ms = tags.front().timestamp_ms;
		if (!sender_.enqueue_epoch(std::move(tags), batch->epoch, std::move(headers), error))
			throw std::runtime_error(error);
	}
}

void NetworkPacketConsumer::discontinuity(const PacketDiscontinuity &event)
{
	std::scoped_lock lock(mutex_);
	if (event.epoch <= epoch_)
		return;
	epoch_ = event.epoch;
	needs_headers_ = true;
	sender_.invalidate_epoch(epoch_);
}

void NetworkPacketConsumer::request_stop() noexcept
{
	sender_.request_stop();
}

void NetworkPacketConsumer::stop() noexcept
{
	sender_.stop();
	std::scoped_lock lock(mutex_);
	muxer_.reset();
}

SenderStatus NetworkPacketConsumer::status() const
{
	return sender_.status();
}

} // namespace active_delay
