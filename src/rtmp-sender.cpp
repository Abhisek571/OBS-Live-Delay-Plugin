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
	{
		std::scoped_lock lock(mutex_);
		if (worker_id_ == std::this_thread::get_id()) {
			error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "RTMP sender cannot restart from its worker callback");
			return false;
		}
	}
	std::scoped_lock lifecycle_lock(lifecycle_mutex_);
	stop_locked();
	if (sequence_headers.empty()) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "FLV sequence headers are empty");
		return false;
	}

	std::shared_ptr<IRtmpConnection> connection;
	try {
		connection = factory_();
	} catch (const std::exception &exception) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			std::string("Unable to create RTMP connection: ") + exception.what());
		return false;
	}
	if (!connection) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "Unable to create RTMP connection");
		return false;
	}

	sent_bytes_.store(0, std::memory_order_relaxed);
	reconnect_count_.store(0, std::memory_order_relaxed);
	{
		std::scoped_lock lock(mutex_);
		connection_ = std::move(connection);
		queue_.reset();
		stop_requested_.store(false, std::memory_order_release);
		startup_timed_out_.store(false, std::memory_order_release);
		startup_deadline_ = std::chrono::steady_clock::now() + config_.startup_timeout;
		target_ = std::move(target);
		sequence_headers_ = std::move(sequence_headers);
		epoch_ = acknowledged_epoch_ = 0;
		delivered_epoch_ = delivery_ticket_ = 0;
		published_epoch_ = 0;
		writing_ = false;
		transition_cancelled_transport_ = false;
		resume_after_transition_ = false;
		header_revision_ = primed_revision_ = 0;
		on_error_ = std::move(on_error);
		error_.clear();
		state_ = SenderState::Starting;
	}
	try {
		worker_ = std::thread(&RtmpSender::run, this);
	} catch (const std::exception &) {
		stop_locked();
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "Unable to launch RTMP worker");
		return false;
	}
	return true;
}

bool RtmpSender::enqueue(std::vector<FlvTag> tags, std::string &error)
{
	return enqueue_epoch(std::move(tags), 0, {}, error);
}

bool RtmpSender::enqueue_epoch(std::vector<FlvTag> tags, std::uint64_t epoch,
	std::vector<FlvTag> headers, std::string &error)
{
	std::scoped_lock lock(mutex_);
	if (epoch < epoch_)
		return true;
	epoch_ = epoch;
	if (!headers.empty()) {
		sequence_headers_ = headers;
		++header_revision_;
	}
	for (auto &tag : tags)
		tag.epoch = epoch;
	// Disconnected destinations resume at current media, never a stale backlog.
	// A transport replaced for a transition is the exception: the action's own
	// drain guard and boundary must survive it, or the coordinator waits forever.
	const bool transition_resume = state_ == SenderState::Reconnecting && resume_after_transition_;
	if ((state_ == SenderState::Starting || state_ == SenderState::Reconnecting) && !transition_resume)
		return true;
	if (state_ != SenderState::Running && !transition_resume) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			error_.empty() ? "RTMP sender is not running" : error_);
		return false;
	}
	if (!queue_.try_push(std::move(tags))) {
		error = diagnostic_error(DiagnosticCode::RtmpSenderQueueFull,
			"RTMP sender queue reached its bounded capacity");
		return false;
	}
	return true;
}

void RtmpSender::invalidate_epoch(std::uint64_t epoch) noexcept
{
	std::shared_ptr<IRtmpConnection> interrupt;
	{
		std::scoped_lock lock(mutex_);
		if (epoch <= epoch_)
			return;
		epoch_ = epoch;
		queue_.discard_pending();
		// No source remains selected until the coordinator supplies its boundary.
		sequence_headers_.clear();
		++header_revision_;
		if (writing_) {
			transition_cancelled_transport_ = true;
			interrupt = connection_;
		} else
			acknowledged_epoch_ = epoch;
	}
	if (interrupt)
		interrupt->interrupt();
}

void RtmpSender::request_stop() noexcept
{
	std::shared_ptr<IRtmpConnection> connection;
	{
		std::scoped_lock lock(mutex_);
		stop_requested_.store(true, std::memory_order_release);
		if (state_ != SenderState::Stopped)
			state_ = SenderState::Stopping;
		queue_.close(true);
		connection = connection_;
	}
	state_changed_.notify_all();
	if (connection)
		connection->interrupt();
}

