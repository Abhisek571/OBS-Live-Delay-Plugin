#include "transition-coordinator.hpp"
#include "transition-codec.hpp"
#include "transition-test-headers.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
using namespace active_delay;
namespace {
void require(bool v, const char *m) { if (!v) throw std::runtime_error(m); }
EncodedPacket packet(PacketKind kind, int64_t t, bool key, uint8_t id) {
 return {kind, kind == PacketKind::Video ? std::vector<uint8_t>{0,0,0,2,0x65,id} : std::vector<uint8_t>{id}, t+2000,kind==PacketKind::Audio ? t+2000 : t,key};
}
struct Recorder : ReleasedPacketConsumer {
 std::vector<ReleasedPacketBatch> batches;
 void consume(const std::shared_ptr<const ReleasedPacketBatch> &b) override { batches.push_back(*b); }
 void discontinuity(const PacketDiscontinuity &) override {}
 void stop() noexcept override {}
};
void independent_holding_keeps_programme_capture() {
 DelayController controller; ReleasedPacketDispatcher dispatcher;
 auto recorder=std::make_shared<Recorder>(); dispatcher.add_consumer(recorder);
 TransitionCoordinator coordinator(controller,dispatcher,1,programme_test_headers());
 coordinator.holding_ready(holding_test_headers());
 coordinator.request({TransitionAction::SetDelay,Microseconds{100000}});
 coordinator.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 require(recorder->batches.size()==1,"holding must publish while compressed programme builds");
 require(recorder->batches[0].headers->avc_decoder_configuration[1]==66,"holding must use own headers");
 coordinator.programme(packet(PacketKind::Video,0,true,0x50));
 coordinator.programme(packet(PacketKind::Audio,1000,false,0x77));
 coordinator.programme(packet(PacketKind::Video,110000,false,0x51));
 coordinator.programme(packet(PacketKind::Audio,111000,false,0x78));
 require(recorder->batches.size()>=2,"delayed original programme must emerge from behind holding");
 auto &last=recorder->batches.back();
 require(last.headers && last.headers->avc_decoder_configuration[1]==100,"programme switch must restore own headers");
 require(last.packets.front().keyframe,"programme switch must begin at keyframe");
 require(last.packets.front().dts_us>recorder->batches[0].packets.back().dts_us,"switch must continue outgoing DTS");
 require(last.packets.front().pts_us-last.packets.front().dts_us==2000,"composition offset preserved");
}
void return_live_and_dump_keep_safe_boundaries() {
 DelayController controller; ReleasedPacketDispatcher dispatcher;
 auto recorder=std::make_shared<Recorder>(); dispatcher.add_consumer(recorder);
 TransitionCoordinator coordinator(controller,dispatcher,1,programme_test_headers());
 coordinator.holding_ready(holding_test_headers());
 coordinator.request({TransitionAction::SetDelay,Microseconds{100000}});
 coordinator.programme(packet(PacketKind::Video,0,true,0x50));
 coordinator.programme(packet(PacketKind::Audio,1000,false,0x77));
 coordinator.programme(packet(PacketKind::Video,110000,false,0x51));
 coordinator.programme(packet(PacketKind::Audio,111000,false,0x78));
 coordinator.request({TransitionAction::EmergencyDump,{}});
 require(controller.status().target_delay==Microseconds{100000},"dump must retain requested target");
 require(controller.status().buffered_bytes==0,"dump must discard retained programme");
 coordinator.holding({packet(PacketKind::Audio,2000000,false,0),packet(PacketKind::Video,2000001,false,0x48)});
 const auto before=recorder->batches.size();
 coordinator.request({TransitionAction::ReturnLive,{}});
 coordinator.holding({packet(PacketKind::Video,2100000,true,0x48),packet(PacketKind::Audio,2101000,false,0)});
 coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(100));
 const auto after_guard=recorder->batches.size();
 coordinator.programme(packet(PacketKind::Audio,300000,false,0xee));
 coordinator.programme(packet(PacketKind::Video,301000,false,0xee));
 require(recorder->batches.size()==after_guard,"Return Live must reject dependent/pre-keyframe current content");
 coordinator.programme(packet(PacketKind::Video,310000,true,0x52));
 coordinator.programme(packet(PacketKind::Audio,309000,false,0xee));
 coordinator.programme(packet(PacketKind::Audio,311000,false,0x79));
 coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(200));
 coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(300));
 require(recorder->batches.size()>after_guard && coordinator.programme_selected(),"fresh live keyframe and paired audio must complete paced boundary");
 for(std::size_t i=after_guard;i<recorder->batches.size();++i)for(const auto &p:recorder->batches[i].packets) require(p.payload.back()!=0xee,"pre-cutoff audio must never leak");
 require(controller.status().target_delay==Microseconds{0},"Return Live clears target");
}

