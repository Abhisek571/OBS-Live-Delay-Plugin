#include "delay-controller.hpp"

#include <algorithm>

namespace active_delay {
namespace {
constexpr Microseconds kNoDelay{};
}

DelayController::DelayController(BufferLimits limits) : limits_(limits) {}

bool DelayController::set_target(Microseconds target, std::string *error)
{
	std::scoped_lock lock(mutex_);
	if (target < kNoDelay || target > limits_.max_delay) {
		const auto message = "Requested delay is outside the configured maximum";
		if (error)
			*error = message;
		return false;
	}

	error_.clear();
	target_delay_ = target;
	if (target == kNoDelay) {
		state_ = DelayState::ReturningLive;
		buffered_.clear();
		buffered_bytes_ = 0;
		state_ = DelayState::Live;
		return true;
	}

	if (state_ == DelayState::Live || target > duration_locked()) {
		state_ = DelayState::BuildingDelay;
		return true;
	}

	state_ = DelayState::Delayed;
	if (!trim_to_target_locked())
		set_error_locked("Unable to find a safe video keyframe while reducing delay");
	return state_ != DelayState::Error;
}

void DelayController::return_live()
{
	std::scoped_lock lock(mutex_);
	state_ = DelayState::ReturningLive;
	buffered_.clear();
	buffered_bytes_ = 0;
	target_delay_ = kNoDelay;
	error_.clear();
	state_ = DelayState::Live;
}

bool DelayController::emergency_dump(Microseconds duration, std::string *error)
{
	std::scoped_lock lock(mutex_);
	if (state_ != DelayState::Delayed || duration <= kNoDelay) {
		if (error)
			*error = "Emergency dump is available only while delayed";
		return false;
	}
	const auto desired = std::max(kNoDelay, duration_locked() - duration);
	target_delay_ = desired;
	if (!trim_to_target_locked()) {
		if (error)
			*error = "No safe video keyframe is buffered for the requested dump";
		return false;
	}
	return true;
}

void DelayController::reset_for_discontinuity(std::string_view reason)
{
	std::scoped_lock lock(mutex_);
	buffered_.clear();
	ready_.clear();
	buffered_bytes_ = 0;
	target_delay_ = kNoDelay;
	state_ = DelayState::Live;
	error_ = std::string(reason);
}

void DelayController::ingest(EncodedPacket packet)
{
	std::scoped_lock lock(mutex_);
	if (state_ == DelayState::Error)
		return;
	if (state_ == DelayState::Live) {
		ready_.push_back(std::move(packet));
		return;
	}

	buffered_bytes_ += packet.payload.size();
	buffered_.push_back(std::move(packet));
	if (buffered_bytes_ > limits_.max_bytes) {
		set_error_locked("Encoded packet buffer reached its memory limit");
		return;
	}
	promote_locked();
}

std::vector<EncodedPacket> DelayController::take_ready_packets()
{
	std::scoped_lock lock(mutex_);
	std::vector<EncodedPacket> result;
	result.reserve(ready_.size());
	while (!ready_.empty()) {
		result.emplace_back(std::move(ready_.front()));
		ready_.pop_front();
	}
	return result;
}

ControllerStatus DelayController::status() const
{
	std::scoped_lock lock(mutex_);
	return {state_, duration_locked(), target_delay_, buffered_bytes_, error_};
}

void DelayController::promote_locked()
{
	if (state_ == DelayState::BuildingDelay && duration_locked() >= target_delay_) {
		if (!discard_to_next_keyframe_locked()) {
			set_error_locked("No video keyframe is available to begin delayed playback");
			return;
		}
		state_ = DelayState::Delayed;
	}

	if (state_ == DelayState::Delayed) {
		while (duration_locked() > target_delay_ && !buffered_.empty()) {
			ready_.push_back(std::move(buffered_.front()));
			buffered_bytes_ -= buffered_.front().payload.size();
			buffered_.pop_front();
		}
	}
}

bool DelayController::discard_to_next_keyframe_locked(bool allow_current)
{
	auto search_start = buffered_.begin();
	if (!allow_current && search_start != buffered_.end() && search_start->kind == PacketKind::Video && search_start->keyframe)
		++search_start;
	auto keyframe = std::find_if(search_start, buffered_.end(), [](const EncodedPacket &packet) {
		return packet.kind == PacketKind::Video && packet.keyframe;
	});
	if (keyframe == buffered_.end())
		return false;

	const auto start = keyframe->dts_us;
	while (!buffered_.empty() && buffered_.front().dts_us < start) {
		buffered_bytes_ -= buffered_.front().payload.size();
		buffered_.pop_front();
	}
	return true;
}

bool DelayController::trim_to_target_locked()
{
	while (duration_locked() > target_delay_) {
		if (!discard_to_next_keyframe_locked(false))
			return false;
	}
	return true;
}

Microseconds DelayController::duration_locked() const
{
	if (buffered_.size() < 2)
		return kNoDelay;
	return Microseconds{std::max<std::int64_t>(0, buffered_.back().dts_us - buffered_.front().dts_us)};
}

void DelayController::set_error_locked(std::string message)
{
	state_ = DelayState::Error;
	error_ = std::move(message);
}

} // namespace active_delay
