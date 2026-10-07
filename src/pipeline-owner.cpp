#include "pipeline-owner.hpp"
#include "transition-codec.hpp"
#include <algorithm>
#include <stdexcept>
namespace active_delay {
PipelineOwner::PipelineOwner(DelayController &c,ReleasedPacketDispatcher &d,std::uint64_t e,HoldingFactory h,ConsumerStart s,Finish f,
 Microseconds target, ConsumerReady ready)
 :controller_(c),dispatcher_(d),epoch_(e),holding_factory_(std::move(h)),consumer_start_(std::move(s)),finish_(std::move(f)),
 prebuffer_target_(target),consumer_ready_(std::move(ready)) {}
PipelineOwner::~PipelineOwner(){stop();}
void PipelineOwner::clear_capture_locked(){
 packets_.clear();packet_bytes_=0;video_ingress_.clear();audio_ingress_.clear();ingress_bytes_=0;
 video_input_.reset();audio_input_.reset();
}
void PipelineOwner::start(){
 std::scoped_lock lifecycle(lifecycle_mutex_);
 std::scoped_lock lock(mutex_);
 if(worker_.joinable()) throw std::logic_error("Pipeline owner already started");
 if(prebuffer_target_!=Microseconds{}) {
  std::string error;
  if(!controller_.arm(prebuffer_target_,error)) throw std::runtime_error(error);
 }
 status_.stopped=false; stopping_=false; status_.phase=BroadcastPhase::Arming;
 last_video_=last_audio_=last_key_=std::chrono::steady_clock::now();
 worker_=std::thread(&PipelineOwner::run,this);
}
void PipelineOwner::headers(FlvCodecHeaders h){
 if(h.avc_decoder_configuration.empty() || h.aac_audio_specific_config.empty()){
  failure("PROGRAMME_HEADERS_INVALID: H.264 and AAC configuration required");return;
 }
 std::scoped_lock lock(mutex_);
 if(stopping_)return;
 if(headers_ && (headers_->avc_decoder_configuration!=h.avc_decoder_configuration || headers_->aac_audio_specific_config!=h.aac_audio_specific_config)){
  failure("PROGRAMME_CODEC_CHANGED: encoder configuration changed during capture");return;
 }
 if(!headers_)headers_=std::move(h);wake_.notify_one();
}
bool PipelineOwner::start_broadcast(std::string &error){
 std::scoped_lock lock(mutex_);
 std::scoped_lock failure_lock(failure_->mutex);
 const auto now=std::chrono::steady_clock::now();
 // The broadcast starts live, so it needs fresh programme, not a full buffer.
 if(stopping_ || !failure_->error.empty() || (status_.phase!=BroadcastPhase::Ready && status_.phase!=BroadcastPhase::Filling) || broadcast_requested_ ||
    now-last_video_>std::chrono::seconds(2) || now-last_audio_>std::chrono::seconds(2) || now-last_key_>std::chrono::seconds(5)) {
  error="PREBUFFER_NOT_READY: arm and wait for fresh programme video and audio";return false;
 }
 broadcast_requested_=true;status_.phase=BroadcastPhase::Connecting;wake_.notify_one();return true;
}
void PipelineOwner::failure(std::string error){
 std::scoped_lock lock(failure_->mutex);
 if(failure_->error.empty()) {
  failure_->error=std::move(error);
  // Retire dequeued/scheduled media now, not on the next owner tick. Only
  // cancellation runs here; the owner remains responsible for all joins.
  const auto epoch=dispatcher_.reserve_epoch(epoch_);
  (void)dispatcher_.dispatch_discontinuity({epoch,"Capture failure publication cutoff"});
 }
 wake_.notify_one();
}
void PipelineOwner::enqueue(EncodedPacket packet){
	(void)enqueue(std::move(packet),capture_token());
}
std::uint64_t PipelineOwner::capture_token() const {std::scoped_lock lock(mutex_);return capture_epoch_;}
bool PipelineOwner::enqueue(EncodedPacket packet,std::uint64_t token){
 std::scoped_lock lock(mutex_);
 if(stopping_ || token!=capture_epoch_) return false;
 {std::scoped_lock failure_lock(failure_->mutex);if(!failure_->error.empty())return false;}
 try{validate_transition_packet_timing(packet);}catch(const std::exception &e){failure(e.what());return false;}
 if(packet_bytes_+packet.payload.size()>16ULL*1024*1024 || packets_.size()>=4096){
  failure("PROGRAMME_INGRESS_OVERFLOW: owner compressed queue reached capacity"); return false;
 }
 const auto now=std::chrono::steady_clock::now();
 if(packet.kind==PacketKind::Video){last_video_=now;if(packet.keyframe)last_key_=now;}else last_audio_=now;
 packet_bytes_+=packet.payload.size(); packets_.push_back(std::move(packet));wake_.notify_one();
 return true;
}
bool PipelineOwner::enqueue_uninterleaved(EncodedPacket packet,std::uint64_t token){
 std::scoped_lock lock(mutex_);
 if(stopping_ || token!=capture_epoch_)return false;
 {std::scoped_lock failure_lock(failure_->mutex);if(!failure_->error.empty())return false;}
 try{validate_transition_packet_timing(packet);}catch(const std::exception &e){failure(e.what());return false;}
 auto &last=packet.kind==PacketKind::Video ? video_input_ : audio_input_;
 if(packet.dts_us<0 || (last && packet.dts_us<=*last)){
  failure("PROGRAMME_TIMESTAMP_INVALID: native encoder clock did not advance");return false;
 }
 if(packet.payload.empty() || packet.payload.size()>16ULL*1024*1024 ||
    ingress_bytes_+packet_bytes_>16ULL*1024*1024-packet.payload.size() ||
    video_ingress_.size()+audio_ingress_.size()+packets_.size()>=4096){
  failure("PROGRAMME_INGRESS_OVERFLOW: bounded native capture queue reached capacity");return false;
 }
 last=packet.dts_us;
 auto &queue=packet.kind==PacketKind::Video ? video_ingress_ : audio_ingress_;
 if(!queue.empty() && packet.dts_us-queue.front().dts_us>2000000){
  failure("PROGRAMME_INGRESS_STALLED: peer encoder exceeded two-second interleave budget");return false;
 }
 const auto now=std::chrono::steady_clock::now();
 if(packet.kind==PacketKind::Video){last_video_=now;if(packet.keyframe)last_key_=now;}else last_audio_=now;
 ingress_bytes_+=packet.payload.size();queue.push_back(std::move(packet));
 if(video_input_ && audio_input_){
  const auto watermark=std::min(*video_input_,*audio_input_);
  // Strictly below watermark: equal-clock video must precede matching audio
  // even if their callbacks arrive on different encoder threads.
  while(!video_ingress_.empty() || !audio_ingress_.empty()){
   auto &next=audio_ingress_.empty() || (!video_ingress_.empty() && video_ingress_.front().dts_us<=audio_ingress_.front().dts_us) ? video_ingress_ : audio_ingress_;
   if(next.front().dts_us>=watermark)break;
   const auto bytes=next.front().payload.size();ingress_bytes_-=bytes;packet_bytes_+=bytes;
   packets_.push_back(std::move(next.front()));next.pop_front();
  }
 }
 wake_.notify_one();return true;
}
bool PipelineOwner::request(TransitionRequest request,std::string &error){
 // Admission and invalidation share the same lock, including both native
 // encoder queues; a callback's old token can never repopulate dumped media.
 std::scoped_lock lock(mutex_);
 {std::scoped_lock failure_lock(failure_->mutex);if(!failure_->error.empty()){error=failure_->error;return false;}}
 if(stopping_ || !status_.ready || !status_.holding_ready ||
    (prebuffer_target_!=Microseconds{} && status_.phase!=BroadcastPhase::Broadcasting)){error="HOLDING_NOT_READY: transition unavailable until delayed programme is broadcasting";return false;}
 if(requests_.size()>=64){error="TRANSITION_QUEUE_FULL: too many pending actions";return false;}
 const bool rewind_mode=controller_.rewind_mode();
 if(rewind_mode){
  const auto state=controller_.status().state;
  if(request.action==TransitionAction::SetDelay && (state!=DelayState::Live || !controller_.prebuffer_ready())){
   error=state==DelayState::Live ? "DELAY_NOT_READY: the buffer is still filling" : "DELAY_ALREADY_ACTIVE: return live first";return false;}
  if(request.action==TransitionAction::ReturnLive && state==DelayState::Live){error="ALREADY_LIVE: no delay to leave";return false;}
  if(request.action==TransitionAction::EmergencyDump && state==DelayState::Live){error="DUMP_UNAVAILABLE: nothing delayed to dump while live";return false;}
 }
 // Preserve accepted action order. Dump retains preceding configured target.
 // Fence the dispatcher's already-dequeued owner batch AND network queues now,
 // rather than waiting for the worker's next request-processing iteration.
 try {
  request.epoch=dispatcher_.reserve_epoch(epoch_);
  const auto failures=dispatcher_.dispatch_discontinuity({request.epoch,"Accepted action publication cutoff"});
  if(!failures.empty()){error=failures.front().error;failure(error);return false;}
 }catch(const std::exception &exception){error=exception.what();failure(error);return false;}
 requests_.push_back(request);
 status_.transition_pending=true;
 // A delay change keeps the controller buffer, so in-flight capture must reach
 // it in order; so does Return Live in rewind mode, which keeps the history.
 // Only actions that discard the buffer revoke captured media.
 const bool keeps_buffer=request.action==TransitionAction::SetDelay || (rewind_mode && request.action==TransitionAction::ReturnLive);
 if(!keeps_buffer){++capture_epoch_;clear_capture_locked();}
 wake_.notify_one();return true;
}
void PipelineOwner::request_stop() noexcept {
 std::scoped_lock lock(mutex_);
 if(!stopping_) {
  // Revoke already-dequeued and timed work at stop admission as well.
  try {const auto epoch=dispatcher_.reserve_epoch(epoch_);(void)dispatcher_.dispatch_discontinuity({epoch,"Stop publication cutoff"});}
  catch(...) {} // stop/teardown still owns and quiesces every consumer
 }
 stopping_=true;status_.ready=false;if(!status_.stopped)status_.phase=BroadcastPhase::Stopping;
 ++capture_epoch_;clear_capture_locked();requests_.clear();wake_.notify_all();
}
void PipelineOwner::stop() noexcept {request_stop();std::scoped_lock lifecycle(lifecycle_mutex_);if(worker_.joinable()) worker_.join();}
PipelineStatus PipelineOwner::status() const {
 std::scoped_lock lock(mutex_);
 auto result=status_;
 std::scoped_lock failure_lock(failure_->mutex);
 if(!failure_->error.empty()){result.ready=false;result.holding_ready=false;result.error=failure_->error;result.phase=BroadcastPhase::Failed;}
 if(result.phase==BroadcastPhase::Ready &&
    (std::chrono::steady_clock::now()-last_video_>std::chrono::seconds(2) || std::chrono::steady_clock::now()-last_audio_>std::chrono::seconds(2)))
  result.phase=BroadcastPhase::Filling;
 return result;
}
std::shared_ptr<ReleasedPacketConsumer> PipelineOwner::consumer_snapshot() const {std::scoped_lock lock(mutex_);return consumer_;}
void PipelineOwner::run() noexcept {
 std::unique_ptr<HoldingCapture> holding;
 std::shared_ptr<ReleasedPacketConsumer> consumer;
 ReleasedPacketDispatcher::ConsumerId id=0;
 std::string error;
 try {
  holding=holding_factory_();
  if(holding)holding->set_failure_callback([this](const std::string &error){failure(error);});
  if(!holding || !holding->prepare(error) || !holding->start(error)) throw std::runtime_error(error.empty()?"HOLDING_START_FAILED":error);
  std::unique_ptr<TransitionCoordinator> coordinator;
  bool codecs_checked=false;
  const auto header_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  for(;;){
   std::optional<FlvCodecHeaders> headers;
   std::deque<TransitionRequest> requests;
   std::vector<EncodedPacket> packets;
   {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock,std::chrono::milliseconds(10));
    if(stopping_) break;
    headers=headers_;requests.swap(requests_);packets.swap(packets_);packet_bytes_=0;
   }
   {std::scoped_lock lock(failure_->mutex);if(!failure_->error.empty()) throw std::runtime_error(failure_->error);}
   const auto holding_status=holding->status();
   if(holding_status.state==HoldingState::Failed) throw std::runtime_error(holding_status.error);
   auto auxiliary=holding->take_packets(); // ALWAYS drain off-air as well.
   if(!codecs_checked && headers && holding_status.state==HoldingState::Ready) {
    const auto auxiliary_headers=holding->headers();
    (void)transition_aac_duration(*headers);(void)transition_aac_duration(auxiliary_headers);
    (void)transition_video_duration(*headers);(void)transition_video_duration(auxiliary_headers);
    if(headers->aac_audio_specific_config!=auxiliary_headers.aac_audio_specific_config)unsupported_transition_codec();
    codecs_checked=true;
   }
   if(prebuffer_target_!=Microseconds{}){
    std::scoped_lock lock(mutex_);
    const auto now=std::chrono::steady_clock::now();
    if(now-last_video_>std::chrono::seconds(2) || now-last_audio_>std::chrono::seconds(2))
     throw std::runtime_error("PREBUFFER_CAPTURE_STALLED: programme video or audio stopped");
   }
   if(prebuffer_target_!=Microseconds{} && !coordinator){
    const auto now=std::chrono::steady_clock::now();
    for(auto &packet:packets)controller_.ingest(std::move(packet));
    packets.clear();
    const auto state=controller_.status();
    if(state.state==DelayState::Error)throw std::runtime_error(state.error);
    bool requested;
    {std::scoped_lock lock(mutex_);
     if(now-last_key_>std::chrono::seconds(5))throw std::runtime_error("PREBUFFER_KEYFRAME_TIMEOUT: no fresh programme keyframe");
     requested=broadcast_requested_;
     status_.holding_ready=holding_status.state==HoldingState::Ready;
     if(!requested)status_.phase=headers && status_.holding_ready && controller_.prebuffer_ready() ? BroadcastPhase::Ready : BroadcastPhase::Filling;
    }
    if(!headers && now>header_deadline)throw std::runtime_error("PROGRAMME_HEADERS_TIMEOUT");
    if(!requested)continue;
    if(!headers)throw std::runtime_error("PROGRAMME_HEADERS_TIMEOUT");
   }
   if(!consumer && headers){
    {std::scoped_lock lock(mutex_);if(stopping_)break;}
    auto fail=failure_;
    consumer=consumer_start_(*headers,[fail](const std::string &e){std::scoped_lock lock(fail->mutex);if(fail->error.empty()) fail->error=e;},error);
    if(!consumer) throw std::runtime_error(error.empty()?"NETWORK_START_FAILED":error);
    {std::scoped_lock lock(failure_->mutex);if(!failure_->error.empty()) throw std::runtime_error(failure_->error);}
    id=dispatcher_.add_consumer(consumer);

    std::scoped_lock lock(mutex_);
    std::scoped_lock failure_lock(failure_->mutex);
    if(!failure_->error.empty()) throw std::runtime_error(failure_->error);
    consumer_=consumer;
   }
   if(!coordinator && consumer && headers){
    {std::scoped_lock lock(mutex_);if(stopping_)break;}
    if(prebuffer_target_!=Microseconds{}){
     if(consumer_ready_ && !consumer_ready_(consumer))continue;
     if(!controller_.start_live())throw std::runtime_error("PREBUFFER_READINESS_LOST: capture cannot start live");
    }
    std::scoped_lock lock(mutex_);
    std::scoped_lock failure_lock(failure_->mutex);
    if(stopping_)break;
    if(!failure_->error.empty())throw std::runtime_error(failure_->error);
    coordinator=std::make_unique<TransitionCoordinator>(controller_,dispatcher_,epoch_,*headers);
    if(!stopping_) {status_.ready=true;status_.phase=BroadcastPhase::Connecting;}
   }
   if(!coordinator){if(std::chrono::steady_clock::now()>header_deadline) throw std::runtime_error("PROGRAMME_HEADERS_TIMEOUT");continue;}
   if(holding_status.state==HoldingState::Ready){
    coordinator->holding_ready(holding->headers());
    std::scoped_lock lock(mutex_);status_.holding_ready=true;
   }
   for(const auto &request:requests) coordinator->request(request);
   coordinator->holding(std::move(auxiliary));
   for(auto &packet:packets) coordinator->programme(std::move(packet));
   coordinator->check_deadline();
   {
    const bool pending=!coordinator->programme_selected() || !consumer->boundary_delivered(coordinator->epoch());
    std::scoped_lock lock(mutex_);status_.transition_pending=pending;
    if(!pending && !stopping_)status_.phase=BroadcastPhase::Broadcasting;
   }
  }
 }catch(const std::exception &e){error=e.what();}catch(...){error="PIPELINE_OWNER_FAILED";}
 // Owner-only joins; worker callbacks latch failures and never tear down.
 {std::scoped_lock lock(mutex_);stopping_=true;}
 if(id) dispatcher_.remove_consumer(id);
 else if(consumer) consumer->stop();
 if(holding) holding->stop();
 controller_.return_live();
 {std::scoped_lock lock(mutex_);status_.ready=false;status_.holding_ready=false;status_.stopped=true;status_.error=error;status_.phase=error.empty()?BroadcastPhase::Stopped:BroadcastPhase::Failed;stopping_=true;clear_capture_locked();}
 if(finish_) {try{finish_(error);}catch(...){}}
}
} // namespace active_delay
