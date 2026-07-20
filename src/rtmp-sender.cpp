#include "rtmp-sender.hpp"

#include "diagnostic-error.hpp"
#include "flv-muxer.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace active_delay {

RtmpSender::RtmpSender(RtmpConnectionFactory factory, SenderConfig config)
	: factory_(std::move(factory)), config_(config), queue_(config.queue)
{
	if (!factory_)
		throw std::invalid_argument("RTMP connection factory is empty");
}

RtmpSender::~RtmpSender()
{
	stop();
}

bool RtmpSender::start(RtmpTarget target, std::vector<FlvTag> sequence_headers, SenderErrorCallback on_error,
	std::string &error)
{
	stop();
	if (sequence_headers.empty()) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "FLV sequence headers are empty");
		return false;
	}

	try {
		connection_ = factory_();
	} catch (const std::exception &exception) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			std::string("Unable to create RTMP connection: ") + exception.what());
		return false;
	}
	if (!connection_) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "Unable to create RTMP connection");
		return false;
	}

	queue_.reset();
	stop_requested_.store(false, std::memory_order_release);
	sent_bytes_.store(0, std::memory_order_relaxed);
	{
		std::scoped_lock lock(mutex_);
		target_ = std::move(target);
		sequence_headers_ = std::move(sequence_headers);
		on_error_ = std::move(on_error);
		error_.clear();
		state_ = SenderState::Starting;
	}
	worker_ = std::thread(&RtmpSender::run, this);

	std::unique_lock lock(mutex_);
	const auto started = state_changed_.wait_for(lock, config_.startup_timeout, [this] {
		return state_ != SenderState::Starting;
	});
	if (started && state_ == SenderState::Running)
		return true;
	if (started)
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			error_.empty() ? "RTMP sender failed to start" : error_);
	else
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			"Timed out while connecting to the RTMP server");
	lock.unlock();
	stop();
	return false;
}

bool RtmpSender::enqueue(std::vector<FlvTag> tags, std::string &error)
{
	{
		std::scoped_lock lock(mutex_);
		if (state_ != SenderState::Running && state_ != SenderState::Reconnecting) {
			error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
				error_.empty() ? "RTMP sender is not running" : error_);
			return false;
		}
	}
	if (!queue_.try_push(std::move(tags))) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderQueueFull,
			"RTMP sender queue reached its bounded capacity");
		return false;
	}
	return true;
}

void RtmpSender::stop() noexcept
{
	stop_requested_.store(true, std::memory_order_release);
	queue_.close(true);
	state_changed_.notify_all();
	if (connection_)
		connection_->interrupt();
	if (worker_.joinable()) {
		if (worker_.get_id() == std::this_thread::get_id())
			return;
		worker_.join();
	}
	if (connection_) {
		connection_->close();
		connection_.reset();
	}
	{
		std::scoped_lock lock(mutex_);
		state_ = SenderState::Stopped;
		sequence_headers_.clear();
		on_error_ = {};
	}
}

SenderStatus RtmpSender::status() const
{
	const auto queue = queue_.status();
	std::scoped_lock lock(mutex_);
	return {state_, queue.tags, queue.bytes, sent_bytes_.load(std::memory_order_relaxed), error_};
}

void RtmpSender::run() noexcept
{
	try {
		std::string error;
		if (!connect_and_prime(error)) {
			fail(diagnostic_error(DiagnosticCode::RtmpConnectionFailed, error));
			return;
		}
		{
			std::scoped_lock lock(mutex_);
			state_ = SenderState::Running;
		}
		state_changed_.notify_all();

		bool awaiting_keyframe = true;
		while (!stop_requested()) {
			auto tag = queue_.wait_pop();
			if (!tag)
				break;
			if (awaiting_keyframe) {
				if (tag->type != FlvTagType::Video || !tag->keyframe)
					continue;
				awaiting_keyframe = false;
			}

			if (send_tag(*tag, error))
				continue;
			if (!reconnect(error)) {
				fail(diagnostic_error(DiagnosticCode::RtmpReconnectExhausted, error));
				return;
			}
			awaiting_keyframe = true;
		}
		if (connection_)
			connection_->close();
	} catch (const std::exception &exception) {
		fail(diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			std::string("RTMP sender failed: ") + exception.what()));
	} catch (...) {
		fail(diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "RTMP sender failed with an unknown error"));
	}
}

bool RtmpSender::connect_and_prime(std::string &error)
{
	if (stop_requested()) {
		error = diagnostic_error(DiagnosticCode::RtmpConnectionFailed, "RTMP connection was cancelled");
		return false;
	}
	if (!connection_->connect(target_, error))
		return false;

	auto header = make_flv_header();
	if (!connection_->send(header, error)) {
		connection_->close();
		return false;
	}
	sent_bytes_.fetch_add(header.size(), std::memory_order_relaxed);
	for (const auto &tag : sequence_headers_) {
		auto bytes = serialize_flv_tag(tag);
		if (!connection_->send(bytes, error)) {
			connection_->close();
			return false;
		}
		sent_bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
	}
	return true;
}

bool RtmpSender::send_tag(const FlvTag &tag, std::string &error)
{
	auto bytes = serialize_flv_tag(tag);
	if (!connection_->send(bytes, error))
		return false;
	sent_bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
	return true;
}

bool RtmpSender::reconnect(std::string &error)
{
	connection_->close();
	for (std::size_t attempt = 0; attempt < config_.reconnect_attempts && !stop_requested(); ++attempt) {
		{
			std::scoped_lock lock(mutex_);
			state_ = SenderState::Reconnecting;
		}
		state_changed_.notify_all();

		std::unique_lock lock(mutex_);
		state_changed_.wait_for(lock, config_.reconnect_delay, [this] { return stop_requested(); });
		lock.unlock();
		if (stop_requested())
			break;
		if (connect_and_prime(error)) {
			std::scoped_lock state_lock(mutex_);
			state_ = SenderState::Running;
			return true;
		}
	}
	if (error.empty())
		error = diagnostic_error(DiagnosticCode::RtmpReconnectExhausted,
		stop_requested() ? "RTMP reconnect was cancelled" : "RTMP reconnect attempts were exhausted");
	return false;
}

void RtmpSender::fail(std::string error) noexcept
{
	SenderErrorCallback callback;
	{
		std::scoped_lock lock(mutex_);
		const auto notify_runtime_failure = state_ != SenderState::Starting;
		error_ = std::move(error);
		state_ = SenderState::Failed;
		if (notify_runtime_failure)
			callback = on_error_;
	}
	queue_.close(true);
	state_changed_.notify_all();
	if (callback) {
		try {
			std::string callback_error;
			{
				std::scoped_lock lock(mutex_);
				callback_error = error_;
			}
			callback(callback_error);
		} catch (...) {
		}
	}
}

bool RtmpSender::stop_requested() const noexcept
{
	return stop_requested_.load(std::memory_order_acquire);
}

} // namespace active_delay
