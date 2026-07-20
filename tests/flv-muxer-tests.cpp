#include "delay-controller.hpp"
#include "flv-muxer.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

EncodedPacket packet(PacketKind kind, std::vector<std::uint8_t> payload, std::int64_t pts, std::int64_t dts,
	bool keyframe = false)
{
	return {kind, std::move(payload), pts, dts, keyframe};
}

FlvMuxer make_muxer()
{
	return FlvMuxer({{0x01, 0x64, 0x00, 0x1f}, {0x12, 0x10}});
}

std::vector<std::uint8_t> avc_nal(std::initializer_list<std::uint8_t> bytes)
{
	const auto size = static_cast<std::uint32_t>(bytes.size());
	std::vector<std::uint8_t> result = {
		static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
		static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
	result.insert(result.end(), bytes);
	return result;
}

void writes_flv_file_header_and_tag_fields()
{
	const std::vector<std::uint8_t> expected_header = {
		'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
	require(make_flv_header() == expected_header, "FLV header should declare audio and video");

	FlvTag tag{FlvTagType::Audio, 0x0102'0304, {0xaf, 0x01, 0xaa}, false};
	const std::vector<std::uint8_t> expected = {
		0x08, 0x00, 0x00, 0x03, 0x02, 0x03, 0x04, 0x01, 0x00, 0x00, 0x00,
		0xaf, 0x01, 0xaa, 0x00, 0x00, 0x00, 0x0e};
	require(serialize_flv_tag(tag) == expected, "serialized tag fields should use FLV byte order");
}

void writes_codec_sequence_headers()
{
	auto headers = make_muxer().sequence_headers();
	require(headers.size() == 2, "AAC and AVC sequence headers should be emitted");
	require(headers[0].type == FlvTagType::Audio && headers[0].payload == std::vector<std::uint8_t>({0xaf, 0x00, 0x12, 0x10}),
		"AAC sequence header should contain AudioSpecificConfig");
	require(headers[1].type == FlvTagType::Video && headers[1].keyframe,
		"AVC sequence header should be a keyframe video tag");
	require(headers[1].payload == std::vector<std::uint8_t>({0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00, 0x1f}),
		"AVC sequence header should contain AVCDecoderConfigurationRecord");
}

void muxes_h264_aac_and_normalizes_timestamps()
{
	auto muxer = make_muxer();
	std::vector<EncodedPacket> packets;
	packets.emplace_back(packet(PacketKind::Video, avc_nal({0x65, 0x88}), 1'040'000, 1'000'000, true));
	packets.emplace_back(packet(PacketKind::Audio, {0x21, 0x22}, 1'020'000, 1'020'000));
	std::vector<FlvTag> tags;
	std::string error;
	require(muxer.mux(std::move(packets), tags, error), "valid packets should mux");
	require(tags.size() == 2, "both packets should produce tags");
	require(tags[0].timestamp_ms == 0 && tags[0].payload ==
		std::vector<std::uint8_t>({0x17, 0x01, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x65, 0x88}),
		"video tag should contain frame type, AVC packet type, and composition time");
	require(tags[1].timestamp_ms == 20 && tags[1].payload == std::vector<std::uint8_t>({0xaf, 0x01, 0x21, 0x22}),
		"audio tag should use the normalized DTS");
}

void accepts_negative_composition_time()
{
	auto muxer = make_muxer();
	std::vector<FlvTag> tags;
	std::string error;
	require(muxer.mux({packet(PacketKind::Video, avc_nal({0x41, 0x9a}), 960'000, 1'000'000, false)}, tags, error),
		"B-frame composition offsets may be negative");
	require(tags[0].payload[2] == 0xff && tags[0].payload[3] == 0xff && tags[0].payload[4] == 0xd8,
		"negative composition time should use signed 24-bit two's complement");
}

void rejects_timestamp_regression_without_advancing_state()
{
	auto muxer = make_muxer();
	std::vector<FlvTag> tags;
	std::string error;
	require(muxer.mux({packet(PacketKind::Video, avc_nal({0x65, 0x88}), 2'000'000, 2'000'000, true)}, tags, error),
		"first packet should mux");
	require(!muxer.mux({packet(PacketKind::Audio, {0x02}, 1'999'000, 1'999'000)}, tags, error),
		"backwards DTS should be rejected");
	require(error.find("backwards") != std::string::npos, "timestamp failure should be explicit");
	require(error.starts_with("[ALD-E2008]"), "mux failures must carry a stable diagnostic code");
	require(muxer.mux({packet(PacketKind::Audio, {0x03}, 2'010'000, 2'010'000)}, tags, error),
		"a rejected batch must not corrupt muxer state");
	require(tags[0].timestamp_ms == 10, "timeline should remain based on the accepted packet");
}

void rejects_annex_b_video_before_sending_it_to_rtmp()
{
	auto muxer = make_muxer();
	std::vector<FlvTag> tags;
	std::string error;
	require(!muxer.mux({packet(PacketKind::Video, {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84},
		1'000'000, 1'000'000, true)}, tags, error), "Annex-B H.264 must not be written into an FLV tag");
	require(error.find("Annex-B") != std::string::npos, "the packet-format error should identify Annex-B input");
	require(error.starts_with("[ALD-E2008]"), "packet-format failures must carry a stable diagnostic code");
}

void rejects_invalid_avc_nal_lengths()
{
	auto muxer = make_muxer();
	std::vector<FlvTag> tags;
	std::string error;
	require(!muxer.mux({packet(PacketKind::Video, {0x00, 0x00, 0x00, 0x10, 0x65, 0x88},
		1'000'000, 1'000'000, true)}, tags, error), "an AVC NAL length beyond the packet must be rejected");
	require(error.find("NAL-unit length") != std::string::npos, "the invalid AVC length should be explicit");
}

void consumes_packets_released_by_delay_controller()
{
	DelayController controller;
	require(controller.set_target(1s), "delay should be accepted");
	controller.ingest(packet(PacketKind::Video, avc_nal({0x65, 0x01}), 0, 0, true));
	controller.ingest(packet(PacketKind::Audio, {0x02}, 500'000, 500'000));
	controller.ingest(packet(PacketKind::Video, avc_nal({0x65, 0x03}), 1'000'000, 1'000'000, true));
	controller.ingest(packet(PacketKind::Video, avc_nal({0x65, 0x04}), 2'000'000, 2'000'000, true));

	auto ready = controller.take_ready_packets();
	require(!ready.empty(), "controller should release delayed packets");
	auto muxer = make_muxer();
	std::vector<FlvTag> tags;
	std::string error;
	require(muxer.mux(std::move(ready), tags, error), "controller output should feed the FLV muxer directly");
	require(tags.front().type == FlvTagType::Video && tags.front().keyframe,
		"delayed FLV output should start at a video keyframe");
}
} // namespace

int main()
{
	try {
		writes_flv_file_header_and_tag_fields();
		writes_codec_sequence_headers();
		muxes_h264_aac_and_normalizes_timestamps();
		accepts_negative_composition_time();
		rejects_timestamp_regression_without_advancing_state();
		rejects_annex_b_video_before_sending_it_to_rtmp();
		rejects_invalid_avc_nal_lengths();
		consumes_packets_released_by_delay_controller();
		std::cout << "FLV muxer tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "FLV muxer test failure: " << error.what() << '\n';
		return 1;
	}
}
