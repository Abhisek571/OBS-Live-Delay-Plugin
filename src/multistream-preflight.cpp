#include "multistream-preflight.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace active_delay {
namespace {
void add_issue(MultistreamPreflight &result, DestinationPlatform platform, PreflightSeverity severity,
	PreflightIssueKind kind, std::string message)
{
	result.issues.push_back({platform, severity, kind, std::move(message)});
}

void check_shared_codecs(MultistreamPreflight &result, DestinationPlatform platform, const StreamRendition &rendition)
{
	if (rendition.video_codec != "h264")
		add_issue(result, platform, PreflightSeverity::HardIncompatibility, PreflightIssueKind::H264Required,
			"requires H.264 video");
	if (rendition.audio_codec != "aac")
		add_issue(result, platform, PreflightSeverity::HardIncompatibility, PreflightIssueKind::AacRequired,
			"requires AAC audio");
	if (rendition.rate_control != "CBR")
		add_issue(result, platform, PreflightSeverity::HardIncompatibility, PreflightIssueKind::CbrRequired,
			"requires CBR rate control");
}

void check_kick(MultistreamPreflight &result, const StreamRendition &rendition)
{
	check_shared_codecs(result, DestinationPlatform::Kick, rendition);
	if (rendition.width > 1920 || rendition.height > 1080)
		add_issue(result, DestinationPlatform::Kick, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::Maximum1080p,
			"supports at most 1920x1080");
	if (rendition.frames_per_second > 60.0)
		add_issue(result, DestinationPlatform::Kick, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::Maximum60Fps,
			"supports at most 60 fps");
	if (rendition.video_bitrate_kbps > 8'000)
		add_issue(result, DestinationPlatform::Kick, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::Maximum8000Kbps,
			"supports at most 8000 kbps video bitrate");
	if (rendition.keyframe_interval_seconds != 2)
		add_issue(result, DestinationPlatform::Kick, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::TwoSecondKeyframesRequired,
			"requires a 2-second keyframe interval");
}

void check_youtube(MultistreamPreflight &result, const StreamRendition &rendition)
{
	check_shared_codecs(result, DestinationPlatform::YouTube, rendition);
	if (rendition.frames_per_second > 60.0)
		add_issue(result, DestinationPlatform::YouTube, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::Maximum60Fps,
			"supports at most 60 fps for this RTMP/RTMPS workflow");
	if (rendition.keyframe_interval_seconds > 4)
		add_issue(result, DestinationPlatform::YouTube, PreflightSeverity::HardIncompatibility,
			PreflightIssueKind::MaximumFourSecondKeyframes,
			"requires a keyframe interval of 4 seconds or less");
	else if (rendition.keyframe_interval_seconds != 2)
		add_issue(result, DestinationPlatform::YouTube, PreflightSeverity::Guidance,
			PreflightIssueKind::TwoSecondKeyframesRecommended,
			"recommends a 2-second keyframe interval");
}

void check_twitch(MultistreamPreflight &result, const StreamRendition &rendition)
{
	check_shared_codecs(result, DestinationPlatform::Twitch, rendition);
	if (rendition.keyframe_interval_seconds != 2)
		add_issue(result, DestinationPlatform::Twitch, PreflightSeverity::Guidance,
			PreflightIssueKind::TwitchKeyframeGuidance,
			"commonly expects a 2-second keyframe interval; verify current Twitch guidance");
}
} // namespace

bool MultistreamPreflight::can_start() const noexcept
{
	return std::none_of(issues.begin(), issues.end(), [](const PreflightIssue &issue) {
		return issue.severity == PreflightSeverity::HardIncompatibility;
	});
}

MultistreamPreflight evaluate_multistream_preflight(const MultistreamConfiguration &configuration,
	const StreamRendition &rendition)
{
	MultistreamPreflight result;
	for (const auto &destination : configuration.secondary_destinations) {
		if (!destination.enabled)
			continue;
		++result.enabled_destination_count;
		switch (destination.platform) {
		case DestinationPlatform::CustomRtmp: break;
		case DestinationPlatform::Twitch: check_twitch(result, rendition); break;
		case DestinationPlatform::YouTube: check_youtube(result, rendition); break;
		case DestinationPlatform::Kick: check_kick(result, rendition); break;
		}
	}
	result.estimated_upload_kbps = static_cast<std::uint64_t>(rendition.video_bitrate_kbps +
		rendition.audio_bitrate_kbps) * result.enabled_destination_count;
	return result;
}

} // namespace active_delay