void missing_fresh_keyframe_times_out_fail_closed(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 TransitionCoordinator coordinator(controller,dispatcher,1,programme_test_headers());
 bool failed=false;
 try{coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::seconds(6));}
 catch(const std::runtime_error &){failed=true;}
 require(failed,"missing safe fresh source boundary must fail closed within readiness budget");
}

void initial_mid_gop_wait_has_bounded_fail_closed_deadline(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 auto recorder=std::make_shared<Recorder>();dispatcher.add_consumer(recorder);
 TransitionCoordinator coordinator(controller,dispatcher,1,programme_test_headers());
 coordinator.holding_ready(holding_test_headers());
 coordinator.request({TransitionAction::SetDelay,Microseconds{100000}});
 coordinator.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 coordinator.programme(packet(PacketKind::Video,0,false,0xee));
 coordinator.programme(packet(PacketKind::Audio,110000,false,0xee));
 bool failed=false;
 try{coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::seconds(6));}
 catch(const std::runtime_error &e){failed=std::string(e.what()).starts_with("PROGRAMME_KEYFRAME_TIMEOUT");}
 require(failed && recorder->batches.size()==1,"initial mid-GOP programme cannot leak or wait without bounded keyframe failure behind independent holding");
}

void drain_precedes_paced_holding_instead_of_timestamp_only_burst(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 auto recorder=std::make_shared<Recorder>();dispatcher.add_consumer(recorder);
 TransitionCoordinator coordinator(controller,dispatcher,1,programme_test_headers());
 coordinator.holding_ready(holding_test_headers());
 coordinator.programme({PacketKind::Video,{0,0,0,2,0x65,0x50},0,0,true});
 coordinator.programme({PacketKind::Audio,{0x77},1000,1000,false});
 const auto before=recorder->batches.size();
 coordinator.request({TransitionAction::SetDelay,Microseconds{100000}});
 coordinator.holding({{PacketKind::Video,{0,0,0,2,0x65,0x48},1000000,1000000,true},
                      {PacketKind::Audio,{0},1001000,1001000,false}});
 require(recorder->batches.size()>before && std::all_of(recorder->batches.begin()+before,recorder->batches.end(),[](const auto &batch){return std::all_of(batch.packets.begin(),batch.packets.end(),[](const auto &p){return p.kind==PacketKind::Audio;});}),
  "guard must send only drain AAC now, not burst future holding video behind timestamp edits");
 coordinator.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(100));
 require(recorder->batches.size()>before+1 && recorder->batches.back().packets.front().kind==PacketKind::Video,
  "bounded codec-duration clock must release the actual holding picture after the guard");
}

