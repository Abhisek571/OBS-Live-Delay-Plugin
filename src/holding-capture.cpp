#include "holding-capture.hpp"
#include <exception>
#include <utility>

namespace active_delay {
HoldingCapture::HoldingCapture(std::unique_ptr<HoldingCaptureBackend> backend) : backend_(std::move(backend)) {}
HoldingCapture::~HoldingCapture() { stop(); }
void HoldingCapture::cancel_monitor() noexcept
{
	{
		std::scoped_lock lock(monitor_mutex_);
		cancel_ = true;
	}
	wake_.notify_all();
	if (monitor_.joinable()) monitor_.join();
}
void HoldingCapture::cleanup() noexcept
{
	cancel_monitor();
	if (backend_) backend_->stop();
	prepared_ = started_ = false;
}
bool HoldingCapture::prepare(std::string &error)
{
	error.clear();
	if (prepared_ || started_) {
		error = "HOLDING_ALREADY_PREPARED: stop before preparing another capture";
		return false;
	}
	cleanup();
	feed_.begin();
	try {
		if (!backend_ || !backend_->prepare(feed_, error)) {
			if (error.empty()) error = "HOLDING_PREPARE_FAILED: capture resources unavailable";
			feed_.fail(error);
			cleanup();
			return false;
		}
		prepared_ = true;
		return true;
	} catch (...) {
		error = "HOLDING_PREPARE_FAILED: capture resource initialization failed";
		feed_.fail(error); cleanup(); return false;
	}
}
bool HoldingCapture::start(std::string &error)
{
	error.clear();
	if (!prepared_ || started_) {
		error = "HOLDING_NOT_PREPARED: prepare once before start";
		return false;
	}
	// Preparation time is not media readiness time. Start a fresh five-second
	// media deadline before entering the host's potentially blocking start call.
	feed_.begin();
	try {
		{
			std::scoped_lock lock(monitor_mutex_);
			cancel_ = false;
		}
		monitor_ = std::thread([this] {
			std::unique_lock lock(monitor_mutex_);
			while (!wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return cancel_; }))
				feed_.check_deadline();
		});
		if (!backend_->start(error)) {
			if (error.empty()) error = "HOLDING_START_FAILED: capture did not start";
			feed_.fail(error); cleanup(); return false;
		}
		if (feed_.status().state == HoldingState::Failed) {
			error = feed_.status().error;
			cleanup(); return false;
		}
		started_ = true;
		return true;
	} catch (...) {
		error = "HOLDING_START_FAILED: capture startup failed";
		feed_.fail(error); cleanup(); return false;
	}
}
void HoldingCapture::stop() noexcept
{
	// Reject all late packets before unregistering/joining host callbacks.
	feed_.stop();
	cleanup();
}
} // namespace active_delay