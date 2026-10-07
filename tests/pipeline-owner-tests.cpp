#include "pipeline-owner.hpp"
#include "multi-target-sender.hpp"
#include "transition-test-headers.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace active_delay;
using namespace std::chrono_literals;
namespace {
void require(bool v,const char *m) { if(!v) throw std::runtime_error(m); }
struct Backend : HoldingCaptureBackend {
 HoldingFeed *feed=nullptr;
 bool prepare(HoldingFeed &f,std::string &) override { feed=&f; return true; }
 bool start(std::string &) override { return true; }
 void stop() noexcept override {}
};
struct Consumer : ReleasedPacketConsumer {
 void consume(const std::shared_ptr<const ReleasedPacketBatch>&) override {}
 void discontinuity(const PacketDiscontinuity&) override {}
 void stop() noexcept override {}
};
void failure_before_install_cannot_publish_ready() {
 DelayController controller; ReleasedPacketDispatcher dispatcher;
 PipelineOwner owner(controller,dispatcher,1,
  []{return std::make_unique<HoldingCapture>(std::make_unique<Backend>());},
  [](FlvCodecHeaders,SenderErrorCallback fail,std::string &) {
   fail("injected synchronous startup failure");
   return std::make_shared<Consumer>();
  },{});
 owner.start(); owner.headers(programme_test_headers());
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(owner.status().error.empty() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
 owner.stop();
 require(!owner.status().ready,"failure-before-install must never publish pipeline ready");
 require(dispatcher.consumer_count()==0,"owner must remove raced consumer safely");
 require(owner.status().error=="injected synchronous startup failure","owner must retain failure");
}

void startup_failure_is_visible_before_factory_returns(){
 struct Gate{std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;};
 auto gate=std::make_shared<Gate>();DelayController controller;ReleasedPacketDispatcher dispatcher;
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<Backend>());},
  [gate](FlvCodecHeaders,SenderErrorCallback fail,std::string &){
   fail("startup callback latched before factory returns");
   std::unique_lock lock(gate->mutex);gate->entered=true;gate->changed.notify_all();gate->changed.wait(lock,[&]{return gate->released;});
   return std::make_shared<Consumer>();
  },{});
 owner.start();owner.headers(programme_test_headers());
 bool entered;
 {std::unique_lock lock(gate->mutex);entered=gate->changed.wait_for(lock,2s,[&]{return gate->entered;});}
 const auto status=owner.status();std::string error;const bool accepted=owner.request({TransitionAction::ReturnLive,{}},error);
 {std::scoped_lock lock(gate->mutex);gate->released=true;gate->changed.notify_all();}
 owner.stop();
 require(entered && !accepted && !status.ready && status.error=="startup callback latched before factory returns",
  "latched asynchronous startup failure must be visible and fail closed before consumer installation returns");
}