void paced_holding_survives_long_rebuild_and_waits_for_delivery(){
 struct AckRecorder : Recorder {
  bool ack=true;
  bool delivered(std::uint64_t,std::uint64_t) const override {return ack;}
 };
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 auto sink=std::make_shared<AckRecorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());
 c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::SetDelay,Microseconds{10000000}});
 const auto now=std::chrono::steady_clock::now();
 sink->ack=false;
 c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 const auto count=sink->batches.size();
 c.check_deadline(now+std::chrono::milliseconds(100));
 require(sink->batches.size()==count,"undelivered drain must not publish blue even after media duration");
 sink->ack=true;c.check_deadline(now+std::chrono::milliseconds(110));
 require(sink->batches.size()==count,"drain duration begins at delivery acknowledgement, not enqueue");
 for(int i=1;i<120;++i){
  const auto t=1000000+i*33333LL;
  c.holding({packet(PacketKind::Video,t,i%30==0,0x48),packet(PacketKind::Audio,t+1000,false,0)});
  c.check_deadline(now+std::chrono::milliseconds(110)+Microseconds{i*33333LL});
 }
 require(sink->batches.back().packets.back().payload.back()==0,"holding remains available across long compressed rebuild");
}
void fast_programme_cannot_cancel_queued_guard(){
 struct AckRecorder : Recorder {bool ack=true;bool delivered(std::uint64_t,std::uint64_t)const override{return ack;}};
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<AckRecorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::ReturnLive,{}});const auto epoch=c.epoch();sink->ack=false;
 c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 c.programme(packet(PacketKind::Video,100000,true,0x50));c.programme(packet(PacketKind::Audio,101000,false,0x77));
 c.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(100));
 require(c.epoch()==epoch && !c.programme_selected(),"fast ready programme must not invalidate an undelivered guard");
 bool failed=false;try{c.check_deadline(std::chrono::steady_clock::now()+std::chrono::seconds(6));}catch(const std::runtime_error &e){failed=std::string(e.what()).starts_with("TRANSITION_GUARD_TIMEOUT");}
 require(failed,"undelivered drain fails closed on bounded watchdog");
}

void codec_eligibility_is_fail_closed(){
 for(int scenario=0;scenario<4;++scenario){
  DelayController controller;ReleasedPacketDispatcher dispatcher;
  auto h=holding_test_headers();
  if(scenario==0)h.avc_decoder_configuration={1,66,0,31};
  if(scenario==1)h.aac_audio_specific_config={0x29,0x90}; // HE-AAC
  if(scenario==2)h.aac_audio_specific_config={0x11,0x94}; // 960 sample flag
  if(scenario==3)h.aac_audio_specific_config={0x11,0x90,0x56,0xe5,0x80}; // SBR present
  bool rejected=false;
  try{TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(h);c.request({TransitionAction::ReturnLive,{}});}
  catch(const std::runtime_error &e){rejected=std::string(e.what()).starts_with("TRANSITION_CODEC_UNSUPPORTED");}
  require(rejected,"unknown/truncated AVC timing or non-LC AAC must fail closed before holding");
 }
 for(auto asc: {std::vector<std::uint8_t>{0x11,0x88,0x56,0xe5,0},std::vector<std::uint8_t>{0x12,0x08,0x56,0xe5,0}}){
  DelayController controller;ReleasedPacketDispatcher dispatcher;auto p=programme_test_headers(),h=holding_test_headers();p.aac_audio_specific_config=h.aac_audio_specific_config=asc;
  TransitionCoordinator c(controller,dispatcher,1,p);c.holding_ready(h);c.request({TransitionAction::ReturnLive,{}});
 }
}

void resumed_programme_audio_waits_for_visible_picture(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::ReturnLive,{}});
 c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 auto now=std::chrono::steady_clock::now();c.check_deadline(now+std::chrono::milliseconds(100));
 auto key=packet(PacketKind::Video,100000,true,0x50);key.pts_us=167000;
 c.programme(key);c.programme(packet(PacketKind::Audio,170000,false,0x77));
 const auto before=sink->batches.size();c.check_deadline(now+std::chrono::milliseconds(110));
 for(std::size_t i=before;i<sink->batches.size();++i)for(const auto &p:sink->batches[i].packets)
  require(p.kind!=PacketKind::Audio || p.payload.back()!=0x77,"resumed programme audio cannot be sent at the bridge's earlier wall time");
 c.check_deadline(now+std::chrono::milliseconds(300));
 require(c.programme_selected(),"programme resumes after paced visible boundary");
}

