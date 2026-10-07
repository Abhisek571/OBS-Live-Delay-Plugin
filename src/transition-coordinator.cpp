#include "transition-coordinator.hpp"
#include "transition-codec.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace active_delay {
namespace {
std::int64_t ceil_ms(std::int64_t t) {
 if(t<0 || t>std::numeric_limits<std::int64_t>::max()-999)
  throw std::runtime_error("TRANSITION_TIMESTAMP_OVERFLOW");
 return ((t+999)/1000)*1000;
}
}
TransitionCoordinator::TransitionCoordinator(DelayController &controller, ReleasedPacketDispatcher &dispatcher,
 std::uint64_t epoch, FlvCodecHeaders headers)
 : controller_(controller), dispatcher_(dispatcher), programme_headers_(std::move(headers)),
   epoch_(dispatcher.reserve_epoch(epoch ? epoch-1 : 0)) {
 (void)transition_aac_duration(programme_headers_);
 programme_frame_us_=transition_video_duration(programme_headers_);
}
void TransitionCoordinator::holding_ready(FlvCodecHeaders headers) {
 if(!holding_headers_.avc_decoder_configuration.empty() &&
    (holding_headers_.avc_decoder_configuration!=headers.avc_decoder_configuration || holding_headers_.aac_audio_specific_config!=headers.aac_audio_specific_config))unsupported_transition_codec();
 if(!holding_headers_.avc_decoder_configuration.empty())return;
 // Preflight before the owner advertises protected holding as available.
 (void)transition_aac_duration(programme_headers_);(void)transition_aac_duration(headers);
 programme_frame_us_=transition_video_duration(programme_headers_);
 holding_frame_us_=transition_video_duration(headers);
 if(programme_headers_.aac_audio_specific_config!=headers.aac_audio_specific_config)unsupported_transition_codec();
 holding_headers_ = std::move(headers);
}
bool TransitionCoordinator::invalidate(std::uint64_t reserved_epoch) {
 if(reserved_epoch) epoch_=reserved_epoch;
 else {
  if(!dispatcher_.advance_epoch(epoch_))return false;
  ++epoch_;
 }
 boundary_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 programme_deadline_.reset();
 programme_key_deadline_=boundary_deadline_;
 selected_.reset(); cutoff_.reset(); boundary_.clear(); boundary_bytes_=0;
 const auto failures=dispatcher_.dispatch_discontinuity({epoch_,"Feed transition"});
 if (!failures.empty()) throw std::runtime_error(failures.front().error);
 return true;
}
void TransitionCoordinator::request(TransitionRequest request) {
 if (holding_headers_.avc_decoder_configuration.empty() || holding_headers_.aac_audio_specific_config.empty())
  throw std::runtime_error("HOLDING_NOT_READY: independent holding and silence are unavailable");
 std::string error;
 if (request.action==TransitionAction::SetDelay) {
  if (!controller_.set_target(request.target,&error)) throw std::runtime_error(error);
 } else if (request.action==TransitionAction::ReturnLive) {
  controller_.return_live();
 } else {
  const auto target=controller_.status().target_delay;
  controller_.return_live();
  if (!controller_.set_target(target,&error)) throw std::runtime_error(error);
 }
 (void)invalidate(request.epoch ? request.epoch : dispatcher_.reserve_epoch(epoch_)); desired_=Feed::Holding;
 last_output_=published_output_;audio_end_=published_audio_end_;video_tail_=published_video_tail_;
 waiting_programme_.clear();waiting_bytes_=0;
 scheduled_.clear();scheduled_bytes_=0;scheduled_headers_.reset();drain_ack_.reset();paced_programme_=programme_boundary_sent_=programme_boundary_delivered_=false;
 drain_required_=last_output_>=0;guard_active_=paced_holding_=drain_required_;drain_ticket_=holding_ticket_=programme_ticket_=0;
 if(drain_required_ && programme_headers_.aac_audio_specific_config!=holding_headers_.aac_audio_specific_config)
  throw std::runtime_error("TRANSITION_CODEC_UNSUPPORTED: AAC configurations must match");
 (void)transition_aac_duration(holding_headers_);
}
void TransitionCoordinator::programme(EncodedPacket packet) {
 if(packet.audio_drain)throw std::runtime_error("TRANSITION_DRAIN_INVALID: programme cannot supply independent silence");
 // An admitted action is not processed yet. Keep buffering so a retained delay
 // has no hole (Return Live/Dump clear it anyway); only publication waits.
 const bool stale=epoch_<dispatcher_.publication_epoch();
 if (!stale && packet.kind==PacketKind::Video && packet.keyframe)
  programme_key_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 controller_.ingest(std::move(packet));
 const auto state=controller_.status();
 if (state.state==DelayState::Error) throw std::runtime_error(state.error);
 if(stale)return;
 auto ready=controller_.take_ready_packets();
 if(paced_holding_) {
  if(waiting_programme_.empty() && std::none_of(ready.begin(),ready.end(),[](const auto &p){return p.kind==PacketKind::Video && p.keyframe;}))ready.clear();
  if(!ready.empty() && !programme_deadline_)programme_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  for(auto &p:ready) {
   waiting_bytes_+=p.payload.size();
   if(waiting_bytes_>16ULL*1024*1024 || waiting_programme_.size()>=4096 ||
      (!waiting_programme_.empty() && p.dts_us-waiting_programme_.front().dts_us>2000000))
    throw std::runtime_error("TRANSITION_GUARD_OVERFLOW: programme released before holding drain completed");
   waiting_programme_.push_back(std::move(p));
  }
  return;
 }
 release_ready(std::move(ready));
}
void TransitionCoordinator::release_ready(std::vector<EncodedPacket> ready) {
 if(!waiting_programme_.empty()) {
  waiting_programme_.insert(waiting_programme_.end(),std::make_move_iterator(ready.begin()),std::make_move_iterator(ready.end()));
  ready=std::move(waiting_programme_);waiting_programme_.clear();waiting_bytes_=0;
 }
 if (ready.empty()) return;
 if (desired_!=Feed::Programme) {
  // Never join a dependent frame from the retained programme prefix.
  auto key=std::find_if(ready.begin(),ready.end(),[](const auto &p){return p.kind==PacketKind::Video && p.keyframe;});
  if (key==ready.end()) {
   if (!programme_deadline_) programme_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   return;
  }
  ready.erase(ready.begin(),key);
  if(!invalidate())return;
  desired_=Feed::Programme;
 }
 publish(Feed::Programme,std::move(ready));
}
void TransitionCoordinator::holding(std::vector<EncodedPacket> packets) {
 if(epoch_<dispatcher_.publication_epoch())return;
 // Drain continuously off-air; do not retain/replay old holding frames.
 if (desired_==Feed::Holding && (waiting_programme_.empty() || !selected_)) publish(Feed::Holding,std::move(packets));
}
void TransitionCoordinator::check_deadline(std::chrono::steady_clock::time_point now) {
 tick_now_=now;
 if(epoch_<dispatcher_.publication_epoch())return;
 if(paced_holding_) {
  if(guard_active_ && now>=boundary_deadline_)throw std::runtime_error("TRANSITION_GUARD_TIMEOUT: scheduled boundary was not delivered");
  if(drain_ticket_ && dispatcher_.delivered(epoch_,drain_ticket_)) {
   if(!drain_ack_)drain_ack_=now;
   std::vector<EncodedPacket> due;
   while(!scheduled_.empty() && now>=*drain_ack_+Microseconds{scheduled_.front().dts_us-drain_time_}) {
    scheduled_bytes_-=scheduled_.front().payload.size();
    due.push_back(std::move(scheduled_.front()));scheduled_.pop_front();
   }
   if(!due.empty()) {
    // Bridge silence is not holding media; mark the guard on real holding audio.
    const bool has_audio=std::any_of(due.begin(),due.end(),[](const auto &p){return p.kind==PacketKind::Audio && !p.audio_drain;});
    record_publication(due,Feed::Holding);
    auto failures=dispatcher_.dispatch(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{epoch_,std::move(due),std::move(scheduled_headers_),++ticket_}));
    if(!holding_ticket_ && has_audio)holding_ticket_=ticket_;
    scheduled_headers_.reset();
    if(!failures.empty())throw std::runtime_error(failures.front().error);
   }
   if(holding_ticket_ && dispatcher_.delivered(epoch_,holding_ticket_))guard_active_=false;
   if(!guard_active_ && !waiting_programme_.empty() && scheduled_.empty() && dispatcher_.delivered(epoch_,ticket_)) {
    paced_holding_=false;
    release_ready({});
   }
  }
 }

 if(paced_programme_) {
  std::vector<EncodedPacket> due;
  while(!scheduled_.empty() && now>=resume_origin_+Microseconds{scheduled_.front().dts_us-resume_time_}) {
   scheduled_bytes_-=scheduled_.front().payload.size();
   if(scheduled_.front().kind==PacketKind::Audio && !scheduled_.front().audio_drain)programme_boundary_sent_=true;
   due.push_back(std::move(scheduled_.front()));scheduled_.pop_front();
  }
  if(!due.empty()) {
   record_publication(due,Feed::Programme);
   auto failures=dispatcher_.dispatch(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{epoch_,std::move(due),std::move(scheduled_headers_),++ticket_}));
   if(programme_boundary_sent_ && !programme_ticket_)programme_ticket_=ticket_;
   scheduled_headers_.reset();
   if(!failures.empty())throw std::runtime_error(failures.front().error);
  }
  if(programme_boundary_sent_ && dispatcher_.delivered(epoch_,programme_ticket_))programme_boundary_delivered_=true;
  if(!programme_boundary_delivered_ && now>=boundary_deadline_)throw std::runtime_error("TRANSITION_GUARD_TIMEOUT: programme presentation boundary unavailable");
  if(programme_boundary_delivered_) {
   // The switch is on air. Pacing past this point only adds lasting latency
   // and a jitter-sensitive queue limit, so release the rest and stop pacing.
   paced_programme_=false;
   std::vector<EncodedPacket> rest(std::make_move_iterator(scheduled_.begin()),std::make_move_iterator(scheduled_.end()));
   scheduled_.clear();scheduled_bytes_=0;
   if(!rest.empty()) {
    record_publication(rest,Feed::Programme);
    auto failures=dispatcher_.dispatch(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{epoch_,std::move(rest),std::move(scheduled_headers_),++ticket_}));
    scheduled_headers_.reset();
    if(!failures.empty())throw std::runtime_error(failures.front().error);
   }
  }
 }
 if ((!selected_ && now>=boundary_deadline_) || (programme_deadline_ && now>=*programme_deadline_))
  throw std::runtime_error("TRANSITION_BOUNDARY_TIMEOUT: fresh keyframe and matching audio unavailable");
 if (controller_.status().state==DelayState::BuildingDelay && now>=programme_key_deadline_)
  throw std::runtime_error("PROGRAMME_KEYFRAME_TIMEOUT: compressed programme cannot safely rebuild delay");
}
void TransitionCoordinator::record_publication(const std::vector<EncodedPacket> &packets,Feed feed) {
 // Scheduling is not publication: superseding an epoch must not inherit its
 // cancelled future timestamps. Already dispatched/entered I/O remains a tail.
 const auto duration=transition_aac_duration(feed==Feed::Programme ? programme_headers_ : holding_headers_);
 for(const auto &p:packets) {
  published_output_=p.dts_us;
  if(p.kind==PacketKind::Audio)published_audio_end_=p.pts_us+duration;
  else {published_feed_=feed;published_video_tail_=std::max(published_video_tail_,p.pts_us+(feed==Feed::Programme ? programme_frame_us_ : holding_frame_us_));}
 }
}
void TransitionCoordinator::publish(Feed feed,std::vector<EncodedPacket> packets) {
 if (packets.empty()) return;
 const auto duration=transition_aac_duration(feed==Feed::Programme ? programme_headers_ : holding_headers_);
 for(const auto &p:packets)validate_transition_packet_timing(p);
 std::optional<FlvCodecHeaders> headers;
 std::vector<EncodedPacket> bridge;
 if (!selected_) {
  for (auto &p:packets) {
   if (!cutoff_) {
    if (p.kind!=PacketKind::Video || !p.keyframe) continue;
    cutoff_=p.dts_us;
   }
   if (p.dts_us<*cutoff_) continue;
   boundary_bytes_+=p.payload.size();
   if (boundary_bytes_>16ULL*1024*1024 || boundary_.size()>=4096 || p.dts_us-*cutoff_>2000000)
    throw std::runtime_error("TRANSITION_BOUNDARY_OVERFLOW: matching audio/keyframe unavailable");
   boundary_.push_back(std::move(p));
  }
  if (!cutoff_ || std::none_of(boundary_.begin(),boundary_.end(),[](const auto &p){return p.kind==PacketKind::Audio;})) return;
  const auto first_pts=boundary_.front().pts_us;
  if(feed==Feed::Programme && last_output_>=0 &&
     std::none_of(boundary_.begin(),boundary_.end(),[&](const auto &p){return p.kind==PacketKind::Audio && p.pts_us>=first_pts;})) return;
  packets=std::move(boundary_); boundary_.clear(); boundary_bytes_=0;
  if (last_output_>std::numeric_limits<std::int64_t>::max()-1000)
   throw std::runtime_error("TRANSITION_TIMESTAMP_OVERFLOW");
  bool drained=false;
  if(feed==Feed::Holding && drain_required_) {
   drained=true;
   const auto silent=std::find_if(packets.begin(),packets.end(),[](const auto &p){return p.kind==PacketKind::Audio;});
   silent_aac_=silent->payload;
   const auto t=ceil_ms(std::max(last_output_+1000,audio_end_));
   const auto drain_headers=FlvCodecHeaders{published_feed_==Feed::Programme ? programme_headers_.avc_decoder_configuration : holding_headers_.avc_decoder_configuration,holding_headers_.aac_audio_specific_config};
   auto failures=dispatcher_.dispatch(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{
    epoch_,{{PacketKind::Audio,silent_aac_,t,t,false,true}},drain_headers,++ticket_}));
   if(!failures.empty())throw std::runtime_error(failures.front().error);
   // Wait the entire previous presentation tail plus the LC drain interval,
   // not merely the difference between newly assigned future timestamps.
   drain_time_=last_output_;
   last_output_=published_output_=t;audio_end_=published_audio_end_=t+duration;
   drain_required_=false;
   drain_ticket_=ticket_;
   if(dispatcher_.delivered(epoch_,drain_ticket_))drain_ack_=std::chrono::steady_clock::now();
  }
  const auto next=ceil_ms(std::max({last_output_+1000,audio_end_,video_tail_+1000}));
  if ((*cutoff_<0 && next>std::numeric_limits<int64_t>::max()+*cutoff_) ||
      (*cutoff_>0 && next<std::numeric_limits<int64_t>::min()+*cutoff_))
   throw std::runtime_error("TRANSITION_TIMESTAMP_OVERFLOW");
  offset_=next-*cutoff_;
  if(feed==Feed::Programme && last_output_>=0 && !silent_aac_.empty()) {
   std::erase_if(packets,[&](const auto &p){return p.kind==PacketKind::Audio && p.pts_us<first_pts;});
   const auto first_audio=std::find_if(packets.begin(),packets.end(),[](const auto &p){return p.kind==PacketKind::Audio;});
   const auto audio_start=first_audio->pts_us+offset_;
   if(audio_start-audio_end_>250000 || silent_aac_.empty())
    throw std::runtime_error("TRANSITION_TIMING_UNSUPPORTED: resume bridge cannot be established");
   for(auto t=ceil_ms(audio_end_);t+duration<=audio_start;t=ceil_ms(t+duration))
    bridge.push_back({PacketKind::Audio,silent_aac_,t,t,false,true});
  }
  if(drained) {
   // The holding picture starts after the programme's video tail, which can be
   // well past the drain frame. Fill that span with silence as the resume does.
   std::optional<std::int64_t> first_audio;
   for(const auto &p:packets)if(p.kind==PacketKind::Audio && (!first_audio || p.pts_us<*first_audio))first_audio=p.pts_us;
   const auto audio_start=*first_audio+offset_;
   for(auto t=ceil_ms(audio_end_);t+duration<=audio_start;t=ceil_ms(t+duration))
    bridge.push_back({PacketKind::Audio,silent_aac_,t,t,false,true});
  }
  selected_=feed;
  headers=feed==Feed::Programme ? programme_headers_ : holding_headers_;
 }
 std::vector<EncodedPacket> output=std::move(bridge);
 for (auto &p:packets) {
  if (p.dts_us<*cutoff_) continue;
  // Epoch/source timestamps are bounded by libobs's session clock.
  if ((offset_>0 && (p.dts_us>std::numeric_limits<int64_t>::max()-offset_ || p.pts_us>std::numeric_limits<int64_t>::max()-offset_)) ||
      (offset_<0 && (p.dts_us<std::numeric_limits<int64_t>::min()-offset_ || p.pts_us<std::numeric_limits<int64_t>::min()-offset_)))
   throw std::runtime_error("TRANSITION_TIMESTAMP_OVERFLOW");
  p.dts_us+=offset_; p.pts_us+=offset_;
  output.push_back(std::move(p));
 }
 if (output.empty()) return;
 std::stable_sort(output.begin(),output.end(),[](const auto &a,const auto &b){return a.dts_us<b.dts_us;});
 for(const auto &p:output) {
  if(p.dts_us<last_output_)throw std::runtime_error("TRANSITION_DTS_REGRESSION");
  last_output_=p.dts_us;
  if(p.kind==PacketKind::Audio) {
   audio_end_=p.pts_us+duration;
   if(feed==Feed::Holding)silent_aac_=p.payload;
  } else video_tail_=std::max(video_tail_,p.pts_us+(feed==Feed::Programme ? programme_frame_us_ : holding_frame_us_));
 }
 if(feed==Feed::Programme && headers && !silent_aac_.empty() && drain_ticket_) {
  paced_programme_=true;programme_boundary_sent_=false;
  resume_origin_=tick_now_;resume_time_=output.front().dts_us;
 }
 if((feed==Feed::Holding && paced_holding_) || (feed==Feed::Programme && paced_programme_)) {
  if(headers)scheduled_headers_=std::move(headers);
  for(auto &p:output) {
   scheduled_bytes_+=p.payload.size();
   if(scheduled_bytes_>16ULL*1024*1024 || scheduled_.size()>=4096 || (!scheduled_.empty() && p.dts_us-scheduled_.front().dts_us>2000000))
    throw std::runtime_error("TRANSITION_GUARD_OVERFLOW");
   scheduled_.push_back(std::move(p));
  }
  return;
 }
 record_publication(output,feed);
 auto failures=dispatcher_.dispatch(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{epoch_,std::move(output),std::move(headers),++ticket_}));
 if (!failures.empty()) throw std::runtime_error(failures.front().error);
}
} // namespace active_delay
