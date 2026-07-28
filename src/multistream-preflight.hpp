#pragma once

#include "multistream-config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace active_delay {

struct StreamRendition {
	std::string video_codec;
	std::string audio_codec;
	std::string rate_control;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	double frames_per_second = 0.0;
	std::uint32_t video_bitrate_kbps = 0;
	std::uint32_t audio_bitrate_kbps = 0;
	std::uint32_t keyframe_interval_seconds = 0;
};

enum class PreflightSeverity { Guidance, HardIncompatibility };

enum class PreflightIssueKind {
	H264Required,
	AacRequired,
	CbrRequired,
	Maximum1080p,
	Maximum60Fps,
	Maximum8000Kbps,
	TwoSecondKeyframesRequired,
	MaximumFourSecondKeyframes,
	TwoSecondKeyframesRecommended,
	TwitchKeyframeGuidance,
};

struct PreflightIssue {
	DestinationPlatform platform = DestinationPlatform::CustomRtmp;
	PreflightSeverity severity = PreflightSeverity::Guidance;
	PreflightIssueKind kind = PreflightIssueKind::H264Required;
	std::string message;
};

struct MultistreamPreflight {
	std::size_t enabled_destination_count = 1;
	std::uint64_t estimated_upload_kbps = 0;
	std::vector<PreflightIssue> issues;

	[[nodiscard]] bool can_start() const noexcept;
};

[[nodiscard]] MultistreamPreflight evaluate_multistream_preflight(const MultistreamConfiguration &configuration,
	const StreamRendition &rendition);

} // namespace active_delay
