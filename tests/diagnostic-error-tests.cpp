#include "diagnostic-error.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace active_delay;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void diagnostic_codes_are_stable_and_visible()
{
	require(diagnostic_code(DiagnosticCode::DirectOutputModeUnsupported) == "ALD-E1002",
		"direct-mode code must remain stable");
	require(diagnostic_code(DiagnosticCode::OutputCodecHeadersUnavailable) == "ALD-E2004",
		"codec-header code must remain stable");
	require(diagnostic_code(DiagnosticCode::RtmpReconnectExhausted) == "ALD-E3006",
		"reconnect code must remain stable");
	require(diagnostic_code(DiagnosticCode::OutputNoFrames) == "ALD-E4002",
		"watchdog code must remain stable");
	require(diagnostic_code(DiagnosticCode::OperatingModeConflict) == "ALD-E2010",
		"mode conflict code must remain stable");
	require(diagnostic_code(DiagnosticCode::NetworkConsumerStartupFailed) == "ALD-E2011",
		"network consumer startup code must remain stable");
	require(diagnostic_code(DiagnosticCode::PacketConsumerFailed) == "ALD-E2012",
		"packet consumer failure code must remain stable");
	require(diagnostic_code(DiagnosticCode::PacketDiscontinuityNotificationFailed) == "ALD-E2013",
		"discontinuity notification code must remain stable");
	require(diagnostic_code(DiagnosticCode::MultistreamConfigurationInvalid) == "ALD-E2014",
		"multistream configuration code must remain stable");
	require(diagnostic_code(DiagnosticCode::MultiTargetStartupFailed) == "ALD-E2015",
		"multistream startup code must remain stable");
	require(diagnostic_code(DiagnosticCode::MultistreamPreflightFailed) == "ALD-E2016",
		"multistream preflight code must remain stable");
	require(diagnostic_code(DiagnosticCode::SecondaryTargetFailed) == "ALD-E3007",
		"secondary target code must remain stable");
	require(diagnostic_code(DiagnosticCode::SceneProbeInputInvalid) == "ALD-E5001",
		"scene-probe input code must remain stable");
	require(diagnostic_code(DiagnosticCode::SceneProbeRecursionDetected) == "ALD-E5002",
		"scene-probe recursion code must remain stable");
	require(diagnostic_code(DiagnosticCode::HoldingSceneInvalid) == "ALD-E5003",
		"holding-scene validation code must remain stable");
	require(diagnostic_code(DiagnosticCode::HoldingSceneInterrupted) == "ALD-E5004",
		"holding-scene interruption code must remain stable");
	require(diagnostic_code(DiagnosticCode::ProgramSceneUnavailable) == "ALD-E5005",
		"program-scene restoration code must remain stable");
	require(diagnostic_code(DiagnosticCode::DockRegistrationFailed) == "ALD-E6001",
		"dock registration code must remain stable");
	require(diagnostic_code(DiagnosticCode::OutputControlConflict) == "ALD-E6002",
		"output control conflict code must remain stable");
	require(diagnostic_code(DiagnosticCode::DelayControlUnavailable) == "ALD-E6003",
		"delay control availability code must remain stable");
	require(diagnostic_code(DiagnosticCode::SettingsSaveFailed) == "ALD-E6004",
		"settings save code must remain stable");
	require(diagnostic_code(DiagnosticCode::HotkeyRegistrationFailed) == "ALD-E6005",
		"hotkey registration code must remain stable");
}

void diagnostic_error_prefixes_once()
{
	const auto message = diagnostic_error(DiagnosticCode::RtmpConnectionFailed, "connection refused");
	require(message == "[ALD-E3002] connection refused", "diagnostic error must carry its code");
	require(diagnostic_error(DiagnosticCode::OutputPipelineException, message) == message,
		"an existing diagnostic code must not be hidden by a second prefix");
}
} // namespace

int main()
{
	try {
		diagnostic_codes_are_stable_and_visible();
		diagnostic_error_prefixes_once();
		std::cout << "diagnostic error tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "diagnostic error test failure: " << error.what() << '\n';
		return 1;
	}
}
