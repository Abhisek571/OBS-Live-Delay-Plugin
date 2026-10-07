#include "holding-obs-packet.hpp"
#include "holding-feed.hpp"
#include "obs-packet-copy.hpp"
#include "transition-codec.hpp"
#include <limits>
#include <stdexcept>
#include <algorithm>
extern "C" {
#include <obs-avc.h>
#include <util/bmem.h>
}

namespace active_delay {
EncodedPacket copy_holding_encoder_packet(const encoder_packet &packet)
{
	if ((packet.type != OBS_ENCODER_VIDEO && packet.type != OBS_ENCODER_AUDIO) ||
		packet.timebase_num <= 0 || packet.timebase_den <= 0 || !packet.data || !packet.size ||
		packet.size > HoldingFeed::queue_byte_limit || packet.sys_dts_usec < 0 ||
		packet.sys_dts_usec > std::numeric_limits<int64_t>::max() - HoldingFeed::queue_duration_us)
		throw std::invalid_argument("HOLDING_PACKET_INVALID: invalid holding timing or payload");
	if(packet.pts<packet.dts)throw std::invalid_argument("HOLDING_PACKET_INVALID: negative composition offset");
	const long double composition = static_cast<long double>(static_cast<uint64_t>(packet.pts)-static_cast<uint64_t>(packet.dts)) *
		1'000'000.0L * packet.timebase_num / packet.timebase_den;
	if (!transition_composition_supported(packet.type==OBS_ENCODER_VIDEO ? PacketKind::Video : PacketKind::Audio,composition) ||
		(packet.type==OBS_ENCODER_AUDIO && packet.pts!=packet.dts))
		throw std::invalid_argument("HOLDING_PACKET_INVALID: excessive holding composition offset");
	// Reuse payload/Annex-B conversion, but do not run the programme helper's
	// relative-PTS arithmetic on these absolute-clock capture packets.
	auto payload_packet = packet;
	payload_packet.pts = payload_packet.dts = payload_packet.dts_usec = 0;
	payload_packet.timebase_num = payload_packet.timebase_den = 1;
	auto copy = copy_encoder_packet(payload_packet);
	copy.dts_us = packet.sys_dts_usec;
	copy.pts_us = packet.sys_dts_usec + static_cast<int64_t>(composition);
	validate_transition_packet_timing(copy);
	return copy;
}
void ObsCaptureCodecMonitor::reset()
{
 std::scoped_lock lock(mutex_);headers_={};video_extra_.clear();audio_extra_.clear();
}
MonitoredCapturePacket ObsCaptureCodecMonitor::observe(const encoder_packet &packet,obs_encoder_t *encoder)
{
 uint8_t *data=nullptr;size_t size=0;
 if(!encoder || !obs_encoder_get_extra_data(encoder,&data,&size) || !data)
  throw std::runtime_error("CAPTURE_HEADERS_UNAVAILABLE: encoder configuration unavailable at packet boundary");
 return observe(packet,std::span<const uint8_t>(data,size));
}
MonitoredCapturePacket ObsCaptureCodecMonitor::observe(const encoder_packet &packet,std::span<const uint8_t> extra)
{
 std::scoped_lock lock(mutex_);
 const bool video=packet.type==OBS_ENCODER_VIDEO;
 if(extra.empty() || extra.size()>(video ? 4096U : 5U))
  throw std::runtime_error("CAPTURE_HEADERS_INVALID: missing or oversized codec configuration");
 auto &original=video ? video_extra_ : audio_extra_;
 if(!original.empty() && !std::equal(original.begin(),original.end(),extra.begin(),extra.end()))
  throw std::runtime_error("CAPTURE_CODEC_CHANGED: encoder configuration changed during capture");
 if(original.empty()) {
  if(video) {
   uint8_t *avc=nullptr;const auto size=obs_parse_avc_header(&avc,extra.data(),extra.size());
   if(!avc || !size){bfree(avc);throw std::runtime_error("CAPTURE_HEADERS_INVALID: AVC configuration unavailable");}
   try{headers_.avc_decoder_configuration.assign(avc,avc+size);}catch(...){bfree(avc);throw;}
   bfree(avc);(void)transition_video_duration(headers_);
  }else{
   headers_.aac_audio_specific_config.assign(extra.begin(),extra.end());(void)transition_aac_duration(headers_);
  }
  original.assign(extra.begin(),extra.end());
 }
 auto copy=copy_holding_encoder_packet(packet);
 if(video) {
  // OBS conversion preserves every Annex-B NAL as a length-prefixed NAL.
  // Compare parameter sets even when the encoder's extradata stayed stale.
  // Headers were validated once above (exactly one SPS/PPS, 4-byte lengths).
  const auto &c=headers_.avc_decoder_configuration;
  const size_t sps_size=(size_t(c[6])<<8)|c[7],pps=8+sps_size;
  const size_t pps_size=(size_t(c[pps+1])<<8)|c[pps+2];
  const auto bytes=std::span<const uint8_t>(copy.payload);
  if(bytes.empty())throw std::runtime_error("CAPTURE_PACKET_INVALID: empty AVC packet");
  for(size_t pos=0;pos<bytes.size();) {
   if(bytes.size()-pos<4)throw std::runtime_error("CAPTURE_PACKET_INVALID: truncated AVC length");
   const size_t size=(size_t(bytes[pos])<<24)|(size_t(bytes[pos+1])<<16)|(size_t(bytes[pos+2])<<8)|bytes[pos+3];pos+=4;
   if(!size || size>bytes.size()-pos)throw std::runtime_error("CAPTURE_PACKET_INVALID: invalid AVC length");
   const auto nal=bytes.subspan(pos,size);const auto type=nal[0]&31;
   if(type==7 || type==8) {
    const auto expected=std::span<const uint8_t>(c).subspan(type==7 ? 8 : pps+3,type==7 ? sps_size : pps_size);
    if(!std::equal(nal.begin(),nal.end(),expected.begin(),expected.end()))
     throw std::runtime_error("CAPTURE_CODEC_CHANGED: in-band AVC parameter set changed");
   }else if(type==13 || type==15)throw std::runtime_error("CAPTURE_CODEC_CHANGED: unsupported AVC parameter set extension");
   pos+=size;
  }
 }
 // AAC payload is opaque: configuration is read only from encoder extradata.
 std::optional<FlvCodecHeaders> headers;
 if(!video_extra_.empty() && !audio_extra_.empty())headers=headers_;
 return {std::move(copy),std::move(headers)};
}
} // namespace active_delay