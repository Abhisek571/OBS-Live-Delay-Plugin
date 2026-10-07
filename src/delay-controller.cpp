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

bool DelayController::arm(Microseconds target, std::string &error)
{
	std::scoped_lock lock(mutex_);
	if (target <= kNoDelay || target > limits_.max_delay) {
		error = "PREBUFFER_TARGET_INVALID: choose a positive delay within the configured maximum";
		return false;
	}
	buffered_.clear(); ready_.clear(); buffered_bytes_ = 0;
	reset_timestamp_tracking_locked();
	video_watermark_.reset(); audio_watermark_.reset(); playback_delay_.reset();
	target_delay_ = target; armed_ = true; state_ = DelayState::BuildingDelay; error_.clear();
	passthrough_ = rewind_mode_ = false;
	return true;
}

bool DelayController::prebuffer_ready_locked() const
{
	if (!armed_ || state_ == DelayState::Error || buffered_.empty() || !video_watermark_ || !audio_watermark_)
		return false;
	const auto watermark = std::min(*video_watermark_, *audio_watermark_);
	return buffered_.front().kind == PacketKind::Video && buffered_.front().keyframe &&
		watermark - buffered_.front().dts_us >= target_delay_.count() &&
		std::any_of(buffered_.begin(), buffered_.end(), [&](const auto &p) {
			return p.kind == PacketKind::Audio && p.dts_us >= buffered_.front().dts_us && p.dts_us <= watermark;
		});
}

bool DelayController::prebuffer_ready() const
{
	std::scoped_lock lock(mutex_);
	return prebuffer_ready_locked();
}

bool DelayController::begin_broadcast()
{
	std::scoped_lock lock(mutex_);
	if (!prebuffer_ready_locked()) return false;
	// Keep the selected GOP's actual age: do not burst a keyframe interval of
	// backlog into the sender just to round the requested delay down.
	playback_delay_ = Microseconds{buffered_.back().dts_us - buffered_.front().dts_us};
	armed_ = false; state_ = DelayState::Delayed;
	return true;
}

bool DelayController::start_live()
{
	std::scoped_lock lock(mutex_);
	if (!armed_ || passthrough_ || state_ == DelayState::Error) return false;
	ready_.clear();
	passthrough_ = rewind_mode_ = true; waiting_for_live_keyframe_ = true;
	state_ = DelayState::Live;
	return true;
}

bool DelayController::rewind()
{
	std::scoped_lock lock(mutex_);
	if (!passthrough_ || !prebuffer_ready_locked()) return false;
	// Live media not yet taken is newer than the history about to replay.
	ready_.clear();
	passthrough_ = false;
	playback_delay_ = Microseconds{buffered_.back().dts_us - buffered_.front().dts_us};
	armed_ = false; state_ = DelayState::Delayed;
	return true;
}

bool DelayController::resume_live()
{
	std::scoped_lock lock(mutex_);
	if (!rewind_mode_ || passthrough_ || target_delay_ <= kNoDelay || state_ == DelayState::Error) return false;
	// The unreleased delayed media is the most recent programme history, so it
	// stays as the next rewind's buffer instead of being discarded.
	ready_.clear(); playback_delay_.reset(); pending_trim_ = false;
	armed_ = passthrough_ = true; waiting_for_live_keyframe_ = true;
	state_ = DelayState::Live;
	roll_prebuffer_locked();
	return true;
}

bool DelayController::rewind_mode() const
{
	std::scoped_lock lock(mutex_);
	return rewind_mode_;
}

void DelayController::roll_prebuffer_locked()
{
	if (!video_watermark_ || !audio_watermark_ || buffered_.empty()) return;
	const auto cutoff = std::min(*video_watermark_, *audio_watermark_) - target_delay_.count();
	std::size_t keep = 0;
	for (std::size_t i = 1; i < buffered_.size(); ++i) {
		const auto &p = buffered_[i];
		if (p.dts_us > cutoff) break;
		if (p.kind == PacketKind::Video && p.keyframe) keep = i;
	}
	while (keep--) { buffered_bytes_ -= buffered_.front().payload.size(); buffered_.pop_front(); }
	if (!passthrough_) state_ = prebuffer_ready_locked() ? DelayState::Delayed : DelayState::BuildingDelay;
}

