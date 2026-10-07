#include "network-packet-consumer.hpp"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace active_delay;
using namespace std::chrono_literals;
namespace {
void require(bool v,const char *m){if(!v)throw std::runtime_error(m);}
struct State {
 std::mutex mutex; std::condition_variable changed;
 std::vector<std::vector<uint8_t>> writes;
 int block_at=3;bool entered=false,released=false,interrupted=false;
};
struct Connection : IRtmpConnection {
 std::shared_ptr<State> state;
 explicit Connection(std::shared_ptr<State> s):state(std::move(s)){}
 bool connect(const RtmpTarget &,std::string &) override{return true;}
 bool send(std::span<const uint8_t> bytes,std::string &) override {
  std::unique_lock lock(state->mutex);
  if(int(state->writes.size())+1==state->block_at){state->entered=true;state->changed.notify_all();state->changed.wait(lock,[&]{return state->released;});}
  state->writes.emplace_back(bytes.begin(),bytes.end());state->changed.notify_all();return true;
 }
 void interrupt() noexcept override{std::scoped_lock lock(state->mutex);state->interrupted=true;state->changed.notify_all();}
 void close() noexcept override{}
};
EncodedPacket video(int64_t t,uint8_t id){return {PacketKind::Video,{0,0,0,2,0x65,id},t+2000,t,true};}
std::shared_ptr<const ReleasedPacketBatch> batch(uint64_t e,int64_t t,uint8_t id,std::optional<FlvCodecHeaders> h={}){
 return std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{e,{video(t,id),{PacketKind::Audio,{id},t+1000,t+1000,false}},h});
}
void release(const std::shared_ptr<State> &s){std::scoped_lock lock(s->mutex);s->released=true;s->changed.notify_all();}
bool wait_ready(NetworkPacketConsumer &consumer){const auto end=std::chrono::steady_clock::now()+2s;while(consumer.status().state!=SenderState::Running && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return consumer.status().state==SenderState::Running;}
void selection_during_priming_uses_current_headers(){
 auto state=std::make_shared<State>();NetworkPacketConsumer consumer([state]{return std::make_unique<Connection>(state);},{{32,4096},1,1ms,2s});
 std::string error;require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{18,16}},{},error),"start");
 bool entered;
 {std::unique_lock lock(state->mutex);entered=state->changed.wait_for(lock,2s,[&]{return state->entered;});}
 if(!entered){release(state);consumer.stop();require(false,"initial priming blocked");}
 consumer.discontinuity({2,"holding"});
 consumer.consume(batch(2,0,0x48,FlvCodecHeaders{{1,66,0,31},{17,144}})); // accepted/discarded while Starting
 release(state);require(wait_ready(consumer),"ready after controlled prime");
 consumer.consume(batch(2,100000,0x49));
 {std::unique_lock lock(state->mutex);require(state->changed.wait_for(lock,2s,[&]{
  return std::any_of(state->writes.begin(),state->writes.end(),[](const auto &w){return w.size()>17 && w[0]==9 && w[12]==1;});
 }),"fresh media must actually enter transport after controlled priming");}
 consumer.stop();
 uint8_t selected=0;bool matched=false;
 for(const auto &w:state->writes){if(w.size()>17 && w[0]==9 && w[12]==0)selected=w[17];if(w.size()>17 && w[0]==9 && w[12]==1){matched=selected==66;break;}}
 require(matched,"selected holding headers must precede media when selection raced initial priming");
}
void entered_write_delays_ack_but_not_capture(){
 auto state=std::make_shared<State>();state->block_at=4;
 NetworkPacketConsumer consumer([state]{return std::make_unique<Connection>(state);},{{32,4096},1,1ms,2s});
 std::string error;require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{18,16}},{},error),"start");
 require(wait_ready(consumer),"ready");consumer.consume(batch(1,0,0x50));
 bool entered;
 {std::unique_lock lock(state->mutex);entered=state->changed.wait_for(lock,2s,[&]{return state->entered;});}
 for(int i=0;i<10;++i)consumer.consume(batch(1,10000+i*2000,0xee));
 const auto queued_before=consumer.status().queued_tags;
 const auto start=std::chrono::steady_clock::now();consumer.discontinuity({2,"dump"});consumer.discontinuity({3,"overlap"});
 const auto queued_after=consumer.status().queued_tags;
 consumer.consume(batch(1,10000,0xee));
 const auto elapsed=std::chrono::steady_clock::now()-start;
 const auto acknowledged=consumer.status().acknowledged_epoch;
 std::cout<<"blocked-write invalidation/admission us="<<std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()<<" queued_before="<<queued_before<<" queued_after="<<queued_after<<" ack="<<acknowledged<<"\n";
 release(state);consumer.stop();
 require(entered,"entered write fixture");require(queued_before>=20 && queued_after==0,"epoch invalidation must reclaim queued programme under controlled pressure");require(elapsed<100ms,"invalidation and stale enqueue must isolate capture from blocked writes");
 require(!consumer.boundary_delivered(3),"invalidated epoch is not a delivered new codec/keyframe/audio boundary");
 require(acknowledged<3,"transition cannot acknowledge while prior entered write remains active");
 require(state->interrupted,"transition must request transport cancellation");
 for(const auto &w:state->writes)require(w.size()<5 || w[w.size()-5]!=0xee,"queued and racing stale media payload IDs must never enter transport after invalidation");
}
struct StickyConnection : Connection {
 std::atomic_bool cancelled=false;
 explicit StickyConnection(std::shared_ptr<State> state):Connection(std::move(state)){}
 bool connect(const RtmpTarget &,std::string &error) override{
  if(cancelled){error="sticky transport cancelled";return false;}return true;
 }
 bool send(std::span<const uint8_t> bytes,std::string &error) override{
  if(cancelled){error="sticky transport cancelled";return false;}
  const bool sent=Connection::send(bytes,error);
  return sent && !cancelled;
 }
 void interrupt() noexcept override{cancelled=true;Connection::interrupt();}
};
void cancelled_transition_replaces_sticky_transport(){
 auto state=std::make_shared<State>();state->block_at=4;
 auto creations=std::make_shared<std::atomic_int>(0);
 NetworkPacketConsumer consumer([state,creations]{++*creations;return std::make_unique<StickyConnection>(state);},{{32,4096},2,1ms,2s});
 std::string error;require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{18,16}},{},error),"start");
 require(wait_ready(consumer),"initial ready");consumer.consume(batch(1,0,0x50));
 bool entered;
 {std::unique_lock lock(state->mutex);entered=state->changed.wait_for(lock,2s,[&]{return state->entered;});}
 consumer.discontinuity({2,"holding"});consumer.consume(batch(2,100000,0x48,FlvCodecHeaders{{1,66,0,31},{17,144}}));
 release(state);
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(consumer.status().reconnect_count==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 const bool resumed=wait_ready(consumer);
 if(resumed)consumer.consume(batch(2,200000,0x49));
 std::this_thread::sleep_for(10ms);consumer.stop();
 require(entered && resumed && creations->load()>=2,"cancelled in-flight transition must recover with a fresh transport; sticky cancellation cannot be reset/reused");
 uint8_t selected=0;bool recovery=false,matched=false;
 for(const auto &w:state->writes){
  if(w.size()==13 && w[0]=='F') {if(recovery)continue;recovery=true;selected=0;}
  if(w.size()>17 && w[0]==9 && w[12]==0)selected=w[17];
  if(w.size()>17 && w[0]==9 && w[12]==1 && selected==66)matched=true;
 }
 require(matched,"reconnected transition must prime selected holding headers and fresh holding media");
}