struct ReadyBackend : HoldingCaptureBackend {
 HoldingFeed *feed=nullptr;
 bool prepare(HoldingFeed &f,std::string &) override{feed=&f;return true;}
 bool start(std::string &) override{
  feed->set_headers(holding_test_headers());
  feed->ingest({PacketKind::Video,{0,0,0,2,0x65,0x48},0,0,true});
  feed->ingest({PacketKind::Audio,{0},1000,1000,false});
  feed->ingest({PacketKind::Video,{0,0,0,2,0x41,0x48},2000,2000,false});
  feed->ingest({PacketKind::Audio,{0},3000,3000,false});return true;
 }
 void stop() noexcept override{}
};
struct BlockedConsumer : Consumer {
 std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;int calls=0;
 void consume(const std::shared_ptr<const ReleasedPacketBatch>&) override{
  std::unique_lock lock(mutex);++calls;entered=true;changed.notify_all();changed.wait(lock,[&]{return released;});
 }
};
void overlapping_actions_are_serialized_not_lost(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<BlockedConsumer>();
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [sink](FlvCodecHeaders,SenderErrorCallback,std::string &){return sink;},{});
 owner.start();owner.headers(programme_test_headers());
 auto deadline=std::chrono::steady_clock::now()+2s;
 while(!owner.status().holding_ready && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 owner.enqueue({PacketKind::Video,{0,0,0,2,0x65,0x50},10000,10000,true});
 owner.enqueue({PacketKind::Audio,{0x77},11000,11000,false});
 bool entered;
 {std::unique_lock lock(sink->mutex);entered=sink->changed.wait_for(lock,2s,[&]{return sink->entered;});}
 std::string error;
 const auto old_token=owner.capture_token();
 const bool accepted=owner.request({TransitionAction::SetDelay,Microseconds{100000}},error) && owner.request({TransitionAction::EmergencyDump,{}},error);
 const bool stale_video=owner.enqueue({PacketKind::Video,{0,0,0,2,0x65,0xee},20000,20000,true},old_token);
 const bool stale_audio=owner.enqueue({PacketKind::Audio,{0xee},21000,21000,false},old_token);
 {std::scoped_lock lock(sink->mutex);sink->released=true;sink->changed.notify_all();}
 deadline=std::chrono::steady_clock::now()+200ms;
 while(controller.status().target_delay!=Microseconds{100000} && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 const auto target=controller.status().target_delay;
 const auto start=std::chrono::steady_clock::now();owner.stop();const auto elapsed=std::chrono::steady_clock::now()-start;
 require(entered&&accepted,"controlled overlapping requests fixture");
 require(!stale_video && !stale_audio,"encoder callback token captured before dump must be rejected at racing admission");
 require(target==Microseconds{100000},"Emergency Dump must retain preceding accepted delay request, not silently lose it");
 require(elapsed<5s && dispatcher.consumer_count()==0,"stop pending transition must quiesce owned consumer within fault-test budget");
}

void delay_change_keeps_in_flight_programme_capture(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Consumer>();
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [sink](FlvCodecHeaders,SenderErrorCallback,std::string &){return sink;},{});
 owner.start();owner.headers(programme_test_headers());
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(!(owner.status().ready && owner.status().holding_ready) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 const auto token=owner.capture_token();
 std::string error;
 const bool accepted=owner.request({TransitionAction::SetDelay,Microseconds{100000}},error);
 const bool kept_token=owner.capture_token()==token;
 const bool in_flight=owner.enqueue({PacketKind::Video,{0,0,0,2,0x65,0x50},10000,10000,true},token);
 owner.stop();
 require(accepted,"delay change fixture must be admitted");
 require(kept_token && in_flight,"a delay change keeps the buffer, so programme captured in flight must not be discarded");
}

void stale_packets_behind_prepare_cannot_advertise_ready(){
 struct SlowBackend : ReadyBackend { bool prepare(HoldingFeed &f,std::string &e) override { std::this_thread::sleep_for(2200ms);return ReadyBackend::prepare(f,e); } };
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<SlowBackend>());},
  [](FlvCodecHeaders,SenderErrorCallback,std::string &){return std::make_shared<Consumer>();},{},100ms);
 owner.start();owner.headers(programme_test_headers());
 for(int i=0;i<4;++i){owner.enqueue({PacketKind::Video,{1},i*100000LL,i*100000LL,true});owner.enqueue({PacketKind::Audio,{2},i*100000LL,i*100000LL,false});}
 std::this_thread::sleep_for(2300ms);
 const auto status=owner.status();owner.stop();
 require(status.phase==BroadcastPhase::Failed && status.error.find("STALLED")!=std::string::npos,
  "queued old packets behind slow native preparation must not refresh readiness");
}
void prebuffer_stall_failure_and_pending_stop_are_off_air(){
 for(int scenario=0;scenario<4;++scenario){
  DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
  PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
   [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},100ms,
   [](const auto &){return false;});
  owner.start();owner.headers(programme_test_headers());
  std::string error;
  if(scenario==0){owner.stop();require(factories==0 && !owner.start_broadcast(error),"disarm before first media must be off-air and reject Start");continue;}
  for(int i=0;i<4;++i){owner.enqueue({PacketKind::Video,{1},i*100000LL,i*100000LL,true});owner.enqueue({PacketKind::Audio,{2},i*100000LL,i*100000LL,false});}
  const auto ready_deadline=std::chrono::steady_clock::now()+1s;
  while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<ready_deadline)std::this_thread::sleep_for(1ms);
  require(owner.status().phase==BroadcastPhase::Ready,"failure fixture must first be genuinely Ready");
  if(scenario==1){
   std::this_thread::sleep_for(2100ms);
   require(owner.status().phase==BroadcastPhase::Failed && !owner.start_broadcast(error) && factories==0,"stalled capture must revoke Ready without opening network");
  }else if(scenario==2){
   owner.failure("PROGRAMME_ENCODER_STOPPED");
   require(owner.status().phase==BroadcastPhase::Failed && !owner.start_broadcast(error) && factories==0,"encoder failure must synchronously revoke Ready");
  }else{
   require(owner.start_broadcast(error),"pending connection fixture Start");
   const auto deadline=std::chrono::steady_clock::now()+1s;
   while(factories==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
   require(factories==1 && owner.status().phase==BroadcastPhase::Connecting,"pending connection must remain truthful");
  }
  const auto stop=std::chrono::steady_clock::now();owner.stop();
  require(std::chrono::steady_clock::now()-stop<500ms && controller.status().buffered_bytes==0 && dispatcher.consumer_count()==0,"pending/fault disarm must quiesce and discard history");
 }
}
void malformed_headers_never_make_ready(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},100ms);
 owner.start();owner.headers({{}, {17,144}});
 for(int i=0;i<4;++i){owner.enqueue({PacketKind::Video,{1},i*100000LL,i*100000LL,true});owner.enqueue({PacketKind::Audio,{2},i*100000LL,i*100000LL,false});}
 std::this_thread::sleep_for(50ms);
 require(owner.status().phase!=BroadcastPhase::Ready && factories==0,"empty AVC configuration must never advertise Ready");
 owner.stop();
}
void independent_programme_callbacks_are_bounded_and_ordered(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},200ms);
 owner.start();owner.headers(programme_test_headers());
 const auto token=owner.capture_token();
 for(int i=0;i<50;++i) {
  require(owner.enqueue_uninterleaved({PacketKind::Video,{0x50},i*10000LL,i*10000LL,i%10==0},token),"video admission");
  // Audio arrives behind the newest video, as independent native encoders do.
  if(i)require(owner.enqueue_uninterleaved({PacketKind::Audio,{0x77},(i-1)*10000LL,(i-1)*10000LL,false},token),"audio admission");
 }
 auto deadline=std::chrono::steady_clock::now()+1s;
 while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 require(owner.status().phase==BroadcastPhase::Ready && factories==0,"independent callbacks must be watermarked, not fail on cross-stream arrival order");
 owner.stop();require(!owner.enqueue_uninterleaved({PacketKind::Audio,{1},500000,500000,false},token),"stopped capture rejects late callback");
}
void prebuffer_is_off_air_until_explicit_ready_start(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 std::atomic_int factories=0,batches=0;std::atomic_bool connected=false;
 struct Sink : Consumer { std::atomic_int &count; explicit Sink(std::atomic_int &c):count(c){} void consume(const std::shared_ptr<const ReleasedPacketBatch> &b) override {
  if(count++==0) require(!b->packets.empty() && b->packets.front().keyframe && b->packets.front().payload.back()==0x50,"first broadcast must be delayed programme keyframe, never holding");
 }};
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Sink>(batches);},{},2s,
  [&](const auto &){return connected.load();});
 owner.start();owner.headers(programme_test_headers());
 std::string error;require(!owner.start_broadcast(error),"early Start must reject rather than latch future publication");
 auto feed=[&](int i){owner.enqueue({PacketKind::Video,{0,0,0,2,0x65,0x50},i*100000LL,i*100000LL,i%10==0});owner.enqueue({PacketKind::Audio,{0x77},i*100000LL,i*100000LL,false});};
 for(int i=0;i<=30;++i)feed(i);
 auto deadline=std::chrono::steady_clock::now()+2s;
 while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 require(owner.status().phase==BroadcastPhase::Ready && factories==0 && batches==0,"Ready must be off-air with no consumer creation");
 require(owner.start_broadcast(error) && !owner.start_broadcast(error),"Start accepted exactly once");
 for(int i=31;i<=60;++i)feed(i);
 std::this_thread::sleep_for(40ms);
 require(factories==1 && batches==0 && owner.status().phase==BroadcastPhase::Connecting,"connecting must not consume retained keyframe");
 connected=true;std::this_thread::sleep_for(30ms);
 require(!owner.request({TransitionAction::ReturnLive,{}},error),"connecting startup must not allow Return Live to replace the initial delayed programme boundary");
 for(int i=61;i<=65;++i)feed(i);
 deadline=std::chrono::steady_clock::now()+1s;
 while(batches==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 require(batches>0 && controller.status().target_delay==2s,"connected start preserves target and sends retained programme");
 std::this_thread::sleep_for(2100ms);
 require(owner.status().phase==BroadcastPhase::Failed && owner.status().error.find("STALLED")!=std::string::npos,
  "broadcast capture stall must fail rather than remain falsely Broadcasting");
 owner.stop();require(dispatcher.consumer_count()==0 && owner.status().stopped,"stop must quiesce consumer");
}
void new_coordinator_cannot_reuse_a_retired_publication_epoch(){
 struct Counter : Consumer {int calls=0;void consume(const std::shared_ptr<const ReleasedPacketBatch>&)override{++calls;}};
 DelayController first,second;ReleasedPacketDispatcher dispatcher;auto sink=std::make_shared<Counter>();dispatcher.add_consumer(sink);
 TransitionCoordinator previous(first,dispatcher,1,programme_test_headers());
 previous.holding_ready(holding_test_headers());
 previous.request({TransitionAction::ReturnLive,{}});
 TransitionCoordinator restarted(second,dispatcher,1,programme_test_headers());
 restarted.programme({PacketKind::Video,{0,0,0,2,0x65,0x50},10000,10000,true});
 restarted.programme({PacketKind::Audio,{0x77},11000,11000,false});
 require(sink->calls==1 && restarted.epoch()>previous.epoch(),"a new session must allocate beyond all retired publication epochs");
}
void accepted_action_cuts_off_already_dequeued_programme(){
 for(int scenario=0;scenario<7;++scenario) {
  const auto action=scenario==0 ? TransitionAction::SetDelay : scenario==1 ? TransitionAction::ReturnLive : TransitionAction::EmergencyDump;
  struct Gate {std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;};
  struct CutoffSink : Consumer {
   std::mutex mutex;std::condition_variable changed;bool entered=false,released=false,cutoff=false;int leaked=0;
   void consume(const std::shared_ptr<const ReleasedPacketBatch> &batch) override {
    std::unique_lock lock(mutex);
    if(!entered) {entered=true;changed.notify_all();changed.wait(lock,[&]{return released;});return;}
    if(cutoff)for(const auto &packet:batch->packets)if(packet.payload.back()==0xee)++leaked;
    changed.notify_all();
   }
  };
  auto factory=std::make_shared<Gate>();auto sink=std::make_shared<CutoffSink>();
  auto *backend=new ReadyBackend;
  DelayController controller;ReleasedPacketDispatcher dispatcher;
  PipelineOwner owner(controller,dispatcher,1,[backend]{return std::make_unique<HoldingCapture>(std::unique_ptr<ReadyBackend>(backend));},
   [factory,sink](FlvCodecHeaders,SenderErrorCallback,std::string &){
    std::unique_lock lock(factory->mutex);factory->entered=true;factory->changed.notify_all();factory->changed.wait(lock,[&]{return factory->released;});return sink;
   },{});
  owner.start();owner.headers(programme_test_headers());
  bool factory_entered;
  {std::unique_lock lock(factory->mutex);factory_entered=factory->changed.wait_for(lock,2s,[&]{return factory->entered;});}
  owner.enqueue({PacketKind::Video,{0,0,0,2,0x65,0x50},10000,10000,true});
  owner.enqueue({PacketKind::Audio,{0x77},11000,11000,false});
  for(int i=0;i<8;++i){
   owner.enqueue({PacketKind::Video,{0,0,0,2,0x41,0xee},12000+i*2000LL,12000+i*2000LL,false});
   owner.enqueue({PacketKind::Audio,{0xee},13000+i*2000LL,13000+i*2000LL,false});
  }
  {std::scoped_lock lock(factory->mutex);factory->released=true;factory->changed.notify_all();}
  bool entered;
  {std::unique_lock lock(sink->mutex);entered=sink->changed.wait_for(lock,2s,[&]{return sink->entered;});}
  std::string error;bool accepted=true;
   if(scenario==3)owner.request_stop();
   else if(scenario==6){auto changed=holding_test_headers();changed.aac_audio_specific_config={0x12,0x10};backend->feed->set_headers(changed);accepted=owner.status().phase==BroadcastPhase::Failed;}
   else if(scenario>=4){auto changed=programme_test_headers();if(scenario==4)changed.aac_audio_specific_config={0x12,0x10};else changed.avc_decoder_configuration.back()^=1;owner.headers(changed);owner.headers(changed);accepted=owner.status().phase==BroadcastPhase::Failed;}
   else accepted=owner.request({action,100ms},error);
  int leaked;
  {std::unique_lock lock(sink->mutex);sink->cutoff=accepted;sink->released=true;sink->changed.notify_all();
   sink->changed.wait_for(lock,100ms,[&]{return sink->leaked>0;});leaked=sink->leaked;}
  owner.stop();
  std::cout<<"accepted-cutoff action="<<int(action)<<" accepted="<<accepted<<" newly published old-tail packets="<<leaked<<'\n';
  require(factory_entered && entered && accepted,"controlled accepted action cutoff fixture");
  require(leaked==0,"accepted transition must invalidate programme already dequeued by the owner, not only future capture callbacks");
 }
}

