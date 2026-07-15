#pragma once

#include "delay-controller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace active_delay {

enum class FlvTagType : std::uint8_t { Audio = 8, Video = 9, ScriptData = 18 };

struct FlvTag {
	FlvTagType type = FlvTagType::Video;
	std::uint32_t timestamp_ms = 0;
	std::vector<std::uint8_t> payload;
	bool keyframe = false;

	[[nodiscard]] std::size_t wire_size() const noexcept;
};

struct FlvCodecHeaders {
	std::vector<std::uint8_t> avc_decoder_configuration;
	std::vector<std::uint8_t> aac_audio_specific_config;
};

class FlvMuxer {
public:
	explicit FlvMuxer(FlvCodecHeaders headers);

	[[nodiscard]] std::vector<FlvTag> sequence_headers() const;
	bool mux(std::vector<EncodedPacket> packets, std::vector<FlvTag> &tags, std::string &error);
	void reset_timeline() noexcept;

private:
	FlvCodecHeaders headers_;
	bool has_base_timestamp_ = false;
	std::int64_t base_dts_us_ = 0;
	std::int64_t last_dts_us_ = 0;
};

[[nodiscard]] std::vector<std::uint8_t> make_flv_header();
[[nodiscard]] std::vector<std::uint8_t> serialize_flv_tag(const FlvTag &tag);

} // namespace active_delay
