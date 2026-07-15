#include "flv-muxer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace active_delay {
namespace {
constexpr std::size_t kFlvTagHeaderSize = 11;
constexpr std::size_t kPreviousTagSize = 4;
constexpr std::size_t kMaxFlvPayloadSize = 0x00ff'ffff;
constexpr std::int64_t kMinCompositionTime = -0x0080'0000;
constexpr std::int64_t kMaxCompositionTime = 0x007f'ffff;

bool validate_avc_packet(const std::vector<std::uint8_t> &payload, std::string &error)
{
	if (payload.size() >= 3 && payload[0] == 0x00 && payload[1] == 0x00 &&
		(payload[2] == 0x01 || (payload.size() >= 4 && payload[2] == 0x00 && payload[3] == 0x01))) {
		error = "H.264 video packet is Annex-B instead of length-prefixed AVC";
		return false;
	}

	std::size_t offset = 0;
	while (offset < payload.size()) {
		if (payload.size() - offset < 4) {
			error = "H.264 AVC packet ends before its NAL-unit length field";
			return false;
		}
		const auto nal_size = (static_cast<std::uint32_t>(payload[offset]) << 24) |
			(static_cast<std::uint32_t>(payload[offset + 1]) << 16) |
			(static_cast<std::uint32_t>(payload[offset + 2]) << 8) |
			static_cast<std::uint32_t>(payload[offset + 3]);
		offset += 4;
		if (nal_size == 0 || nal_size > payload.size() - offset) {
			error = "H.264 AVC packet contains an invalid NAL-unit length";
			return false;
		}
		offset += nal_size;
	}
	return true;
}

void append_be24(std::vector<std::uint8_t> &output, std::uint32_t value)
{
	output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
	output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
	output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void append_be32(std::vector<std::uint8_t> &output, std::uint32_t value)
{
	output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
	output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
	output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
	output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

FlvTag make_sequence_header(FlvTagType type, const std::vector<std::uint8_t> &configuration)
{
	FlvTag tag;
	tag.type = type;
	tag.keyframe = type == FlvTagType::Video;
	if (type == FlvTagType::Video)
		tag.payload = {0x17, 0x00, 0x00, 0x00, 0x00};
	else
		tag.payload = {0xaf, 0x00};
	tag.payload.insert(tag.payload.end(), configuration.begin(), configuration.end());
	return tag;
}
} // namespace

std::size_t FlvTag::wire_size() const noexcept
{
	return kFlvTagHeaderSize + payload.size() + kPreviousTagSize;
}

FlvMuxer::FlvMuxer(FlvCodecHeaders headers) : headers_(std::move(headers))
{
	if (headers_.avc_decoder_configuration.empty())
		throw std::invalid_argument("H.264 AVC decoder configuration is empty");
	if (headers_.aac_audio_specific_config.empty())
		throw std::invalid_argument("AAC AudioSpecificConfig is empty");
}

std::vector<FlvTag> FlvMuxer::sequence_headers() const
{
	std::vector<FlvTag> result;
	result.reserve(2);
	result.emplace_back(make_sequence_header(FlvTagType::Audio, headers_.aac_audio_specific_config));
	result.emplace_back(make_sequence_header(FlvTagType::Video, headers_.avc_decoder_configuration));
	return result;
}

bool FlvMuxer::mux(std::vector<EncodedPacket> packets, std::vector<FlvTag> &tags, std::string &error)
{
	error.clear();
	if (packets.empty()) {
		tags.clear();
		return true;
	}

	auto has_base = has_base_timestamp_;
	auto base_dts = base_dts_us_;
	auto last_dts = last_dts_us_;
	std::vector<FlvTag> result;
	result.reserve(packets.size());

	for (auto &packet : packets) {
		if (packet.payload.empty()) {
			error = "An encoded packet has an empty payload";
			return false;
		}
		if (packet.kind == PacketKind::Video && !validate_avc_packet(packet.payload, error))
			return false;
		const auto framing_size = packet.kind == PacketKind::Video ? 5ULL : 2ULL;
		if (packet.payload.size() > kMaxFlvPayloadSize - framing_size) {
			error = "Encoded packet is too large for a single FLV tag";
			return false;
		}
		if (has_base && packet.dts_us < last_dts) {
			error = "FLV input DTS moved backwards";
			return false;
		}
		if (!has_base) {
			has_base = true;
			base_dts = packet.dts_us;
		}

		const auto relative_dts_us = packet.dts_us - base_dts;
		if (relative_dts_us < 0 || relative_dts_us / 1'000 > std::numeric_limits<std::uint32_t>::max()) {
			error = "FLV timestamp is outside the 32-bit millisecond range";
			return false;
		}

		FlvTag tag;
		tag.type = packet.kind == PacketKind::Video ? FlvTagType::Video : FlvTagType::Audio;
		tag.timestamp_ms = static_cast<std::uint32_t>(relative_dts_us / 1'000);
		tag.keyframe = packet.kind == PacketKind::Video && packet.keyframe;
		if (packet.kind == PacketKind::Video) {
			const auto composition_ms = (packet.pts_us - packet.dts_us) / 1'000;
			if (composition_ms < kMinCompositionTime || composition_ms > kMaxCompositionTime) {
				error = "H.264 composition time is outside the signed 24-bit FLV range";
				return false;
			}
			tag.payload.reserve(packet.payload.size() + 5);
			tag.payload.push_back(packet.keyframe ? 0x17 : 0x27);
			tag.payload.push_back(0x01);
			append_be24(tag.payload, static_cast<std::uint32_t>(composition_ms) & 0x00ff'ffff);
		} else {
			tag.payload.reserve(packet.payload.size() + 2);
			tag.payload.push_back(0xaf);
			tag.payload.push_back(0x01);
		}
		tag.payload.insert(tag.payload.end(), packet.payload.begin(), packet.payload.end());
		result.emplace_back(std::move(tag));
		last_dts = packet.dts_us;
	}

	has_base_timestamp_ = has_base;
	base_dts_us_ = base_dts;
	last_dts_us_ = last_dts;
	tags = std::move(result);
	return true;
}

void FlvMuxer::reset_timeline() noexcept
{
	has_base_timestamp_ = false;
	base_dts_us_ = 0;
	last_dts_us_ = 0;
}

std::vector<std::uint8_t> make_flv_header()
{
	return {'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> serialize_flv_tag(const FlvTag &tag)
{
	if (tag.payload.size() > kMaxFlvPayloadSize)
		throw std::length_error("FLV tag payload exceeds the 24-bit size field");

	std::vector<std::uint8_t> output;
	output.reserve(tag.wire_size());
	output.push_back(static_cast<std::uint8_t>(tag.type));
	append_be24(output, static_cast<std::uint32_t>(tag.payload.size()));
	append_be24(output, tag.timestamp_ms & 0x00ff'ffff);
	output.push_back(static_cast<std::uint8_t>(tag.timestamp_ms >> 24));
	append_be24(output, 0);
	output.insert(output.end(), tag.payload.begin(), tag.payload.end());
	append_be32(output, static_cast<std::uint32_t>(kFlvTagHeaderSize + tag.payload.size()));
	return output;
}

} // namespace active_delay
