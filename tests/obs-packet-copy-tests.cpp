#include "obs-packet-copy.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace active_delay;

namespace {
void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

encoder_packet packet(obs_encoder_type type, std::vector<std::uint8_t> &payload)
{
	encoder_packet result{};
	result.type = type;
	result.data = payload.data();
	result.size = payload.size();
	result.pts = 90'900;
	result.dts = 90'000;
	result.dts_usec = 1'000'000;
	result.timebase_num = 1;
	result.timebase_den = 90'000;
	return result;
}

void converts_annex_b_h264_to_length_prefixed_avc()
{
	std::vector<std::uint8_t> annex_b = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
	auto source = packet(OBS_ENCODER_VIDEO, annex_b);
	const auto copy = copy_encoder_packet(source);
	const std::vector<std::uint8_t> expected = {0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84};
	require(copy.kind == PacketKind::Video, "video packets should remain video");
	require(copy.payload == expected, "Annex-B start codes should become AVC NAL-unit lengths");
	require(copy.keyframe, "an IDR NAL unit should be identified as a keyframe");
	require(copy.dts_us == 1'000'000 && copy.pts_us == 1'010'000, "video timing should be preserved");
}

void copies_aac_without_video_conversion()
{
	std::vector<std::uint8_t> aac = {0x21, 0x10, 0x56};
	auto source = packet(OBS_ENCODER_AUDIO, aac);
	const auto copy = copy_encoder_packet(source);
	require(copy.kind == PacketKind::Audio, "audio packets should remain audio");
	require(copy.payload == aac, "AAC payloads should be copied unchanged");
}
} // namespace

int main()
{
	try {
		converts_annex_b_h264_to_length_prefixed_avc();
		copies_aac_without_video_conversion();
		std::cout << "OBS packet copy tests passed\n";
	} catch (const std::exception &error) {
		std::cerr << "OBS packet copy test failure: " << error.what() << '\n';
		return 1;
	}
}