bool DelayController::set_target(Microseconds target, std::string *error)
{
	std::scoped_lock lock(mutex_);
	if (armed_) { if (error) *error = "PREBUFFER_ARMED: disarm before changing delay"; return false; }
	playback_delay_.reset();
	if (target < kNoDelay || target > limits_.max_delay) {
		const auto message = diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Requested delay is outside the configured maximum");
		if (error)
			*error = message;
		return false;
	}

	if (target != target_delay_)
		ready_.clear();
	error_.clear();
	target_delay_ = target;
	pending_trim_ = false;
	if (target == kNoDelay) {
		state_ = DelayState::ReturningLive;
		buffered_.clear();
		ready_.clear();
		buffered_bytes_ = 0;
		reset_timestamp_tracking_locked();
		waiting_for_live_keyframe_ = true;
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
	if (!trim_to_target_locked()) {
		pending_trim_ = true;
		state_ = DelayState::BuildingDelay;
	}
	return state_ != DelayState::Error;
}

void DelayController::return_live()
{
	std::scoped_lock lock(mutex_);
	armed_ = passthrough_ = false; playback_delay_.reset(); video_watermark_.reset(); audio_watermark_.reset();
	state_ = DelayState::ReturningLive;
	buffered_.clear();
	ready_.clear();
	buffered_bytes_ = 0;
	target_delay_ = kNoDelay;
	error_.clear();
	reset_timestamp_tracking_locked();
	waiting_for_live_keyframe_ = true;
	state_ = DelayState::Live;
}

void DelayController::reset_for_discontinuity(std::string_view reason)
{
	std::scoped_lock lock(mutex_);
	armed_ = passthrough_ = rewind_mode_ = false; playback_delay_.reset(); video_watermark_.reset(); audio_watermark_.reset();
	buffered_.clear();
	ready_.clear();
	buffered_bytes_ = 0;
	target_delay_ = kNoDelay;
	state_ = DelayState::Live;
	error_ = std::string(reason);
	reset_timestamp_tracking_locked();
	waiting_for_live_keyframe_ = true;
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
		bool emit = true;
		if (waiting_for_live_keyframe_) {
			if (packet.kind != PacketKind::Video || !packet.keyframe)
				emit = false;
			else {
				waiting_for_live_keyframe_ = false;
				live_keyframe_dts_us_ = packet.dts_us;
			}
		}
		if (packet.kind == PacketKind::Audio && packet.dts_us < live_keyframe_dts_us_)
			emit = false;
		if (!passthrough_) {
			if (emit) ready_.push_back(std::move(packet));
			return;
		}
		// Rewind mode also keeps every packet as history, emitted or not.
		if (emit) ready_.push_back(packet);
	}
	// A delay can be requested between keyframes. Retain no dependent prefix:
	// independent holding remains on air until a safe programme GOP exists.
	if ((state_ == DelayState::BuildingDelay || passthrough_) && buffered_.empty() &&
		(packet.kind != PacketKind::Video || !packet.keyframe))
		return;
	if (!buffered_.empty() && packet.dts_us < buffered_.back().dts_us) {
		set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Encoder packet timestamps moved backwards"));
		return;
	}

	buffered_bytes_ += packet.payload.size();
	if (packet.kind == PacketKind::Video) video_watermark_ = packet.dts_us;
	else audio_watermark_ = packet.dts_us;
	buffered_.push_back(std::move(packet));
	if (armed_) roll_prebuffer_locked();
	if (buffered_bytes_ > limits_.max_bytes) {
		set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
			"Encoded packet buffer reached its memory limit"));
		return;
	}
	if (armed_) {
		if (buffered_.size() > 262144 || duration_locked() > limits_.max_delay + std::chrono::seconds(5))
			set_error_locked("PREBUFFER_CAPACITY: compressed history exceeded its time or packet budget");
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
	pending_trim_ = false;
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
	if (pending_trim_) {
		if (!trim_to_target_locked())
			return;
		pending_trim_ = false;
	}
	if (state_ == DelayState::BuildingDelay && duration_locked() >= target_delay_) {
		if (!discard_to_next_keyframe_locked()) {
			set_error_locked(diagnostic_error(DiagnosticCode::OutputControllerFailed,
				"No video keyframe is available to begin delayed playback"));
			return;
		}
		state_ = DelayState::Delayed;
	}

	if (state_ == DelayState::Delayed) {
		while (duration_locked() > playback_delay_.value_or(target_delay_) && !buffered_.empty()) {
			const auto payload_size = buffered_.front().payload.size();
			ready_.push_back(std::move(buffered_.front()));
			buffered_bytes_ -= payload_size;
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

	const auto discard_count = static_cast<std::size_t>(keyframe - buffered_.begin());
	for (std::size_t index = 0; index < discard_count; ++index) {
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
	if (armed_) {
		buffered_.clear(); ready_.clear(); buffered_bytes_ = 0;
		video_watermark_.reset(); audio_watermark_.reset();
	}
	state_ = DelayState::Error;
	error_ = std::move(message);
}

} // namespace active_delay
