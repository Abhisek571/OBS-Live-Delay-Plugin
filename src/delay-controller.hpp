#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace active_delay {

using Microseconds = std::chrono::microseconds;

enum class PacketKind { Video, Audio };
enum class DelayState { Live, BuildingDelay, Delayed, ReturningLive, Error };

struct EncodedPacket {
	PacketKind kind = PacketKind::Video;
	std::vector<std::uint8_t> payload;
	std::int64_t pts_us = 0;
	std::int64_t dts_us = 0;
	bool keyframe = false;
};

struct BufferLimits {
	Microseconds max_delay{std::chrono::minutes(10)};
	std::size_t max_bytes = 1024ULL * 1024ULL * 1024ULL;
};

struct ControllerStatus {
	DelayState state = DelayState::Live;
	Microseconds current_delay{};
	Microseconds target_delay{};
	std::size_t buffered_bytes = 0;
	std::string error;
};

class DelayController {
public:
	explicit DelayController(BufferLimits limits = {});

	bool set_target(Microseconds target, std::string *error = nullptr);
	void return_live();
	bool emergency_dump(Microseconds duration, std::string *error = nullptr);
	void reset_for_discontinuity(std::string_view reason);

	// Called from the OBS encoded-output callback. It never performs network I/O.
	void ingest(EncodedPacket packet);
	std::vector<EncodedPacket> take_ready_packets();
	[[nodiscard]] ControllerStatus status() const;

private:
	void promote_locked();
	bool discard_to_next_keyframe_locked(bool allow_current = true);
	bool trim_to_target_locked();
	[[nodiscard]] Microseconds duration_locked() const;
	void set_error_locked(std::string message);

	BufferLimits limits_;
	mutable std::mutex mutex_;
	std::deque<EncodedPacket> buffered_;
	std::deque<EncodedPacket> ready_;
	std::size_t buffered_bytes_ = 0;
	DelayState state_ = DelayState::Live;
	Microseconds target_delay_{};
	std::string error_;
};

} // namespace active_delay
