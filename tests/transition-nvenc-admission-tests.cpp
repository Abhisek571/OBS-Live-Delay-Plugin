#include "transition-codec.hpp"
#include "transition-test-headers.hpp"
#include <iostream>
#include <string>

// Investigation regression only: no media payload is rewritten or published.
// The captured SPS/PPS are from the installed OBS NVENC synthetic fixture.
// Generated SPS variants isolate admission gates; they are NOT GPU evidence.
using namespace active_delay;
namespace {
void require(bool value, const char *message) {
 if (!value) throw std::runtime_error(message);
}
std::vector<uint8_t> hex(const std::string &text) {
 std::vector<uint8_t> bytes;
 for (size_t i=0;i<text.size();i+=2)
  bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i,2),nullptr,16)));
 return bytes;
}
const auto captured_sps=hex("6764000dac2ca505067e7c05a808080a000007d00001d4c1c0000bb8001770dde5c140");
const auto captured_pps=hex("68eb8f2c");
FlvCodecHeaders headers(const std::vector<uint8_t> &sps) {
 FlvCodecHeaders h;
 auto &c=h.avc_decoder_configuration;
 c={1,sps[1],sps[2],sps[3],255,225,
    static_cast<uint8_t>(sps.size()>>8),static_cast<uint8_t>(sps.size())};
 c.insert(c.end(),sps.begin(),sps.end());
 c.insert(c.end(),{1,0,static_cast<uint8_t>(captured_pps.size())});
 c.insert(c.end(),captured_pps.begin(),captured_pps.end());
 h.aac_audio_specific_config={0x11,0x90};
 return h;
}
bool rejected(const FlvCodecHeaders &h) {
 try { (void)transition_video_duration(h); }
 catch (const std::runtime_error &e) {
  require(std::string(e.what()).starts_with("TRANSITION_CODEC_UNSUPPORTED"),"unexpected diagnostic");
  return true;
 }
 return false;
}
std::string ue(unsigned value) {
 std::string bits;
 for (++value;value;value>>=1) bits.insert(bits.begin(),(value&1)?'1':'0');
 return std::string(bits.size()-1,'0')+bits;
}
std::vector<uint8_t> isolated_sps(bool hrd, bool pic_struct, bool restriction) {
 std::string captured;
 for (auto byte:captured_sps)
  for (int i=7;i>=0;--i) captured+=(byte&(1<<i))?'1':'0';
 // FFmpeg trace_headers bit positions include the NAL header. The captured
 // SPS contains no emulation-prevention bytes; prefix ends after fixed rate.
 require(captured.size()==280 && captured[192]=='1' && captured[271]=='1' && captured[272]=='0',
         "captured SPS offsets changed");
 auto bits=captured.substr(0,192)+(hrd?"1":"0");
 if (hrd) bits+=captured.substr(193,76);
 bits+='0'; // no VCL HRD
 if (hrd) bits+='0'; // low_delay_hrd_flag
 bits+=pic_struct?'1':'0';
 bits+=restriction?'1':'0';
 if (restriction) bits+="1"+ue(0)+ue(0)+ue(10)+ue(10)+ue(2)+ue(4);
 bits+='1'; // rbsp_stop_one_bit
 while (bits.size()%8) bits+='0';
 std::vector<uint8_t> nal;
 unsigned zeros=0;
 for (size_t pos=0;pos<bits.size();pos+=8) {
  const auto byte=static_cast<uint8_t>(std::stoul(bits.substr(pos,8),nullptr,2));
  if (zeros==2 && byte<=3) { nal.push_back(3); zeros=0; }
  nal.push_back(byte); zeros=byte==0?zeros+1:0;
 }
 return nal;
}
}
int main(int argc,char **argv) {
 try {
  const auto captured=headers(captured_sps);
  if (argc==2 && std::string(argv[1])=="--require-nvenc") {
   require(transition_video_duration(captured)==33334,"NVENC frame duration mismatch");
   return 0;
  }
  require(rejected(captured),"unproved NVENC timing must remain fail closed");
  require(transition_video_duration(programme_test_headers())==33334,"x264 programme regressed");
  require(transition_video_duration(holding_test_headers())==33334,"x264 holding regressed");
  require(transition_video_duration(headers(isolated_sps(false,false,true)))==33334,
          "generated control must be admitted before testing independent gates");
  require(rejected(headers(isolated_sps(true,false,true))),"HRD gate disappeared");
  require(rejected(headers(isolated_sps(false,true,true))),"picture-structure gate disappeared");
  require(rejected(headers(isolated_sps(false,false,false))),"missing reorder bound gate disappeared");
  for (size_t n=0;n<captured.avc_decoder_configuration.size();++n) {
   auto truncated=captured;truncated.avc_decoder_configuration.resize(n);
   require(rejected(truncated),"truncated captured configuration admitted");
  }
  // H.264 A.3.1/Table A-1 and E.2.1: High profile, constraint_set3=0,
  // level 1.3, coded 20x12 macroblocks (NOT cropped display dimensions).
  constexpr auto inferred_reorder=2376/(20*12);
  static_assert(inferred_reorder==9);
  require(inferred_reorder*33334LL>transition_max_composition_us,
          "conservative absent-restriction inference no longer exceeds current budget");
  std::cout<<"PASS: captured rejection, x264 controls, three isolated gates, all truncated prefixes; "
           <<"inferred reorder="<<inferred_reorder<<", conservative span="<<inferred_reorder*33334LL<<" us\n";
  return 0;
 } catch (const std::exception &e) {
  std::cerr<<e.what()<<'\n';return 1;
 }
}