struct ClosingConnection : Connection {
 bool fail_media=true;int closes=0;
 explicit ClosingConnection(std::shared_ptr<State> state):Connection(std::move(state)){}
 bool send(std::span<const uint8_t> bytes,std::string &error) override{
  if(fail_media && bytes.size()>12 && bytes[0]==9 && bytes[12]==1){fail_media=false;error="injected media fault";return false;}
  return Connection::send(bytes,error);
 }
 void close() noexcept override{
  if(closes++==0){std::unique_lock lock(state->mutex);state->entered=true;state->changed.notify_all();state->changed.wait(lock,[&]{return state->released;});}
 }
};
void entered_close_flush_also_delays_epoch_ack(){
 auto state=std::make_shared<State>();state->block_at=0;
 NetworkPacketConsumer consumer([state]{return std::make_unique<ClosingConnection>(state);},{{32,4096},1,1ms,2s});
 std::string error;require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{18,16}},{},error),"start");
 require(wait_ready(consumer),"ready");consumer.consume(batch(1,0,0x50));
 bool entered;
 {std::unique_lock lock(state->mutex);entered=state->changed.wait_for(lock,2s,[&]{return state->entered;});}
 consumer.discontinuity({2,"dump during close flush"});const auto ack=consumer.status().acknowledged_epoch;
 release(state);consumer.stop();
 require(entered && ack<2,"entered transport close can flush old bytes and must finish/cancel before epoch acknowledgement");
}


void silent_drain_delivery_is_not_a_visible_boundary(){
 auto state=std::make_shared<State>();state->block_at=6;
 NetworkPacketConsumer consumer([state]{return std::make_unique<Connection>(state);},{{32,4096},1,1ms,2s});
 std::string error;require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{17,144}},{},error),"start");require(wait_ready(consumer),"ready");
 consumer.discontinuity({2,"guard"});
 consumer.consume(std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{2,{{PacketKind::Audio,{0},1000,1000,false,true}},FlvCodecHeaders{{1,100,0,31},{17,144}},1}));
 bool entered;
 {std::unique_lock lock(state->mutex);entered=state->changed.wait_for(lock,2s,[&]{return state->entered;});}
 const bool early=consumer.delivered(2,1),visible=consumer.boundary_delivered(2);
 release(state);
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(!consumer.delivered(2,1) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 const bool delivered=consumer.delivered(2,1),still_invisible=!consumer.boundary_delivered(2);
 consumer.discontinuity({3,"superseding"});
 const bool stale=consumer.delivered(3,1);
 consumer.stop();
 require(entered && !early && !visible && delivered && still_invisible && !stale,"actual silent drain write needs completion, never keyframe acknowledgement or newer-epoch acknowledgement");
 require(consumer.start({"rtmp://fake/live","test"},{{1,100,0,31},{17,144}},{},error),"restart");require(wait_ready(consumer),"restart ready");
 const bool reused=consumer.delivered(2,1);consumer.stop();
 require(!reused,"restarted sender must not reuse old guard delivery ticket");
}
}
int main(){try{silent_drain_delivery_is_not_a_visible_boundary();selection_during_priming_uses_current_headers();entered_write_delays_ack_but_not_capture();cancelled_transition_replaces_sticky_transport();entered_close_flush_also_delays_epoch_ack();std::cout<<"Epoch pipeline tests passed\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
