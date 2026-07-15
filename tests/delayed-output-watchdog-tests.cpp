#include "delayed-output-watchdog.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}
} // namespace

int main()
{
	try {
		require(evaluate_delayed_output_health(true, 0, 1s) == DelayedOutputHealth::WaitingForVideo,
			"an active output should receive a startup grace period");
		require(evaluate_delayed_output_health(true, 1, 1s) == DelayedOutputHealth::Progressing,
			"the first encoded video frame should satisfy the watchdog");
		require(evaluate_delayed_output_health(false, 0, 1s) == DelayedOutputHealth::Stopped,
			"an asynchronously stopped output should trigger recovery");
		require(evaluate_delayed_output_health(true, 0, 5s) == DelayedOutputHealth::VideoTimeout,
			"an active output with no video frames must time out");
		std::cout << "delayed-output-watchdog tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "delayed-output-watchdog test failure: " << error.what() << '\n';
		return 1;
	}
}
