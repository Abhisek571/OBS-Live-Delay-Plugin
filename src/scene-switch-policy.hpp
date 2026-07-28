#pragma once

namespace active_delay {

enum class SceneSwitchHealth {
	Inactive,
	Healthy,
	HoldingUnavailable,
	HoldingInterrupted,
	ProgramUnavailable,
};

[[nodiscard]] SceneSwitchHealth evaluate_scene_switch_health(bool transition_active, bool delay_building,
	bool program_available, bool holding_available, bool holding_is_current) noexcept;

} // namespace active_delay
