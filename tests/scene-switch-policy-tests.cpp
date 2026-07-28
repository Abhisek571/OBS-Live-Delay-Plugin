#include "scene-switch-policy.hpp"

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

void inactive_switch_needs_no_scene_action()
{
	require(evaluate_scene_switch_health(false, false, false, false, false) == SceneSwitchHealth::Inactive,
		"an inactive scene switch must ignore stale scene state");
}

void building_delay_requires_the_holding_scene()
{
	require(evaluate_scene_switch_health(true, true, true, true, true) == SceneSwitchHealth::Healthy,
		"a valid holding scene should keep delay building");
	require(evaluate_scene_switch_health(true, true, true, false, false) == SceneSwitchHealth::HoldingUnavailable,
		"removing the holding scene must abort the scene transition");
	require(evaluate_scene_switch_health(true, true, true, true, false) == SceneSwitchHealth::HoldingInterrupted,
		"changing program scene during delay build must be detected");
}

void original_program_must_remain_restorable()
{
	require(evaluate_scene_switch_health(true, true, false, true, true) == SceneSwitchHealth::ProgramUnavailable,
		"removing the saved program scene must abort delay building");
	require(evaluate_scene_switch_health(true, false, true, true, false) == SceneSwitchHealth::Healthy,
		"the program scene may replace holding after delay becomes active");
}
} // namespace

int main()
{
	try {
		inactive_switch_needs_no_scene_action();
		building_delay_requires_the_holding_scene();
		original_program_must_remain_restorable();
		std::cout << "scene-switch policy tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "scene-switch policy test failure: " << error.what() << '\n';
		return 1;
	}
}