void RtmpSender::stop() noexcept
{
	{
		std::unique_lock lock(mutex_);
		if (worker_id_ == std::this_thread::get_id()) {
			lock.unlock();
			request_stop();
			return;
		}
	}
	std::scoped_lock lifecycle_lock(lifecycle_mutex_);
	stop_locked();
}

void RtmpSender::stop_locked() noexcept
{
	request_stop();
	if (worker_.joinable())
		worker_.join();
	std::shared_ptr<IRtmpConnection> connection;
	{
		std::scoped_lock lock(mutex_);
		connection.swap(connection_);
		worker_id_ = {};
		state_ = SenderState::Stopped;
		sequence_headers_.clear();
		on_error_ = {};
	}
	if (connection)
		connection->close();
}

SenderStatus RtmpSender::status() const
{
	const auto queue = queue_.status();
	std::scoped_lock lock(mutex_);
	return {state_, queue.tags, queue.bytes, sent_bytes_.load(std::memory_order_relaxed),
		reconnect_count_.load(std::memory_order_relaxed), error_, acknowledged_epoch_, published_epoch_, delivered_epoch_, delivery_ticket_};
}

void RtmpSender::run() noexcept
{
	{
		std::scoped_lock lock(mutex_);
		worker_id_ = std::this_thread::get_id();
	}
	try {
		std::string error;
		// One wall-clock budget covers connect, header writes and initial retries.
		// The watchdog owns no transport calls except the thread-safe interrupt.
		std::jthread startup_watchdog([this](std::stop_token token) {
			std::stop_callback wake(token, [this] {
				std::scoped_lock lock(mutex_);
				state_changed_.notify_all();
			});
			std::unique_lock lock(mutex_);
			if (state_changed_.wait_until(lock, startup_deadline_, [this, token] {
				return token.stop_requested() || stop_requested();
			}))
				return;
			startup_timed_out_.store(true, std::memory_order_release);
			const auto connection = connection_;
			lock.unlock();
			if (connection) connection->interrupt();
			state_changed_.notify_all();
		});
		const bool connected = connect_and_prime(error) || reconnect(error);
		startup_watchdog.request_stop();
		startup_watchdog.join();
		if (startup_timed_out_.load(std::memory_order_acquire)) {
			error = "RTMP startup deadline was exceeded";
		}
		if (!connected || startup_timed_out_.load(std::memory_order_acquire)) {
			fail(diagnostic_error(DiagnosticCode::RtmpConnectionFailed, error));
			return;
		}
		{
			std::scoped_lock lock(mutex_);
			if (!stop_requested())
				state_ = SenderState::Running;
		}
		state_changed_.notify_all();

		bool awaiting_keyframe = true;
		std::uint32_t audio_cutoff_ms = 0;
		std::uint64_t media_epoch = 0;
		while (!stop_requested()) {
			auto tag = queue_.wait_pop();
			if (!tag || stop_requested())
				break;
			{
				std::scoped_lock lock(mutex_);
				if (tag->epoch != epoch_)
					continue;
			}
			if (media_epoch != tag->epoch) {
				media_epoch = tag->epoch;
				awaiting_keyframe = true;
			}
			std::vector<FlvTag> selected_headers;
			std::uint64_t revision;
			{
				std::scoped_lock lock(mutex_);
				revision = header_revision_;
				if (primed_revision_ != revision)
					selected_headers = sequence_headers_;
			}
			bool primed = true;
			for (auto &header : selected_headers) {
				header.epoch = tag->epoch;
				header.timestamp_ms = tag->timestamp_ms;
				if (!send_tag(header, error)) { primed = false; break; }
			}
			if (!primed) {
				if (!reconnect(error)) { fail(error); return; }
				awaiting_keyframe = true;
				continue;
			}
			{
				std::scoped_lock lock(mutex_);
				if (tag->epoch != epoch_) continue;
				primed_revision_ = revision;
			}
			const bool codec_header = tag->payload.size() > 1 && tag->payload[1] == 0;
			if (codec_header) {
				if (send_tag(*tag, error))
					continue;
			} else {
			if (tag->audio_drain) {
				if (tag->type != FlvTagType::Audio) { fail("TRANSITION_DRAIN_INVALID"); return; }
				if (send_tag(*tag, error)) continue;
				bool superseded;
				{
					std::scoped_lock lock(mutex_);
					superseded = tag->epoch != epoch_;
				}
				// A lost guard cannot safely be acknowledged or replayed after reconnect.
				// A guard that a newer action already superseded is obsolete: only its
				// interrupted transport needs recovery.
				if (!superseded) { fail("TRANSITION_DRAIN_DELIVERY_FAILED"); return; }
			} else {
			if (awaiting_keyframe) {
				if (tag->type != FlvTagType::Video || !tag->keyframe)
					continue;
				awaiting_keyframe = false;
				audio_cutoff_ms = tag->timestamp_ms;
			}
			if (tag->type == FlvTagType::Audio && tag->timestamp_ms < audio_cutoff_ms)
				continue;

			if (send_tag(*tag, error)) {
				if (tag->type == FlvTagType::Audio) {
					std::scoped_lock lock(mutex_);
					if (tag->epoch == epoch_ && !awaiting_keyframe)
						published_epoch_ = tag->epoch;
				}
				continue;
			}
			}
			}
			if (!reconnect(error)) {
				fail(diagnostic_error(DiagnosticCode::RtmpReconnectExhausted, error));
				return;
			}
			awaiting_keyframe = true;
		}
		if (connection_)
			close_transport();
	} catch (const std::exception &exception) {
		fail(diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed,
			std::string("RTMP sender failed: ") + exception.what()));
	} catch (...) {
		fail(diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, "RTMP sender failed with an unknown error"));
	}
}

