#pragma once
#include "flv-muxer.hpp"
#include <stdexcept>
#include <span>
#include <limits>

namespace active_delay {
inline constexpr std::int64_t transition_max_composition_us=250000;
inline bool transition_composition_supported(PacketKind kind,long double composition) {
 return composition>=0 && composition<=transition_max_composition_us && (kind!=PacketKind::Audio || composition==0);
}
inline void validate_transition_packet_timing(const EncodedPacket &p) {
 // Validate endpoints before subtraction; leave headroom for guard durations.
 if(p.dts_us<0 || p.pts_us<p.dts_us || p.dts_us>std::numeric_limits<std::int64_t>::max()-1000000 ||
    p.pts_us>std::numeric_limits<std::int64_t>::max()-1000000 ||
    !transition_composition_supported(p.kind,static_cast<long double>(p.pts_us-p.dts_us)))
  throw std::runtime_error("TRANSITION_TIMING_UNSUPPORTED: source presentation clock is outside the admitted codec timeline");
}
// Deliberately small admission contract, not a general H.264/AAC decoder.
// ASC: ISO 14496-3 AudioSpecificConfig/GASpecificConfig; AVC: SPS/VUI
// syntax as cross-checked against the local SDK and FFmpeg n8.1.2 parsers.
inline void unsupported_transition_codec() {
 throw std::runtime_error("TRANSITION_CODEC_UNSUPPORTED: requires bounded progressive AVC and 1024-sample LC AAC at 44.1/48 kHz mono/stereo");
}
class TransitionBits {
public:
 explicit TransitionBits(std::span<const std::uint8_t> bytes):bytes_(bytes){}
 std::uint32_t get(unsigned count) {
  if(count>32 || count>bytes_.size()*8-position_)unsupported_transition_codec();
  std::uint32_t value=0;
  for(unsigned i=0;i<count;++i,++position_)value=(value<<1)|((bytes_[position_/8]>>(7-position_%8))&1);
  return value;
 }
 std::uint32_t ue() {
  unsigned zero=0;
  while(get(1)==0)if(++zero>24)unsupported_transition_codec();
  return ((1U<<zero)-1)+get(zero);
 }
private:
 std::span<const std::uint8_t> bytes_;
 std::size_t position_=0;
};
inline std::int64_t transition_aac_duration(const FlvCodecHeaders &headers) {
 const auto &c=headers.aac_audio_specific_config;
 if(c.size()!=2 && c.size()!=5)unsupported_transition_codec();
 TransitionBits bits(c);
 if(bits.get(5)!=2)unsupported_transition_codec();
 const auto rate=bits.get(4),channels=bits.get(4);
 if((rate!=3 && rate!=4) || (channels!=1 && channels!=2) || bits.get(3)!=0)unsupported_transition_codec();
 // Only the explicit SBR-absent sync extension is admitted. No implicit HE,
 // PS, PCE, 960-sample frames, core dependency, or unknown trailing syntax.
 if(c.size()==5 && (bits.get(11)!=0x2b7 || bits.get(5)!=5 || bits.get(1)!=0 || bits.get(7)!=0))unsupported_transition_codec();
 const auto hz=rate==3 ? 48000 : 44100;
 return (1024LL*1000000+hz-1)/hz;
}
inline std::int64_t transition_video_duration(const FlvCodecHeaders &headers) {
 const auto &c=headers.avc_decoder_configuration;
 if(c.size()<9 || c.size()>4096 || c[0]!=1 || (c[4]&3)!=3 || (c[5]&31)!=1)unsupported_transition_codec();
 const std::size_t size=(std::size_t(c[6])<<8)|c[7];
 if(size<5 || 8+size+3>c.size() || (c[8]&31)!=7)unsupported_transition_codec();
 // One SPS and one PPS only; do not guess which of multiple timelines applies.
 const auto pps=8+size;
 const std::size_t pps_size=(std::size_t(c[pps+1])<<8)|c[pps+2];
 if(c[pps]!=1 || pps_size<2 || pps+3+pps_size>c.size() || (c[pps+3]&31)!=8)unsupported_transition_codec();
 const auto end=pps+3+pps_size;
 if(end!=c.size() && !(c[1]==100 && c.size()==end+4 && c[end]==0xfd && c[end+1]==0xf8 && c[end+2]==0xf8 && c[end+3]==0))unsupported_transition_codec();
 std::vector<std::uint8_t> rbsp;
 for(std::size_t i=9;i<8+size;++i) {
  if(i>=11 && c[i]==3 && c[i-1]==0 && c[i-2]==0) {
   if(i+1>=8+size || c[i+1]>3)unsupported_transition_codec();
   continue;
  }
  rbsp.push_back(c[i]);
 }
 TransitionBits b(rbsp);
 const auto profile=b.get(8);b.get(8);b.get(8);
 if(profile!=c[1] || (profile!=66 && profile!=77 && profile!=100) || b.ue()>31)unsupported_transition_codec();
 if(profile==100) {
  if(b.ue()!=1 || b.ue()!=0 || b.ue()!=0 || b.get(1)!=0 || b.get(1)!=0)unsupported_transition_codec();
 }
 if(b.ue()>12)unsupported_transition_codec(); // log2_max_frame_num_minus4
 const auto poc=b.ue();
 if(poc==0){if(b.ue()>12)unsupported_transition_codec();}
 else if(poc!=2)unsupported_transition_codec();
 if(b.ue()>16 || b.get(1)!=0)unsupported_transition_codec();
 if(b.ue()>511 || b.ue()>511 || b.get(1)!=1)unsupported_transition_codec(); // progressive only
 b.get(1);
 if(b.get(1))for(int i=0;i<4;++i)b.ue();
 if(!b.get(1))unsupported_transition_codec(); // VUI required
 if(b.get(1)){if(b.get(8)==255){b.get(16);b.get(16);}}
 if(b.get(1))b.get(1);
 if(b.get(1)){b.get(3);b.get(1);if(b.get(1)){b.get(8);b.get(8);b.get(8);}}
 if(b.get(1)){b.ue();b.ue();}
 if(!b.get(1))unsupported_transition_codec();
 const auto units=b.get(32),scale=b.get(32);b.get(1);
 if(!units || !scale)unsupported_transition_codec();
 const auto duration=(2000000ULL*units+scale-1)/scale;
 if(duration<4000 || duration>100000)unsupported_transition_codec();
 // HRD/picture-structure extensions require a different timing proof.
 if(b.get(1) || b.get(1) || b.get(1) || !b.get(1))unsupported_transition_codec();
 b.get(1);b.ue();b.ue();b.ue();b.ue();
 const auto reorder=b.ue(),buffer=b.ue();
 if(reorder>buffer || buffer>16 || reorder*duration>transition_max_composition_us)unsupported_transition_codec();
 return static_cast<std::int64_t>(duration);
}
} // namespace active_delay
