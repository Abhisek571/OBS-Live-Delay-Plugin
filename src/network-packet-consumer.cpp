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
		// The batch is immutable and shared by the dispatcher. FLV framing is the
		// per-network-consumer representation and necessarily owns its tag bytes.
		if (!muxer_->mux(batch->packets, tags, error))
			throw std::runtime_error(error);
	}
	if (!sender_.enqueue(std::move(tags), error))
		throw std::runtime_error(error);
}

void NetworkPacketConsumer::discontinuity(const PacketDiscontinuity &)
{
	std::scoped_lock lock(mutex_);
	if (muxer_)
		muxer_->reset_timeline();
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
