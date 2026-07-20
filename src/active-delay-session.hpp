#pragma once

#include "delay-controller.hpp"
#include "flv-muxer.hpp"
#include "multistream-config.hpp"
#include "multi-target-sender.hpp"
#include "released-packet-dispatcher.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace active_delay {

enum class OperatingMode { DirectSingle, NativeMultistream, CompatibilitySource };

// These three groups deliberately have separate ownership and locking rules.
// The controller owns delayed media, codec state belongs to capture, and the
// lifecycle owns consumers. They must not be mutated as one giant session blob.
struct ControllerResponsibility {
	DelayController delay;
	std::atomic_bool preserve_on_next_output_start = false;
};

struct CodecResponsibility {
	std::mutex mutex;
	std::optional<FlvCodecHeaders> cached_headers;
};

struct ConsumerLifecycleResponsibility {
	mutable std::mutex mutex;
	OperatingMode mode = OperatingMode::DirectSingle;
	bool active = false;
	std::uint64_t epoch = 0;
	ReleasedPacketDispatcher dispatcher;
};

struct MultistreamResponsibility {
	mutable std::mutex mutex;
	MultistreamConfiguration configuration;
	MultiTargetStatus status;

	[[nodiscard]] MultistreamConfiguration snapshot() const
	{
		std::scoped_lock lock(mutex);
		return configuration;
	}

	void set(MultistreamConfiguration value)
	{
		std::scoped_lock lock(mutex);
		configuration = std::move(value);
	}

	[[nodiscard]] MultiTargetStatus status_snapshot() const
	{
		std::scoped_lock lock(mutex);
		return status;
	}

	void set_status(MultiTargetStatus value)
	{
		std::scoped_lock lock(mutex);
		status = std::move(value);
	}
};

class ActiveDelaySession {
public:
	[[nodiscard]] OperatingMode operating_mode() const
	{
		std::scoped_lock lock(consumers.mutex);
		return consumers.mode;
	}

	bool set_operating_mode(OperatingMode mode, std::string &error)
	{
		std::scoped_lock lock(consumers.mutex);
		if (consumers.active) {
			error = "Operating mode can only change while delayed output is stopped";
			return false;
		}
		consumers.mode = mode;
		return true;
	}

	bool begin_consumer_lifecycle(OperatingMode expected_mode, std::string &error)
	{
		std::scoped_lock lock(consumers.mutex);
		if (consumers.active) {
			error = "Delayed output is already active";
			return false;
		}
		if (consumers.mode != expected_mode) {
			error = "Selected operating mode is not supported by this output";
			return false;
		}
		consumers.active = true;
		return true;
	}

	void end_consumer_lifecycle() noexcept
	{
		std::scoped_lock lock(consumers.mutex);
		consumers.active = false;
	}

	[[nodiscard]] std::uint64_t begin_packet_epoch() noexcept
	{
		std::scoped_lock lock(consumers.mutex);
		return ++consumers.epoch;
	}

	ControllerResponsibility controller;
	CodecResponsibility codec;
	ConsumerLifecycleResponsibility consumers;
	MultistreamResponsibility multistream;
};

} // namespace active_delay
