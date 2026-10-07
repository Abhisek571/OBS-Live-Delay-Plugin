#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include "holding-silence.hpp"
#include "holding-feed.hpp"

void require(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	try {

		std::array<float, 1024> left, right;
		left.fill(0.75f); right.fill(-0.25f);
		float *planes[] = {left.data(), right.data()};
		require(active_delay::fill_holding_silence(planes, 2, 1024), "silence fill rejected valid planes");
		require(std::all_of(left.begin(), left.end(), [](float v) { return v == 0; }) &&
			std::all_of(right.begin(), right.end(), [](float v) { return v == 0; }), "nonzero programme sentinel survived");

		using namespace active_delay;
		HoldingFeed feed;
		const auto start = HoldingFeed::Clock::now();
		feed.begin(start);
		feed.set_headers({{1, 2}, {0x12, 0x10}});
		feed.ingest({PacketKind::Audio, {1}, 0, 0, false});
		feed.ingest({PacketKind::Video, {2}, 900, 900, false});
		require(feed.status().state == HoldingState::Starting, "dependent frame made holding ready");
		feed.ingest({PacketKind::Video, {3}, 1100, 1000, true});
		require(feed.status().state == HoldingState::Starting, "keyframe without paired audio made holding ready");
		feed.ingest({PacketKind::Audio, {4}, 1000, 1000, false});
		require(feed.status().state == HoldingState::Ready, "headers/keyframe/silent AAC did not make holding ready");
		auto packets = feed.take_packets();
		require(packets.size() == 2 && packets[0].keyframe && packets[0].pts_us == 1100 &&
			packets[1].kind == PacketKind::Audio, "unsafe prefix or composition offset changed");
		feed.ingest({PacketKind::Video, std::vector<std::uint8_t>(HoldingFeed::queue_byte_limit), 2000, 2000, true});
		require(feed.status().state == HoldingState::Ready, "exact byte bound should be allowed");
		feed.ingest({PacketKind::Audio, {1}, 2000, 2000, false});
		require(feed.status().state == HoldingState::Failed && feed.take_packets().empty() &&
			feed.status().queued_bytes == 0, "compressed byte exhaustion did not fail closed");
		feed.begin(start);
		feed.set_headers({{1}, {2}});
		feed.ingest({PacketKind::Video, {1}, 0, 0, true});
		feed.ingest({PacketKind::Audio, {2}, 2'000'000, 2'000'000, false});
		require(feed.status().state == HoldingState::Ready, "exact duration bound should be allowed");
		feed.ingest({PacketKind::Video, {3}, 2'000'001, 2'000'001, true});
		require(feed.status().state == HoldingState::Failed, "compressed duration exhaustion did not fail closed");
		feed.begin(start);
		feed.check_deadline(start + std::chrono::seconds(5));
		require(feed.status().state == HoldingState::Failed && feed.headers().avc_decoder_configuration.empty(),
			"missing encoder data did not fail closed at five seconds");
		feed.stop(); feed.stop();
		feed.ingest({PacketKind::Video, {1}, 0, 0, true});
		require(feed.status().state == HoldingState::Stopped, "late callback resurrected stopped feed");
		feed.begin(start);
		feed.set_headers({{1}, {2}});
		feed.ingest({PacketKind::Video, {1}, 1000, 1000, true});
		feed.ingest({PacketKind::Audio, {2}, 1000, 1000, false});
		feed.take_packets();
		feed.ingest({PacketKind::Video, {3}, 999, 999, false});
		require(feed.status().state == HoldingState::Failed, "backwards DTS was accepted after draining");
		feed.begin(start - std::chrono::seconds(5));
		feed.set_headers({{1}, {2}});
		feed.ingest({PacketKind::Video, {1}, 0, 0, true});
		feed.ingest({PacketKind::Audio, {2}, 0, 0, false});
		require(feed.status().state == HoldingState::Failed, "late packets beat readiness deadline before watchdog tick");
		feed.begin(); feed.set_headers({{1}, {2}});
		feed.ingest({PacketKind::Video, {1}, 0, 0, true});
		feed.ingest({PacketKind::Audio, {2}, 0, 0, false});
		feed.take_packets();
		feed.ingest({PacketKind::Video, {3}, 2100, 2000, false});
		feed.ingest({PacketKind::Audio, {4}, 1000, 1000, false});
		auto asynchronous = feed.take_packets();
		require(asynchronous.size() == 1 && asynchronous[0].dts_us == 1000,
			"independent encoder watermark published video ahead of pending AAC");
		feed.ingest({PacketKind::Audio, {5}, 2000, 2000, false});
		auto paired = feed.take_packets();
		require(paired.size() == 2 && paired[0].kind == PacketKind::Video && paired[0].pts_us == 2100 &&
			paired[1].kind == PacketKind::Audio && paired[1].dts_us == 2000,
			"independent encoder callback order/composition offsets were not preserved");

        for(bool ready:{false,true}) {
            feed.begin();feed.set_headers({{1,2},{0x11,0x90}});
            if(ready){feed.ingest({PacketKind::Video,{1},0,0,true});feed.ingest({PacketKind::Audio,{2},0,0,false});}
            feed.set_headers({{1,2},{0x11,0x90}});
            require(feed.status().state==(ready ? HoldingState::Ready : HoldingState::Starting),"identical holding snapshots harmless");
            feed.set_headers({{1,2},{0x12,0x10}});
            require(feed.status().state==HoldingState::Failed && feed.take_packets().empty(),"changed holding snapshot must fail closed before further admission");
        }
		std::cout << "Holding feed tests passed\n";
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n'; return 1;
	}
}