#pragma once
#include "holding-feed.hpp"
#include <condition_variable>
#include <memory>
#include <thread>

namespace active_delay {
// Backend callbacks may touch the feed only. stop() must quiesce callbacks
// before releasing resources and must also unwind partially prepared state.
class HoldingCaptureBackend {
public:
	virtual ~HoldingCaptureBackend() = default;
	virtual bool prepare(HoldingFeed &feed, std::string &error) = 0;
	virtual bool start(std::string &error) = 0;
	virtual void stop() noexcept = 0;
};

// Owner serializes prepare/start/stop/destruction off encoder callbacks.
// status/headers/take_packets are thread-safe. The internal failure notification
// only retires publication; the coordinator owns deferred teardown.
class HoldingCapture {
public:
	explicit HoldingCapture(std::unique_ptr<HoldingCaptureBackend> backend);
	~HoldingCapture();
	HoldingCapture(const HoldingCapture &) = delete;
	HoldingCapture &operator=(const HoldingCapture &) = delete;
	void set_failure_callback(std::function<void(const std::string &)> callback) { feed_.set_failure_callback(std::move(callback)); }
	bool prepare(std::string &error);
	bool start(std::string &error);
	void stop() noexcept;
	[[nodiscard]] HoldingStatus status() const { return feed_.status(); }
	[[nodiscard]] FlvCodecHeaders headers() const { return feed_.headers(); }
	std::vector<EncodedPacket> take_packets() { return feed_.take_packets(); }
private:
	void cancel_monitor() noexcept;
	void cleanup() noexcept;
	HoldingFeed feed_;
	std::unique_ptr<HoldingCaptureBackend> backend_;
	std::thread monitor_;
	std::mutex monitor_mutex_;
	std::condition_variable wake_;
	bool cancel_ = false;
	bool prepared_ = false, started_ = false;
};
} // namespace active_delay