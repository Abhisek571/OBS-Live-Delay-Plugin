#pragma once
#include "flv-muxer.hpp"
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <functional>

namespace active_delay {
enum class HoldingState { Stopped, Starting, Ready, Failed };
struct HoldingStatus {
	HoldingState state = HoldingState::Stopped;
	std::size_t queued_bytes = 0;
	std::int64_t video_cutoff_us = 0;
	std::string error;
};

// Callback-safe compressed feed. Owner drains continuously, even off-air.
// Failure is latched and optionally fences publication; teardown stays owner-only.
class HoldingFeed {
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto readiness_limit = std::chrono::seconds(5);
	static constexpr std::int64_t queue_duration_us = 2'000'000;
	static constexpr std::size_t queue_byte_limit = 16 * 1024 * 1024;
	// Internal failure notification: must only fence publication/latch state,
	// never reenter the feed, join, or destroy capture. Installed before start.
	void set_failure_callback(std::function<void(const std::string &)> callback);
	void begin(Clock::time_point now = Clock::now());
	void set_headers(FlvCodecHeaders headers);
	void ingest(EncodedPacket packet);
	void check_deadline(Clock::time_point now = Clock::now());
	void fail(std::string error);
	void stop();
	[[nodiscard]] HoldingStatus status() const;
	[[nodiscard]] FlvCodecHeaders headers() const;
	std::vector<EncodedPacket> take_packets();
private:
	void update_ready_locked();
	void check_deadline_locked(Clock::time_point now);
	void fail_locked(std::string error);
	mutable std::mutex mutex_;
	HoldingStatus status_;
	std::function<void(const std::string &)> failure_callback_;
	FlvCodecHeaders headers_;
	Clock::time_point deadline_{};
	std::deque<EncodedPacket> packets_;
	bool keyframe_ = false;
	bool audio_ = false;
	std::optional<std::int64_t> minimum_dts_, maximum_dts_;
	std::optional<std::int64_t> last_video_dts_, last_audio_dts_;
};
} // namespace active_delay