bool RtmpSender::connect_and_prime(std::string &error)
{
 if (stop_requested() || startup_timed_out_.load(std::memory_order_acquire)) {
  error = diagnostic_error(DiagnosticCode::RtmpConnectionFailed, "RTMP connection was cancelled");
  return false;
 }
 { std::scoped_lock lock(mutex_); writing_ = true; }
 bool connected = false;
 try { connected = connection_->connect(target_, error); }
 catch (...) { std::scoped_lock lock(mutex_); writing_ = false; acknowledged_epoch_ = epoch_; throw; }
 { std::scoped_lock lock(mutex_); writing_ = false; acknowledged_epoch_ = epoch_; }
 if (!connected) return false;
 const auto file_header = make_flv_header();
 if (!send_bytes(file_header, {}, error)) { close_transport(); return false; }
 std::vector<FlvTag> headers;
 std::uint64_t revision;
 {
  std::scoped_lock lock(mutex_);
  headers = sequence_headers_;
  revision = header_revision_;
  for (auto &header : headers) header.epoch = epoch_;
 }
 for (const auto &header : headers) {
  if (!send_tag(header, error)) { close_transport(); return false; }
 }
 { std::scoped_lock lock(mutex_); primed_revision_ = revision; }
 return true;
}

bool RtmpSender::send_tag(const FlvTag &tag, std::string &error)
{
	auto bytes = serialize_flv_tag(tag);
	const bool sent=send_bytes(bytes, tag.epoch, error);
	if(sent && tag.delivery_ticket) {
	 std::scoped_lock lock(mutex_);
	 if(tag.epoch==epoch_ && !stop_requested()) {delivered_epoch_=tag.epoch;delivery_ticket_=tag.delivery_ticket;}
	}
	return sent;
}

bool RtmpSender::send_bytes(std::span<const std::uint8_t> bytes, std::optional<std::uint64_t> epoch, std::string &error)
{
	{
		std::scoped_lock lock(mutex_);
		if ((epoch && *epoch != epoch_) || stop_requested())
			return true;
		writing_ = true;
	}
	bool sent = false;
	try {
		sent = connection_->send(bytes, error);
	} catch (...) {
		std::scoped_lock lock(mutex_);
		writing_ = false;
		acknowledged_epoch_ = epoch_;
		throw;
	}
	{
		std::scoped_lock lock(mutex_);
		writing_ = false;
		acknowledged_epoch_ = epoch_;
	}
	if (sent)
		sent_bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
	return sent;
}

void RtmpSender::close_transport() noexcept
{
 { std::scoped_lock lock(mutex_); writing_ = true; }
 connection_->close();
 { std::scoped_lock lock(mutex_); writing_ = false; acknowledged_epoch_ = epoch_; }
}

