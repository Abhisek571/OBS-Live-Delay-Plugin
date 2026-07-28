#include "delay-controller.hpp"

#include "diagnostic-error.hpp"

#include <algorithm>
#include <limits>

namespace active_delay {
namespace {
constexpr Microseconds kNoDelay{};

bool checked_add(std::int64_t left, std::int64_t right, std::int64_t &result)
{
	if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
	    (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
		return false;
	result = left + right;
	return true;
}

bool checked_subtract(std::int64_t left, std::int64_t right, std::int64_t &result)
{
	if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
	    (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right))
		return false;
	result = left - right;
	return true;
}
}

DelayController::DelayController() : DelayController(BufferLimits{}) {}

DelayController::DelayController(BufferLimits limits) : limits_(limits) {}

bool DelayController::set_target(Microseconds target)
{
	return set_target(target, nullptr);
}

bool DelayController::set_target(Microseconds target, std::string *error)
{
	std::scoped_lock lock(mutex_);
	if (target < kNoDelay || target > limits_.max_delay) {
		const auto message = diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Requested delay is outside the configured maximum");
		if (error)
			*error = message;
		return false;
	}

	error_.clear();
	target_delay_ = target;
	if (target == kNoDelay) {
		state_ = DelayState::ReturningLive;
		buffered_.clear();
		ready_.clear();
		buffered_bytes_ = 0;
		reset_timestamp_tracking_locked();
		state_ = DelayState::Live;
		return true;
	}

	if (state_ == DelayState::Live) {
		ready_.clear();
		state_ = DelayState::BuildingDelay;
		return true;
	}
	if (target > duration_locked()) {
		state_ = DelayState::BuildingDelay;
		return true;
	}

	state_ = DelayState::Delayed;
	if (!trim_to_target_locked())
		set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Unable to find a safe video keyframe while reducing delay"));
	return state_ != DelayState::Error;
}

void DelayController::return_live()
{
	std::scoped_lock lock(mutex_);
	state_ = DelayState::ReturningLive;
	buffered_.clear();
	ready_.clear();
	buffered_bytes_ = 0;
	target_delay_ = kNoDelay;
	error_.clear();
	reset_timestamp_tracking_locked();
	state_ = DelayState::Live;
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
	reset_timestamp_tracking_locked();
}

void DelayController::begin_timestamp_epoch()
{
	std::scoped_lock lock(mutex_);
	timestamp_offset_us_.reset();
	rebase_next_timestamp_ = true;
}

void DelayController::ingest(EncodedPacket packet)
{
	std::scoped_lock lock(mutex_);
	if (state_ == DelayState::Error)
		return;
	if (!normalize_timestamp_locked(packet))
		return;
	if (state_ == DelayState::Live) {
		ready_.push_back(std::move(packet));
		return;
	}
	if (!buffered_.empty() && packet.dts_us < buffered_.back().dts_us) {
		set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Encoder packet timestamps moved backwards"));
		return;
	}

	buffered_bytes_ += packet.payload.size();
	buffered_.push_back(std::move(packet));
	if (buffered_bytes_ > limits_.max_bytes) {
		set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Encoded packet buffer reached its memory limit"));
		return;
	}
	promote_locked();
}

bool DelayController::normalize_timestamp_locked(EncodedPacket &packet)
{
	if (rebase_next_timestamp_) {
		rebase_next_timestamp_ = false;
		if (last_input_dts_us_) {
			std::int64_t epoch_start = 0;
			std::int64_t offset = 0;
			if (!checked_add(*last_input_dts_us_, 1, epoch_start) ||
			    !checked_subtract(epoch_start, packet.dts_us, offset)) {
				set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
					"Encoder timestamp epoch cannot be represented safely"));
				return false;
			}
			timestamp_offset_us_ = offset;
		}
	}

	if (timestamp_offset_us_) {
		std::int64_t normalized_dts = 0;
		std::int64_t normalized_pts = 0;
		if (!checked_add(packet.dts_us, *timestamp_offset_us_, normalized_dts) ||
		    !checked_add(packet.pts_us, *timestamp_offset_us_, normalized_pts)) {
				set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
					"Encoder timestamps overflowed while joining a restarted epoch"));
			return false;
		}
		packet.dts_us = normalized_dts;
		packet.pts_us = normalized_pts;
	}

	last_input_dts_us_ = packet.dts_us;
	return true;
}

void DelayController::reset_timestamp_tracking_locked()
{
	last_input_dts_us_.reset();
	timestamp_offset_us_.reset();
	rebase_next_timestamp_ = false;
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
			set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
				"No video keyframe is available to begin delayed playback"));
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
