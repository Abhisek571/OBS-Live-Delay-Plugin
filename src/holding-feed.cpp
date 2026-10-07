#include "holding-feed.hpp"
#include <utility>
#include <algorithm>

namespace active_delay {
void HoldingFeed::set_failure_callback(std::function<void(const std::string &)> callback)
{
 std::scoped_lock lock(mutex_);failure_callback_=std::move(callback);
}
void HoldingFeed::begin(Clock::time_point now)
{
	std::scoped_lock lock(mutex_);
	status_ = {HoldingState::Starting};
	headers_ = {};
	packets_.clear();
	keyframe_ = audio_ = false;
	minimum_dts_.reset(); maximum_dts_.reset();
	deadline_ = now + readiness_limit;
	last_video_dts_.reset(); last_audio_dts_.reset();
}
void HoldingFeed::update_ready_locked()
{
	if (status_.state == HoldingState::Starting && keyframe_ && audio_ &&
		!headers_.avc_decoder_configuration.empty() && !headers_.aac_audio_specific_config.empty())
		status_.state = HoldingState::Ready;
}
void HoldingFeed::set_headers(FlvCodecHeaders headers)
{
	std::scoped_lock lock(mutex_);
	check_deadline_locked(Clock::now());
	if (status_.state != HoldingState::Starting && status_.state != HoldingState::Ready) return;
	if ((!headers_.avc_decoder_configuration.empty() || !headers_.aac_audio_specific_config.empty()) &&
		(headers_.avc_decoder_configuration!=headers.avc_decoder_configuration || headers_.aac_audio_specific_config!=headers.aac_audio_specific_config)) {
		fail_locked("HOLDING_CODEC_CHANGED: encoder configuration changed during capture");return;
	}
	headers_ = std::move(headers);
	update_ready_locked();
}
void HoldingFeed::ingest(EncodedPacket packet)
{
	std::scoped_lock lock(mutex_);
	check_deadline_locked(Clock::now());
	if (status_.state != HoldingState::Starting && status_.state != HoldingState::Ready) return;
	if (!keyframe_) {
		if (packet.kind != PacketKind::Video || !packet.keyframe) return;
		keyframe_ = true;
		status_.video_cutoff_us = packet.dts_us;
	}
	if (packet.kind == PacketKind::Audio) {
		if (packet.dts_us < status_.video_cutoff_us) return;
		audio_ = true;
	}
	auto &last = packet.kind == PacketKind::Video ? last_video_dts_ : last_audio_dts_;
	if (last && packet.dts_us < *last) {
		fail_locked("HOLDING_TIMESTAMP_INVALID: encoder DTS moved backwards");
		return;
	}
	last = packet.dts_us;
	if (packet.payload.size() > queue_byte_limit - status_.queued_bytes || packets_.size() >= 4096) {
		fail_locked("HOLDING_QUEUE_LIMIT: compressed holding queue exhausted");
		return;
	}
	const auto minimum = minimum_dts_ ? std::min(*minimum_dts_, packet.dts_us) : packet.dts_us;
	const auto maximum = maximum_dts_ ? std::max(*maximum_dts_, packet.dts_us) : packet.dts_us;
	if (static_cast<std::uint64_t>(maximum) - static_cast<std::uint64_t>(minimum) > queue_duration_us) {
		fail_locked("HOLDING_QUEUE_LIMIT: holding media exceeded two seconds");
		return;
	}
	minimum_dts_ = minimum; maximum_dts_ = maximum;
	status_.queued_bytes += packet.payload.size();
	packets_.push_back(std::move(packet));
	update_ready_locked();
}
void HoldingFeed::fail_locked(std::string error)
{
	if (status_.state == HoldingState::Stopped || status_.state == HoldingState::Failed) return;
	status_.state = HoldingState::Failed;
	status_.error = std::move(error);
	if(failure_callback_)failure_callback_(status_.error);
	packets_.clear();
	status_.queued_bytes = 0;
	headers_ = {};
}
void HoldingFeed::fail(std::string error)
{
	std::scoped_lock lock(mutex_);
	fail_locked(std::move(error));
}
void HoldingFeed::check_deadline(Clock::time_point now)
{
	std::scoped_lock lock(mutex_);
	check_deadline_locked(now);
}
void HoldingFeed::check_deadline_locked(Clock::time_point now)
{
	if (status_.state == HoldingState::Starting && now >= deadline_)
		fail_locked("HOLDING_READINESS_TIMEOUT: H.264 keyframe, AAC or codec headers unavailable");
}
void HoldingFeed::stop()
{
	std::scoped_lock lock(mutex_);
	status_ = {};
	headers_ = {};
	packets_.clear();
	keyframe_ = audio_ = false;
}
HoldingStatus HoldingFeed::status() const
{
	std::scoped_lock lock(mutex_);
	return status_;
}
FlvCodecHeaders HoldingFeed::headers() const
{
	std::scoped_lock lock(mutex_);
	return headers_;
}
std::vector<EncodedPacket> HoldingFeed::take_packets()
{
	std::scoped_lock lock(mutex_);
	if (status_.state != HoldingState::Ready) return {};
	std::vector<EncodedPacket> result;
	result.reserve(packets_.size());
	// Independent encoder callbacks arrive concurrently. Only release up to
	// the slower stream's DTS watermark, preventing regression across drains.
	const auto watermark = std::min(*last_video_dts_, *last_audio_dts_);
	std::stable_sort(packets_.begin(), packets_.end(), [](const auto &a, const auto &b) {
		if (a.dts_us != b.dts_us) return a.dts_us < b.dts_us;
		return a.kind == PacketKind::Video && b.kind == PacketKind::Audio;
	});
	while (!packets_.empty() && packets_.front().dts_us <= watermark) {
		status_.queued_bytes -= packets_.front().payload.size();
		result.push_back(std::move(packets_.front()));
		packets_.pop_front();
	}
	if (packets_.empty()) { minimum_dts_.reset(); maximum_dts_.reset(); }
	else { minimum_dts_ = packets_.front().dts_us; maximum_dts_ = packets_.back().dts_us; }
	return result;
}
} // namespace active_delay