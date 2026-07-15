#include "obs-packet-copy.hpp"

#include <memory>

extern "C" {
#include <obs-avc.h>
}

namespace active_delay {

EncodedPacket copy_encoder_packet(const encoder_packet &packet)
{
	encoder_packet parsed_packet{};
	auto release_parsed_packet = [](encoder_packet *value) {
		if (value)
			obs_encoder_packet_release(value);
	};
	std::unique_ptr<encoder_packet, decltype(release_parsed_packet)> parsed_packet_guard(nullptr,
		release_parsed_packet);
	const encoder_packet *source = &packet;
	if (packet.type == OBS_ENCODER_VIDEO) {
		// OBS H.264 encoders provide Annex-B NAL units. FLV/RTMP requires AVC
		// length-prefixed NAL units, matching the conversion performed by OBS's
		// built-in RTMP and FLV outputs before they serialize a video packet.
		obs_parse_avc_packet(&parsed_packet, &packet);
		parsed_packet_guard.reset(&parsed_packet);
		source = &parsed_packet;
	}

	EncodedPacket copy;
	copy.kind = source->type == OBS_ENCODER_VIDEO ? PacketKind::Video : PacketKind::Audio;
	copy.payload.assign(source->data, source->data + source->size);
	copy.pts_us = source->timebase_den != 0
		? (source->pts * 1'000'000LL * source->timebase_num) / source->timebase_den
		: source->dts_usec;
	copy.dts_us = source->dts_usec;
	copy.keyframe = source->keyframe;
	return copy;
}

} // namespace active_delay