void programme_boundary_delivery_timeout_is_not_enqueue_success(){
 struct AckRecorder : Recorder {bool ack=true;bool delivered(std::uint64_t,std::uint64_t)const override{return ack;}};
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<AckRecorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::ReturnLive,{}});c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 const auto now=std::chrono::steady_clock::now();c.check_deadline(now+std::chrono::milliseconds(100));
 c.programme(packet(PacketKind::Video,100000,true,0x50));c.programme(packet(PacketKind::Audio,101000,false,0x77));
 c.check_deadline(now+std::chrono::milliseconds(110));sink->ack=false;c.check_deadline(now+std::chrono::milliseconds(300));
 bool failed=false;try{c.check_deadline(now+std::chrono::seconds(6));}catch(const std::runtime_error &e){failed=std::string(e.what()).starts_with("TRANSITION_GUARD_TIMEOUT");}
 require(failed,"queued programme boundary cannot remain indefinitely pending after lost transport delivery");
}

void timed_work_is_revoked_and_bounded(){
 for(bool pressure:{false,true}) {
  DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
  TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
  c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
  c.request({TransitionAction::SetDelay,Microseconds{10000000}});
  c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
  const auto count=sink->batches.size();
  if(!pressure){
   const auto floor=dispatcher.reserve_epoch(c.epoch());(void)dispatcher.dispatch_discontinuity({floor,"superseded guard"});
   c.check_deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(100));
   require(sink->batches.size()==count,"superseding acceptance must revoke already-timed holding packets");
  }else{
   bool failed=false;try{c.holding({packet(PacketKind::Video,4000000,false,0x48)});}catch(const std::runtime_error &e){failed=std::string(e.what()).starts_with("TRANSITION_GUARD_OVERFLOW");}
   require(failed,"future scheduled capture has a hard two-second compressed queue budget");
  }
 }
}

void superseding_guard_does_not_rebase_from_unpublished_future(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::SetDelay,Microseconds{10000000}});
 c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0),packet(PacketKind::Video,1900000,false,0x48),packet(PacketKind::Audio,1901000,false,0)});
 const auto first_drain=sink->batches.back().packets.back().dts_us;
 c.request({TransitionAction::ReturnLive,{}});
 c.holding({packet(PacketKind::Video,5000000,true,0x48),packet(PacketKind::Audio,5001000,false,0)});
 require(sink->batches.back().packets.back().dts_us-first_drain<100000,"revoked future holding timestamps cannot create an unpaced jump in the next drain");
}

void stale_epoch_programme_is_still_buffered(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.request({TransitionAction::SetDelay,Microseconds{10000000}});
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 const auto before=controller.status().buffered_bytes;
 // The owner admitted another action; this tick still carries older programme.
 const auto floor=dispatcher.reserve_epoch(c.epoch());(void)dispatcher.dispatch_discontinuity({floor,"accepted action"});
 c.programme(packet(PacketKind::Video,33333,false,0x51));
 require(controller.status().buffered_bytes>before,"programme captured before an action is processed must stay in the delay buffer, not leave a hole");
}

void resumed_programme_stops_pacing_after_boundary_delivery(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.programme(packet(PacketKind::Video,0,true,0x50));c.programme(packet(PacketKind::Audio,1000,false,0x77));
 c.request({TransitionAction::ReturnLive,{}});
 c.holding({packet(PacketKind::Video,1000000,true,0x48),packet(PacketKind::Audio,1001000,false,0)});
 const auto now=std::chrono::steady_clock::now();c.check_deadline(now+std::chrono::milliseconds(100));
 c.programme(packet(PacketKind::Video,100000,true,0x50));c.programme(packet(PacketKind::Audio,101000,false,0x77));
 c.check_deadline(now+std::chrono::milliseconds(110));c.check_deadline(now+std::chrono::milliseconds(300));
 require(c.programme_selected(),"programme resumes after paced visible boundary");
 const auto before=sink->batches.size();
 // Three seconds of steady programme without a wall-clock tick in between.
 for(int i=1;i<=90;++i){
  const auto t=101000+i*33333LL;
  c.programme(packet(PacketKind::Video,t,false,0x51));c.programme(packet(PacketKind::Audio,t+1000,false,0x79));
 }
 require(sink->batches.size()>=before+90,"once the resume boundary is delivered, programme must flow directly instead of staying paced");
}