void initial_secondary_stall_cannot_disable_privacy(bool first_media){
 struct WireState {std::mutex mutex;std::condition_variable changed;int sends=0;bool entered=false,stop=false;};
 struct Wire : IRtmpConnection {
  std::shared_ptr<WireState> state;bool block,media_only;
  Wire(std::shared_ptr<WireState> s,bool b,bool m):state(s),block(b),media_only(m){}
  bool connect(const RtmpTarget &,std::string &) override{return true;}
  bool send(std::span<const std::uint8_t> bytes,std::string &) override{
   std::unique_lock lock(state->mutex);++state->sends;
   const bool media=bytes.size()>12 && (bytes[0]==8 || bytes[0]==9) && bytes[12]==1;
   if(block && state->sends>3 && (!media_only || media)){state->entered=true;state->changed.notify_all();state->changed.wait(lock,[&]{return state->stop;});return false;}
   return true;
  }
  void interrupt() noexcept override{std::scoped_lock lock(state->mutex);state->stop=true;state->changed.notify_all();}
  void close() noexcept override{}
 };
 DelayController controller;ReleasedPacketDispatcher dispatcher;
 auto primary=std::make_shared<WireState>(),secondary=std::make_shared<WireState>();int allocated=0;
 auto *backend=new ReadyBackend;
 auto multi=std::make_shared<MultiTargetSender>([&]()->std::unique_ptr<IRtmpConnection>{const bool b=allocated++!=0;return std::make_unique<Wire>(b?secondary:primary,b,first_media);},SenderConfig{});
 PipelineOwner owner(controller,dispatcher,1,[backend]{return std::make_unique<HoldingCapture>(std::unique_ptr<ReadyBackend>(backend));},
  [&](FlvCodecHeaders h,SenderErrorCallback cb,std::string &e)->std::shared_ptr<ReleasedPacketConsumer>{
   MultistreamConfiguration config;config.secondary_destinations.push_back({"secondary_2","Synthetic",{"rtmp://127.0.0.1/live","synthetic"}});
   if(!multi->start({"rtmp://127.0.0.1/primary","synthetic"},"Primary",config,h,cb,e))return {};return multi;
  },{},100ms,[](const auto &consumer){
   auto s=std::dynamic_pointer_cast<MultiTargetSender>(consumer)->status();bool ready=false;
   for(const auto &d:s.destinations){if(d.primary)ready=d.sender.state==SenderState::Running;if(d.sender.state==SenderState::Starting || d.sender.state==SenderState::Reconnecting)return false;}return ready;
  });
 owner.start();owner.headers(programme_test_headers());
 auto feed=[&](int i){auto d=1000000LL+i*20000LL;owner.enqueue_uninterleaved({PacketKind::Video,{0,0,0,2,0x65,0x50},d,d,i%10==0},owner.capture_token());owner.enqueue_uninterleaved({PacketKind::Audio,{0x77},d,d,false},owner.capture_token());};
 for(int i=0;i<20;++i)feed(i);
 auto deadline=std::chrono::steady_clock::now()+1s;
 while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 std::string error;require(owner.start_broadcast(error),"secondary stall fixture starts from Ready");
 for(int i=20;i<170;++i){feed(i);std::this_thread::sleep_for(10ms);}
 const auto status=owner.status();const auto network=multi->status();
 const auto token=owner.capture_token();const bool accepted=owner.request({TransitionAction::EmergencyDump,{}},error);
 const bool stale=owner.enqueue({PacketKind::Audio,{0xee},5000000,5000000,false},token);
 bool entered,cancelled;{std::scoped_lock lock(secondary->mutex);entered=secondary->entered;cancelled=secondary->stop;}
 const auto protected_epoch=dispatcher.publication_epoch();
 if(accepted) {
  backend->feed->ingest({PacketKind::Video,{0,0,0,2,0x65,0x48},100000,100000,true});
  backend->feed->ingest({PacketKind::Audio,{0},101000,101000,false});
  backend->feed->ingest({PacketKind::Video,{0,0,0,2,0x41,0x48},102000,102000,false});
  backend->feed->ingest({PacketKind::Audio,{0},103000,103000,false});
 }
 deadline=std::chrono::steady_clock::now()+1s;
 while(accepted && !multi->boundary_delivered(protected_epoch) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 const bool protected_boundary=multi->boundary_delivered(protected_epoch);
 int primary_sends;{std::scoped_lock lock(primary->mutex);primary_sends=primary->sends;}
 owner.stop();
 require(primary_sends>20,"healthy primary media continues during secondary stall");
 require(accepted && protected_boundary && protected_epoch>network.destinations[0].sender.published_epoch,"accepted dump must reach delivered independent holding boundary on primary");
 require(entered && network.destinations[0].sender.published_epoch>0 && network.destinations[1].sender.published_epoch==0,"secondary blocks only after priming while primary delivers");
 require(status.phase==BroadcastPhase::Broadcasting && accepted && !stale,"stalled initial secondary must not disable Emergency Dump on an on-air primary");
 require(cancelled && network.destinations[1].sender.state==SenderState::Failed,"stalled secondary must be asynchronously isolated and cancelled");
}
void changed_headers_pending_start_and_stop_race(){
 for(bool race_stop:{false,true}) {
  struct Gate {std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;};
  struct Sink : Consumer {std::atomic_int calls=0;void consume(const std::shared_ptr<const ReleasedPacketBatch>&)override{++calls;}};
  auto gate=std::make_shared<Gate>();auto sink=std::make_shared<Sink>();
  DelayController controller;ReleasedPacketDispatcher dispatcher;
  PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
   [gate,sink](FlvCodecHeaders,SenderErrorCallback,std::string &){
    std::unique_lock lock(gate->mutex);gate->entered=true;gate->changed.notify_all();gate->changed.wait(lock,[&]{return gate->released;});return sink;
   },{});
  owner.start();owner.headers(programme_test_headers());bool entered;
  {std::unique_lock lock(gate->mutex);entered=gate->changed.wait_for(lock,1s,[&]{return gate->entered;});}
  auto changed=programme_test_headers();changed.aac_audio_specific_config={0x12,0x10};
  std::thread changes([&]{for(int i=0;i<20;++i)owner.headers(changed);});
  if(race_stop)owner.request_stop();changes.join();
  const auto status=owner.status();
  const bool late=owner.enqueue({PacketKind::Audio,{0xee},100000,100000,false},owner.capture_token());
  {std::scoped_lock lock(gate->mutex);gate->released=true;gate->changed.notify_all();}
  owner.stop();
  require(entered && !late && (race_stop || status.phase==BroadcastPhase::Failed),"changed pending startup must latch failure before factory completion");
  require(sink->calls==0 && dispatcher.consumer_count()==0 && owner.status().stopped,"repeated changed snapshots racing stop must quiesce without publication");
 }
}
void changed_headers_revoke_offair_ready(){
 for(bool video:{false,true}){
  DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
  PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
   [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},100ms);
  owner.start();owner.headers(programme_test_headers());
  for(int i=0;i<5;++i){auto d=i*100000LL;owner.enqueue({PacketKind::Video,{1},d,d,true});owner.enqueue({PacketKind::Audio,{2},d,d,false});}
  auto until=std::chrono::steady_clock::now()+1s;
  while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
  owner.headers(programme_test_headers());require(owner.status().phase==BroadcastPhase::Ready,"identical snapshots must be harmless");
  auto changed=programme_test_headers();if(video)changed.avc_decoder_configuration.back()^=1;else changed.aac_audio_specific_config={0x12,0x10};
  owner.headers(changed);std::string error;
  require(owner.status().phase==BroadcastPhase::Failed && !owner.start_broadcast(error),"changed codec snapshot must synchronously revoke Ready");
  owner.stop();require(factories==0,"changed offair headers must not construct consumer");
 }
}
void unsupported_packet_timing_fails_before_network(){
 for(bool native:{false,true})for(bool initially_ready:{false,true})for(int scenario=0;scenario<5;++scenario){
  DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
  PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
   [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},100ms);
  owner.start();owner.headers(programme_test_headers());
  auto admit=[&](EncodedPacket p){return native ? owner.enqueue_uninterleaved(std::move(p),owner.capture_token()) : owner.enqueue(std::move(p),owner.capture_token());};
  if(initially_ready){
  for(int i=0;i<5;++i){auto d=1000000LL+i*100000LL;admit({PacketKind::Video,{1},d,d,true});admit({PacketKind::Audio,{2},d,d,false});}
  const auto until=std::chrono::steady_clock::now()+1s;
  while(owner.status().phase!=BroadcastPhase::Ready && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
  require(owner.status().phase==BroadcastPhase::Ready,"timing regression starts from off-air Ready");
  }
  EncodedPacket bad{PacketKind::Video,{1},2300000,2000000,true};
  if(scenario==1)bad.pts_us=1999999;
  if(scenario==2){bad.kind=PacketKind::Audio;bad.pts_us=2000001;}
  if(scenario==3){bad.pts_us=INT64_MAX;bad.dts_us=INT64_MIN;}
  if(scenario==4){bad.pts_us=INT64_MAX;bad.dts_us=INT64_MAX;}
  const bool accepted=admit(std::move(bad));std::string error;const bool started=owner.start_broadcast(error);
  const auto status=owner.status();owner.stop();
  require(!accepted && !started && status.phase==BroadcastPhase::Failed && !status.ready && factories==0,
   "unsupported source timing must synchronously revoke Ready before retaining history or constructing network");
 }
}
void unsupported_codec_cannot_advertise_off_air_ready(){
 DelayController controller;ReleasedPacketDispatcher dispatcher;std::atomic_int factories=0;
 PipelineOwner owner(controller,dispatcher,1,[]{return std::make_unique<HoldingCapture>(std::make_unique<ReadyBackend>());},
  [&](FlvCodecHeaders,SenderErrorCallback,std::string &){++factories;return std::make_shared<Consumer>();},{},100ms);
 auto headers=programme_test_headers();headers.aac_audio_specific_config={0x11,0x94};
 owner.start();owner.headers(headers);
 for(int i=0;i<4;++i){owner.enqueue({PacketKind::Video,{1},i*100000LL,i*100000LL,true});owner.enqueue({PacketKind::Audio,{2},i*100000LL,i*100000LL,false});}
 const auto until=std::chrono::steady_clock::now()+500ms;
 while(owner.status().phase!=BroadcastPhase::Failed && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
 const auto status=owner.status();owner.stop();
 require(status.phase==BroadcastPhase::Failed && status.error.starts_with("TRANSITION_CODEC_UNSUPPORTED") && !status.holding_ready && factories==0,"unsupported timing cannot advertise protected off-air Ready or connect a destination");
}
}
int main(){try{changed_headers_pending_start_and_stop_race();changed_headers_revoke_offair_ready();unsupported_packet_timing_fails_before_network();initial_secondary_stall_cannot_disable_privacy(false);initial_secondary_stall_cannot_disable_privacy(true);unsupported_codec_cannot_advertise_off_air_ready();new_coordinator_cannot_reuse_a_retired_publication_epoch();accepted_action_cuts_off_already_dequeued_programme();prebuffer_stall_failure_and_pending_stop_are_off_air();stale_packets_behind_prepare_cannot_advertise_ready();malformed_headers_never_make_ready();independent_programme_callbacks_are_bounded_and_ordered();prebuffer_is_off_air_until_explicit_ready_start();failure_before_install_cannot_publish_ready();startup_failure_is_visible_before_factory_returns();overlapping_actions_are_serialized_not_lost();delay_change_keeps_in_flight_programme_capture();std::cout<<"Owner tests passed\n";}
 catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
