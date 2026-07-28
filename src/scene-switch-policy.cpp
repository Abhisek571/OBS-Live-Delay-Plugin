#include "scene-switch-policy.hpp"

namespace active_delay {

SceneSwitchHealth evaluate_scene_switch_health(bool transition_active, bool delay_building,
	bool program_available, bool holding_available, bool holding_is_current) noexcept
{
	if (!transition_active)
		return SceneSwitchHealth::Inactive;
	if (!program_available)
		return SceneSwitchHealth::ProgramUnavailable;
	if (!holding_available)
		return SceneSwitchHealth::HoldingUnavailable;
	if (delay_building && !holding_is_current)
		return SceneSwitchHealth::HoldingInterrupted;
	return SceneSwitchHealth::Healthy;
}

} // namespace active_delay