void switch_to_holding_has_no_audio_gap(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 // A B-frame composition offset pushes the video tail well past the audio end.
 c.programme({PacketKind::Video,{0,0,0,2,0x65,0x50},100000,0,true});
 c.programme({PacketKind::Audio,{0x77},1000,1000,false});
 const auto before=sink->batches.size();
 c.request({TransitionAction::SetDelay,Microseconds{10000000}});
 c.holding({{PacketKind::Video,{0,0,0,2,0x65,0x48},1000000,1000000,true},{PacketKind::Audio,{0},1001000,1001000,false}});
 c.check_deadline(std::chrono::steady_clock::now()+std::chrono::seconds(1));
 std::vector<EncodedPacket> audio;
 for(auto i=before;i<sink->batches.size();++i)for(const auto &p:sink->batches[i].packets)if(p.kind==PacketKind::Audio)audio.push_back(p);
 require(audio.size()>=2,"drain and holding audio must both be published");
 const auto duration=transition_aac_duration(holding_test_headers());
 for(std::size_t i=1;i<audio.size();++i)
  require(audio[i].pts_us-(audio[i-1].pts_us+duration)<duration+1000,"switching to holding must not leave an unfilled audio gap");
}
void rewind_mode_switches_programme_directly(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Recorder>();dispatcher.add_consumer(sink);
 std::string error;require(controller.arm(Microseconds{200000},error) && controller.start_live(),"rewind fixture arms and starts live");
 TransitionCoordinator c(controller,dispatcher,1,programme_test_headers());c.holding_ready(holding_test_headers());
 c.holding({packet(PacketKind::Video,0,true,0x48),packet(PacketKind::Audio,1000,false,0)});
 require(sink->batches.empty(),"holding stays off air while live");
 // Video id is the frame index, so a replay is visible as older ids.
 auto feed=[&](int from,int to){for(int i=from;i<=to;++i){c.programme(packet(PacketKind::Video,i*50000LL,i%2==0,uint8_t(i)));c.programme(packet(PacketKind::Audio,i*50000LL+1000,false,0x77));c.check_deadline();}};
 auto videos=[&](std::size_t from){std::vector<EncodedPacket> v;for(auto i=from;i<sink->batches.size();++i)for(const auto &p:sink->batches[i].packets)if(p.kind==PacketKind::Video)v.push_back(p);return v;};
 feed(0,10);
 require(!sink->batches.empty() && videos(0).back().payload.back()==10,"broadcast starts live");
 c.request({TransitionAction::SetDelay,Microseconds{200000}});
 const auto rewound=sink->batches.size();
 feed(11,14);
 auto replay=videos(rewound);
 require(!replay.empty() && replay.front().keyframe && replay.front().payload.back()<=6,"Start Delay replays kept history at once");
 require(replay.back().payload.back()<=10,"rewound programme stays behind live");
 c.request({TransitionAction::ReturnLive,{}});
 const auto returned=sink->batches.size();
 feed(15,18);
 auto current=videos(returned);
 require(!current.empty() && current.front().keyframe && current.front().payload.back()==16,"Return Live jumps to current programme");
 std::int64_t last=-1;
 for(const auto &b:sink->batches)for(const auto &p:b.packets){
  require(p.payload!=std::vector<uint8_t>{0,0,0,2,0x65,0x48},"direct switches never show holding video");
  require(p.dts_us>=last,"output timestamps keep moving forward across rewinds");last=p.dts_us;
 }
}
}
int main() { try { stale_epoch_programme_is_still_buffered(); resumed_programme_stops_pacing_after_boundary_delivery(); switch_to_holding_has_no_audio_gap();rewind_mode_switches_programme_directly(); superseding_guard_does_not_rebase_from_unpublished_future(); timed_work_is_revoked_and_bounded(); programme_boundary_delivery_timeout_is_not_enqueue_success(); resumed_programme_audio_waits_for_visible_picture(); codec_eligibility_is_fail_closed(); paced_holding_survives_long_rebuild_and_waits_for_delivery(); fast_programme_cannot_cancel_queued_guard(); independent_holding_keeps_programme_capture(); return_live_and_dump_keep_safe_boundaries(); missing_fresh_keyframe_times_out_fail_closed(); initial_mid_gop_wait_has_bounded_fail_closed_deadline(); drain_precedes_paced_holding_instead_of_timestamp_only_burst();std::cout<<"Transition tests passed\n"; }
 catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; } }
