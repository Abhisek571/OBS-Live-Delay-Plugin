#include "transition-coordinator.hpp"
#include "network-packet-consumer.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <condition_variable>
#include <thread>
#include <stdexcept>
using namespace active_delay;
using namespace std::chrono_literals;
namespace {
void require(bool v,const char *m){if(!v)throw std::runtime_error(m);}
uint32_t be24(const uint8_t *p){return (uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];}
struct Fixture {FlvCodecHeaders headers;std::vector<EncodedPacket> packets;};
Fixture read(const char *path){
 std::ifstream file(path,std::ios::binary);require(bool(file),"fixture open");
 std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});require(bytes.size()>13,"fixture length");
 Fixture result;
 for(size_t pos=13;pos+15<=bytes.size();){
  auto size=be24(bytes.data()+pos+1);require(pos+15+size<=bytes.size(),"fixture tag length");
  const auto type=bytes[pos];auto t=int64_t(be24(bytes.data()+pos+4)|(uint32_t(bytes[pos+7])<<24))*1000;
  const auto *payload=bytes.data()+pos+11;
  if(type==9 && size>=5){
   if(payload[1]==0)result.headers.avc_decoder_configuration.assign(payload+5,payload+size);
   if(payload[1]==1){auto comp=int32_t(be24(payload+2));if(comp&0x800000)comp-=0x1000000;
    result.packets.push_back({PacketKind::Video,{payload+5,payload+size},t+int64_t(comp)*1000,t,(payload[0]>>4)==1});}
  }
  if(type==8 && size>=2){
   if(payload[1]==0)result.headers.aac_audio_specific_config.assign(payload+2,payload+size);
   if(payload[1]==1)result.packets.push_back({PacketKind::Audio,{payload+2,payload+size},t,t,false});
  }
  pos+=15+size;
 }
 require(!result.headers.avc_decoder_configuration.empty()&&!result.headers.aac_audio_specific_config.empty(),"fixture codec headers");
 return result;
}
struct Wire {
 std::mutex mutex;std::condition_variable changed;
 std::vector<uint8_t> bytes,last;
 std::vector<std::pair<std::vector<uint8_t>,std::chrono::steady_clock::time_point>> events;
};
struct Connection : IRtmpConnection {
 std::shared_ptr<Wire> wire;explicit Connection(std::shared_ptr<Wire> w):wire(std::move(w)){}
 bool connect(const RtmpTarget &,std::string &)override{return true;}
 bool send(std::span<const uint8_t> bytes,std::string &)override{
  std::scoped_lock lock(wire->mutex);wire->bytes.insert(wire->bytes.end(),bytes.begin(),bytes.end());wire->last.assign(bytes.begin(),bytes.end());wire->events.emplace_back(wire->last,std::chrono::steady_clock::now());wire->changed.notify_all();return true;
 }
 void interrupt()noexcept override{} void close()noexcept override{}
};
struct Observer : ReleasedPacketConsumer {
 FlvMuxer muxer;std::vector<uint8_t> expected;std::int64_t previous_dts=-1000;
 struct Guard {std::uint64_t epoch;std::vector<uint8_t> drain,video;std::int64_t drain_dts,video_dts=0;};
 std::vector<Guard> guards;
 explicit Observer(FlvCodecHeaders h):muxer(std::move(h)){}
 void consume(const std::shared_ptr<const ReleasedPacketBatch>&b)override{
  if(b->headers)muxer.set_headers(*b->headers);
  std::vector<FlvTag> tags;std::string error;require(muxer.mux(b->packets,tags,error),"observer mux");
  if(!tags.empty())expected=serialize_flv_tag(tags.back());
  // The drain is the first drain batch of its epoch; later single silent frames
  // are the bridge that fills the audio gap before the holding picture.
  if(b->headers && b->packets.front().audio_drain && b->packets.size()==1 && (guards.empty() || guards.back().epoch!=b->epoch))
   guards.push_back({b->epoch,serialize_flv_tag(tags.front()),{},previous_dts});
  previous_dts=b->packets.back().dts_us;
  for(std::size_t i=0;i<b->packets.size();++i)if(b->packets[i].kind==PacketKind::Video && !guards.empty() && guards.back().epoch==b->epoch && guards.back().video.empty()) {
   guards.back().video=serialize_flv_tag(tags[i]);guards.back().video_dts=b->packets[i].dts_us;
  }
 }
 void discontinuity(const PacketDiscontinuity &)override{} void stop()noexcept override{}
};
}
int main(int argc,char **argv){try{
 require(argc==4,"usage: splice programme.flv holding.flv output.flv");
 auto programme=read(argv[1]),holding=read(argv[2]);
 auto wire=std::make_shared<Wire>();auto consumer=std::make_shared<NetworkPacketConsumer>([wire]{return std::make_unique<Connection>(wire);},SenderConfig{});
 std::string error;require(consumer->start({"rtmp://fake/live","test"},programme.headers,{},error),"start");
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(consumer->status().state!=SenderState::Running && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 require(consumer->status().state==SenderState::Running,"sender ready");
 DelayController controller;ReleasedPacketDispatcher dispatcher;dispatcher.add_consumer(consumer);
 auto observer=std::make_shared<Observer>(programme.headers);dispatcher.add_consumer(observer);
 TransitionCoordinator coordinator(controller,dispatcher,1,programme.headers);coordinator.holding_ready(holding.headers);
 auto flush=[&]{if(observer->expected.empty())return;std::unique_lock lock(wire->mutex);require(wire->changed.wait_for(lock,2s,[&]{return wire->last==observer->expected;}),"actual sender must deliver final staged packet before next transition");};
 auto program=[&](int64_t begin,int64_t end){
  const auto start=std::chrono::steady_clock::now();
  for(auto p:programme.packets)if(p.dts_us>=begin&&p.dts_us<end){
   while(std::chrono::steady_clock::now()<start+Microseconds{p.dts_us-begin}){coordinator.check_deadline();std::this_thread::sleep_for(1ms);}
   coordinator.programme(std::move(p));coordinator.check_deadline();
  }
  const auto until=std::chrono::steady_clock::now()+300ms;
  while(std::chrono::steady_clock::now()<until){coordinator.check_deadline();std::this_thread::sleep_for(1ms);}
  flush();};
 auto hold=[&](int64_t clock){auto packets=holding.packets;for(auto &p:packets){p.dts_us+=clock;p.pts_us+=clock;}coordinator.holding(std::move(packets));
  const auto until=std::chrono::steady_clock::now()+1200ms;
  while(std::chrono::steady_clock::now()<until){coordinator.check_deadline();std::this_thread::sleep_for(1ms);}
  coordinator.check_deadline();flush();};
 program(0,1000000);
 coordinator.request({TransitionAction::SetDelay,Microseconds{1000000}});hold(10000000);program(1000000,3300000);
 coordinator.request({TransitionAction::ReturnLive,{}});hold(20000000);program(3300000,5300000);
 coordinator.request({TransitionAction::SetDelay,Microseconds{1000000}});hold(30000000);program(5300000,7500000);
 coordinator.request({TransitionAction::EmergencyDump,{}});require(controller.status().target_delay==Microseconds{1000000},"dump retains target");hold(40000000);program(7500000,10000000);
 dispatcher.stop_all();
 require(observer->guards.size()==4,"all four paced guards must reach the actual sender");
 for(const auto &guard:observer->guards) {
  auto written=[&](const auto &bytes){for(const auto &event:wire->events)if(event.first==bytes)return event.second;throw std::runtime_error("guard media absent on wire");};
  const auto elapsed=std::chrono::duration_cast<Microseconds>(written(guard.video)-written(guard.drain)).count();
  const auto minimum=guard.video_dts-guard.drain_dts;
  require(elapsed>=minimum,"wire must not burst future holding video after drain enqueue");
  std::cout<<"guard epoch="<<guard.epoch<<" wire_interval_us="<<elapsed<<" required_us="<<minimum<<'\n';
 }
 std::ofstream output(argv[3],std::ios::binary);output.write(reinterpret_cast<const char*>(wire->bytes.data()),std::streamsize(wire->bytes.size()));require(bool(output),"splice file write");
 std::cout<<"Actual coordinator/dispatcher/consumer/muxer/sender codec splice: "<<wire->bytes.size()<<" bytes\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
