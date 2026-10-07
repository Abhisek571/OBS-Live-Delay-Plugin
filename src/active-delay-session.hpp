#pragma once

#include "delay-controller.hpp"
#include "multistream-config.hpp"
#include "multi-target-sender.hpp"
#include "released-packet-dispatcher.hpp"
#include "pipeline-owner.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace active_delay {

enum class OperatingMode { DirectSingle, NativeMultistream, CompatibilitySource };

// These groups deliberately have separate ownership and locking rules. The
// controller owns delayed media, and the lifecycle owns consumers. They must
// not be mutated as one giant session blob.
struct ControllerResponsibility {
	DelayController delay;
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
	bool set_prebuffer_target(Microseconds target, std::string &error) {
		std::scoped_lock lock(consumers.mutex);
		if (consumers.active || target <= Microseconds{} || target > std::chrono::minutes(10)) {
			error = "PREBUFFER_TARGET_INVALID: disarm before selecting a positive delay (maximum 600 seconds)";
			return false;
		}
		prebuffer_target_ = target; return true;
	}
	Microseconds prebuffer_target() const { std::scoped_lock lock(consumers.mutex); return prebuffer_target_; }
	bool start_broadcast(std::string &error) {
		std::shared_ptr<PipelineOwner> owner;
		{ std::scoped_lock lock(consumers.mutex); owner = pipeline_.lock(); }
		if (!owner) { error = "PREBUFFER_NOT_ARMED: arm the buffer first"; return false; }
		return owner->start_broadcast(error);
	}
	void attach_pipeline(std::shared_ptr<PipelineOwner> owner) {
		std::scoped_lock lock(consumers.mutex);
		pipeline_ = std::move(owner);
	}
	bool request_transition(TransitionRequest request, std::string &error) {
		std::shared_ptr<PipelineOwner> owner;
		{ std::scoped_lock lock(consumers.mutex); owner = pipeline_.lock(); }
		if (!owner) { error = "PIPELINE_NOT_ACTIVE: transition unavailable"; return false; }
		return owner->request(request, error);
	}
	void set_holding_scene(std::string name) { std::scoped_lock lock(consumers.mutex); holding_scene_ = std::move(name); }
	PipelineStatus pipeline_status() const {
		std::shared_ptr<PipelineOwner> owner;
		{ std::scoped_lock lock(consumers.mutex); owner = pipeline_.lock(); }
		return owner ? owner->status() : PipelineStatus{};
	}
	void refresh_network_status() {
		std::shared_ptr<PipelineOwner> owner;
		{ std::scoped_lock lock(consumers.mutex); owner = pipeline_.lock(); }
		if (!owner) return;
		if (auto multi = std::dynamic_pointer_cast<MultiTargetSender>(owner->consumer_snapshot()))
			multistream.set_status(multi->status());
	}
	std::string holding_scene() const { std::scoped_lock lock(consumers.mutex); return holding_scene_; }
	ConsumerLifecycleResponsibility consumers;
	MultistreamResponsibility multistream;
private:
	std::weak_ptr<PipelineOwner> pipeline_;
	Microseconds prebuffer_target_{};
	std::string holding_scene_;
};

} // namespace active_delay