bool RtmpSender::take_transport_cancellation() noexcept
{
	std::scoped_lock lock(mutex_);
	return std::exchange(transition_cancelled_transport_, false);
}

bool RtmpSender::replace_cancelled_transport(std::string &error)
{
	if (stop_requested())
		return true;
	// FFmpeg cancellation is sticky. Only its owning worker may close and
	// replace it; never clear cancellation on a concurrently stopped socket.
	std::shared_ptr<IRtmpConnection> replacement = factory_();
	if (!replacement) { error = "TRANSITION_TRANSPORT_REPLACEMENT_FAILED"; return false; }
	std::shared_ptr<IRtmpConnection> previous;
	{
		std::scoped_lock lock(mutex_);
		previous.swap(connection_);
		connection_ = std::move(replacement);
		if (stop_requested() || startup_timed_out_.load(std::memory_order_acquire))
			connection_->interrupt();
	}
	previous.reset(); // destruction/close is never inside the admission mutex
	return true;
}

void RtmpSender::end_transition_resume(bool discard_pending) noexcept
{
	std::scoped_lock lock(mutex_);
	if (resume_after_transition_ && discard_pending)
		queue_.discard_pending();
	resume_after_transition_ = false;
}

bool RtmpSender::reconnect(std::string &error)
{
	bool transition_cancelled = false;
	{
		std::scoped_lock lock(mutex_);
		if (!stop_requested())
			state_ = SenderState::Reconnecting;
		published_epoch_ = 0;
		delivered_epoch_ = delivery_ticket_ = 0;
		// The transition already discarded older epochs; keep its new media.
		resume_after_transition_ = transition_cancelled_transport_;
		if (!resume_after_transition_)
			queue_.discard_pending();
	}
	close_transport();
	transition_cancelled = take_transport_cancellation();
	if (transition_cancelled && !replace_cancelled_transport(error)) {
		end_transition_resume(true);
		return false;
	}
	auto attempts = config_.reconnect_attempts + (transition_cancelled ? 1 : 0);
	for (std::size_t attempt = 0; attempt < attempts && !stop_requested() &&
		!startup_timed_out_.load(std::memory_order_acquire); ++attempt) {
		reconnect_count_.fetch_add(1, std::memory_order_relaxed);
		{
			std::scoped_lock lock(mutex_);
			if (!stop_requested())
				state_ = SenderState::Reconnecting;
		}
		state_changed_.notify_all();

		std::unique_lock lock(mutex_);
		state_changed_.wait_for(lock, transition_cancelled ? std::chrono::milliseconds{0} : config_.reconnect_delay, [this] {
			return stop_requested() || startup_timed_out_.load(std::memory_order_acquire);
		});
		lock.unlock();
		if (stop_requested())
			break;
		if (connect_and_prime(error)) {
			std::scoped_lock state_lock(mutex_);
			if (!stop_requested())
				state_ = SenderState::Running;
			resume_after_transition_ = false;
			return true;
		}
		// A transition, not the network, interrupted this attempt. Retry at once
		// on a fresh transport without spending one of the reconnect attempts.
		transition_cancelled = take_transport_cancellation();
		if (transition_cancelled) {
			if (!replace_cancelled_transport(error)) {
				end_transition_resume(true);
				return false;
			}
			++attempts;
		} else {
			// A real network failure: across retry delays, resume at current
			// media rather than holding a backlog.
			end_transition_resume(true);
		}
	}
	end_transition_resume(false);
	if (error.empty())
		error = diagnostic_error(DiagnosticCode::RtmpReconnectExhausted,
		stop_requested() ? "RTMP reconnect was cancelled" : "RTMP reconnect attempts were exhausted");
	return false;
}

void RtmpSender::fail(std::string error) noexcept
{
	// Only the worker closes the transport. Release it even on exceptions,
	// before notifying an owner that may begin teardown from the callback.
	if (connection_)
		close_transport();
	SenderErrorCallback callback;
	{
		std::scoped_lock lock(mutex_);
		const auto notify_runtime_failure = !stop_requested();
		error_ = std::move(error);
		state_ = notify_runtime_failure ? SenderState::Failed : SenderState::Stopping;
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
