#include "holding-feed.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include "holding-capture.hpp"
using namespace active_delay;
struct Backend final : HoldingCaptureBackend {
	bool prepare_ok = true, start_ok = true;
	bool produce_media = false;
	HoldingFeed *feed = nullptr;
	std::atomic_bool cancel = false;
	std::thread producer;
	int owned = 0, stops = 0;
	bool prepare(HoldingFeed &target, std::string &error) override
	{
		feed = &target;
		owned = 3; // Resources acquired before a possible partial failure.
		if (!prepare_ok) error = "HOLDING_TEST_PREPARE";
		return prepare_ok;
	}
	bool start(std::string &error) override
	{
		if (!start_ok) error = "HOLDING_TEST_START";
		if (start_ok && produce_media) {
			cancel = false;
			producer = std::thread([this] {
				feed->set_headers({{1}, {2}});
				for (int64_t ts = 0; !cancel.load(); ts += 1000) {
					feed->ingest({PacketKind::Video, {1}, ts + 100, ts, ts % 1'000'000 == 0});
					feed->ingest({PacketKind::Audio, {2}, ts, ts, false});
					std::this_thread::sleep_for(std::chrono::microseconds(100));
				}
			});
		}
		return start_ok;
	}
	void stop() noexcept override
	{
		cancel = true;
		if (producer.joinable()) producer.join();
		feed = nullptr; owned = 0; ++stops;
	}
};
void check(bool v, const char *s) { if (!v) throw std::runtime_error(s); }
int main()
{
	try {

		auto backend = std::make_unique<Backend>();
		auto *probe = backend.get();
		HoldingCapture capture(std::move(backend));
		std::string error;
		probe->prepare_ok = false;
		check(!capture.prepare(error) && probe->owned == 0, "partial prepare leaked resources");
		check(capture.status().state == HoldingState::Failed, "prepare failure not latched");
		probe->prepare_ok = true; probe->start_ok = false;
		check(capture.prepare(error) && !capture.start(error) && probe->owned == 0, "partial start leaked resources");
		probe->start_ok = true;
		check(capture.prepare(error) && capture.start(error), "retry failed");
		const auto begin = HoldingFeed::Clock::now();
		while (capture.status().state != HoldingState::Failed && HoldingFeed::Clock::now() - begin < std::chrono::seconds(6))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		check(capture.status().state == HoldingState::Failed, "no-packet readiness watchdog did not fire");
		check(probe->owned == 3, "watchdog destroyed backend resources instead of deferring to owner");
		check(capture.take_packets().empty(), "failed capture published packets");
		const auto failed_stop_begin = HoldingFeed::Clock::now();
		capture.stop(); capture.stop();
		check(HoldingFeed::Clock::now() - failed_stop_begin < std::chrono::seconds(5), "faulted capture stop exceeded five seconds");
		check(probe->owned == 0 && capture.status().state == HoldingState::Stopped, "stop not quiescent");
		probe->produce_media = true;
		for (int i = 0; i < 30; ++i) {
			check(capture.prepare(error) && capture.start(error), "repeated prepare/start failed");
			const auto ready_deadline = HoldingFeed::Clock::now() + std::chrono::seconds(1);
			while (capture.status().state == HoldingState::Starting && HoldingFeed::Clock::now() < ready_deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			check(capture.status().state == HoldingState::Ready && !capture.take_packets().empty(), "concurrent callback capture not ready");
			const auto stop_begin = HoldingFeed::Clock::now();
			capture.stop();
			check(HoldingFeed::Clock::now() - stop_begin < std::chrono::seconds(5), "callback capture stop exceeded five seconds");
			check(probe->owned == 0 && !probe->producer.joinable() && capture.take_packets().empty(), "repeated lifecycle leaked resources/callbacks");
		}

		std::cout << "Holding lifecycle tests passed\n";
	} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}