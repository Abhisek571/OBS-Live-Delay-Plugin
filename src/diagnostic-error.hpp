#pragma once

#include <string>
#include <string_view>

namespace active_delay {

// Stable identifiers for failures that can be observed in the dock or OBS log.
// Keep an existing value's meaning unchanged so manual acceptance reports remain
// comparable across builds. Detailed errors must never include stream credentials.
enum class DiagnosticCode {
	DirectProfileUnavailable,
	DirectOutputModeUnsupported,
	DirectServiceMissing,
	DirectVideoEncoderUnavailable,
	DirectAudioEncoderUnsupported,
	DirectAudioEncoderUnavailable,
	DirectEncoderSettingsUnavailable,
	DirectEncoderCreationFailed,
	DirectOutputCreationFailed,
	DirectServiceAttachFailed,
	DirectOutputStartFailed,
	OutputCaptureNotReady,
	OutputEncoderInitializationFailed,
	OutputServiceMissing,
	OutputCodecHeadersUnavailable,
	OutputCaptureStartFailed,
	OutputEncoderStopped,
	OutputControllerFailed,
	OutputMuxFailed,
	OutputPipelineException,
	OperatingModeConflict,
	NetworkConsumerStartupFailed,
	PacketConsumerFailed,
	PacketDiscontinuityNotificationFailed,
	MultistreamConfigurationInvalid,
	MultiTargetStartupFailed,
	MultistreamPreflightFailed,
	RtmpTargetInvalid,
	RtmpConnectionFailed,
	RtmpWriteFailed,
	RtmpSenderStartupFailed,
	RtmpSenderQueueFull,
	RtmpReconnectExhausted,
	SecondaryTargetFailed,
	OutputStoppedUnexpectedly,
	OutputNoFrames,
	SceneProbeInputInvalid,
	SceneProbeRecursionDetected,
	HoldingSceneInvalid,
	HoldingSceneInterrupted,
	ProgramSceneUnavailable,
	DockRegistrationFailed,
	OutputControlConflict,
	DelayControlUnavailable,
	SettingsSaveFailed,
	HotkeyRegistrationFailed,
};

constexpr std::string_view diagnostic_code(DiagnosticCode code) noexcept
{
	switch (code) {
	case DiagnosticCode::DirectProfileUnavailable: return "ALD-E1001";
	case DiagnosticCode::DirectOutputModeUnsupported: return "ALD-E1002";
	case DiagnosticCode::DirectServiceMissing: return "ALD-E1003";
	case DiagnosticCode::DirectVideoEncoderUnavailable: return "ALD-E1004";
	case DiagnosticCode::DirectAudioEncoderUnsupported: return "ALD-E1005";
	case DiagnosticCode::DirectAudioEncoderUnavailable: return "ALD-E1006";
	case DiagnosticCode::DirectEncoderSettingsUnavailable: return "ALD-E1007";
	case DiagnosticCode::DirectEncoderCreationFailed: return "ALD-E1008";
	case DiagnosticCode::DirectOutputCreationFailed: return "ALD-E1009";
	case DiagnosticCode::DirectServiceAttachFailed: return "ALD-E1010";
	case DiagnosticCode::DirectOutputStartFailed: return "ALD-E1011";
	case DiagnosticCode::OutputCaptureNotReady: return "ALD-E2001";
	case DiagnosticCode::OutputEncoderInitializationFailed: return "ALD-E2002";
	case DiagnosticCode::OutputServiceMissing: return "ALD-E2003";
	case DiagnosticCode::OutputCodecHeadersUnavailable: return "ALD-E2004";
	case DiagnosticCode::OutputCaptureStartFailed: return "ALD-E2005";
	case DiagnosticCode::OutputEncoderStopped: return "ALD-E2006";
	case DiagnosticCode::OutputControllerFailed: return "ALD-E2007";
	case DiagnosticCode::OutputMuxFailed: return "ALD-E2008";
	case DiagnosticCode::OutputPipelineException: return "ALD-E2009";
	case DiagnosticCode::OperatingModeConflict: return "ALD-E2010";
	case DiagnosticCode::NetworkConsumerStartupFailed: return "ALD-E2011";
	case DiagnosticCode::PacketConsumerFailed: return "ALD-E2012";
	case DiagnosticCode::PacketDiscontinuityNotificationFailed: return "ALD-E2013";
	case DiagnosticCode::MultistreamConfigurationInvalid: return "ALD-E2014";
	case DiagnosticCode::MultiTargetStartupFailed: return "ALD-E2015";
	case DiagnosticCode::MultistreamPreflightFailed: return "ALD-E2016";
	case DiagnosticCode::RtmpTargetInvalid: return "ALD-E3001";
	case DiagnosticCode::RtmpConnectionFailed: return "ALD-E3002";
	case DiagnosticCode::RtmpWriteFailed: return "ALD-E3003";
	case DiagnosticCode::RtmpSenderStartupFailed: return "ALD-E3004";
	case DiagnosticCode::RtmpSenderQueueFull: return "ALD-E3005";
	case DiagnosticCode::RtmpReconnectExhausted: return "ALD-E3006";
	case DiagnosticCode::SecondaryTargetFailed: return "ALD-E3007";
	case DiagnosticCode::OutputStoppedUnexpectedly: return "ALD-E4001";
	case DiagnosticCode::OutputNoFrames: return "ALD-E4002";
	case DiagnosticCode::SceneProbeInputInvalid: return "ALD-E5001";
	case DiagnosticCode::SceneProbeRecursionDetected: return "ALD-E5002";
	case DiagnosticCode::HoldingSceneInvalid: return "ALD-E5003";
	case DiagnosticCode::HoldingSceneInterrupted: return "ALD-E5004";
	case DiagnosticCode::ProgramSceneUnavailable: return "ALD-E5005";
	case DiagnosticCode::DockRegistrationFailed: return "ALD-E6001";
	case DiagnosticCode::OutputControlConflict: return "ALD-E6002";
	case DiagnosticCode::DelayControlUnavailable: return "ALD-E6003";
	case DiagnosticCode::SettingsSaveFailed: return "ALD-E6004";
	case DiagnosticCode::HotkeyRegistrationFailed: return "ALD-E6005";
	}
	return "ALD-E0000";
}

inline std::string diagnostic_error(DiagnosticCode code, std::string_view detail)
{
	if (detail.starts_with("[ALD-E"))
		return std::string(detail);
	return "[" + std::string(diagnostic_code(code)) + "] " + std::string(detail);
}

} // namespace active_delay
