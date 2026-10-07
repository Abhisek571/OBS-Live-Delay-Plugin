#include "active-delay-output.hpp"
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <media-io/audio-io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace active_delay;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
std::atomic_int service_reads=0;
std::string local_server;
std::atomic_bool programme_green=false;
void colour_render(void *data,gs_effect_t *){
 vec4 colour{};colour.w=1.0f;
 if(data==&programme_green){if(programme_green)colour.y=1.0f;else colour.x=1.0f;}else colour.z=1.0f;
 gs_clear(GS_CLEAR_COLOR,&colour,0.0f,0);
}
const char *service_info(void *,uint32_t type){
 ++service_reads;
 if(type==OBS_SERVICE_CONNECT_INFO_SERVER_URL)return local_server.c_str();
 if(type==OBS_SERVICE_CONNECT_INFO_STREAM_KEY)return "synthetic";
 return "";
}
struct OutputGuard {
 obs_output_t *output=nullptr;
 ~OutputGuard(){if(output){if(obs_output_active(output))obs_output_force_stop(output);obs_output_release(output);}}
};
}
int main(int argc,char **argv){
 try {
  check(argc==4 || argc==5,"installed OBS root and numeric loopback ports required");
  const bool guard_test=argc==5 && std::string(argv[4])=="--guard";
  const auto *encoder_env=std::getenv("ALD_NATIVE_TEST_ENCODER");
  const std::string encoder=encoder_env ? encoder_env : "obs_x264";
  check(encoder=="obs_x264" || encoder=="obs_nvenc_h264_tex","unsupported test encoder");
  const auto *soak_env=std::getenv("ALD_NATIVE_TEST_SOAK_SECONDS");
  const int soak_seconds=soak_env ? std::stoi(soak_env) : 0;
  check(soak_seconds>=0 && soak_seconds<=10800,"soak must be within zero to three hours");
  const int port=std::stoi(argv[2]);check(port>=1024 && port<=65535,"invalid local test port");
  local_server="rtmp://127.0.0.1:"+std::to_string(port)+"/local";
  const std::filesystem::path root=argv[1];
  const auto directory=AddDllDirectory((root/"bin/64bit").c_str());
  check(directory,"native test dependency directory unavailable");
  struct DllGuard{DLL_DIRECTORY_COOKIE cookie;~DllGuard(){RemoveDllDirectory(cookie);}} dll{directory};
  check(obs_startup("en-US",nullptr,nullptr),"isolated libobs startup failed");
  struct ObsGuard{~ObsGuard(){obs_shutdown();}} obs;
  const auto data=(root/"data/libobs").string()+"/";obs_add_data_path(data.c_str());
  for(const auto *name:{"obs-x264","obs-ffmpeg"}){
   obs_module_t *module=nullptr;
   const auto binary=(root/"obs-plugins/64bit"/(std::string(name)+".dll")).string();
   const auto module_data=(root/"data/obs-plugins"/name).string();
   check(obs_open_module(&module,binary.c_str(),module_data.c_str())==MODULE_SUCCESS && obs_init_module(module),"native codec module unavailable");
  }
  obs_audio_info audio{};audio.samples_per_sec=48000;audio.speakers=SPEAKERS_STEREO;
  check(obs_reset_audio(&audio),"synthetic global audio initialization failed");
  obs_video_info video{};
  const auto graphics=(root/"bin/64bit/libobs-d3d11.dll").string();
  video.graphics_module=graphics.c_str();video.fps_num=30;video.fps_den=1;
  video.base_width=video.output_width=320;video.base_height=video.output_height=180;
  video.output_format=VIDEO_FORMAT_NV12;video.adapter=0;video.gpu_conversion=true;
  video.colorspace=VIDEO_CS_709;video.range=VIDEO_RANGE_PARTIAL;video.scale_type=OBS_SCALE_BILINEAR;
  check(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"headless synthetic D3D11 video unavailable");
  if(encoder=="obs_nvenc_h264_tex"){
   obs_module_t *module=nullptr;
   const auto binary=(root/"obs-plugins/64bit/obs-nvenc.dll").string();
   const auto module_data=(root/"data/obs-plugins/obs-nvenc").string();
   check(obs_open_module(&module,binary.c_str(),module_data.c_str())==MODULE_SUCCESS && obs_init_module(module),"native NVENC module unavailable");
  }
  obs_source_info colour_info{};colour_info.id="prebuffer_colour";colour_info.type=OBS_SOURCE_TYPE_INPUT;colour_info.output_flags=OBS_SOURCE_VIDEO;
  colour_info.get_name=[](void *){return "Synthetic prebuffer colour";};
  colour_info.create=[](obs_data_t *s,obs_source_t *)->void *{return obs_data_get_bool(s,"programme") ? static_cast<void *>(&programme_green) : static_cast<void *>(&service_reads);};
  colour_info.destroy=[](void *){};colour_info.get_width=[](void *)->uint32_t{return 320;};colour_info.get_height=[](void *)->uint32_t{return 180;};colour_info.video_render=colour_render;
  obs_register_source(&colour_info);
  auto *programme=obs_scene_create("Prebuffer synthetic programme");
  auto *holding=obs_scene_create("Prebuffer synthetic holding");
  check(programme && holding,"synthetic scenes unavailable");
  struct Scenes{obs_scene_t *p,*h;~Scenes(){obs_set_output_source(0,nullptr);obs_source_remove(obs_scene_get_source(p));obs_source_remove(obs_scene_get_source(h));obs_scene_release(p);obs_scene_release(h);}} scenes{programme,holding};
  for(bool is_programme:{true,false}){
   auto *colour_settings=obs_data_create();obs_data_set_bool(colour_settings,"programme",is_programme);
   auto *source=obs_source_create("prebuffer_colour",is_programme?"Red then green programme":"Blue holding",colour_settings,nullptr);
   obs_data_release(colour_settings);check(source,"synthetic colour source unavailable");
   obs_scene_add(is_programme?programme:holding,source);obs_source_release(source);
  }
  obs_set_output_source(0,obs_scene_get_source(programme));
  obs_service_info info{};info.id="prebuffer_local_service";
  info.get_name=[](void *){return "Synthetic loopback only";};
  info.create=[](obs_data_t *,obs_service_t *)->void *{return &service_reads;};info.destroy=[](void *){};
  info.get_protocol=[](void *){return "RTMP";};info.get_connect_info=service_info;
  obs_register_service(&info);
  auto *service=obs_service_create(info.id,"Prebuffer local service",nullptr,nullptr);
  check(service,"synthetic service unavailable");
  struct ServiceGuard{obs_service_t *p;~ServiceGuard(){obs_service_release(p);}} sg{service};
  auto session=std::make_shared<ActiveDelaySession>();register_active_delay_output(session);
  session->set_holding_scene("Prebuffer synthetic holding");
  std::string error;check(session->set_prebuffer_target(1s,error),"positive target setup failed");
  const int secondary_port=std::stoi(argv[3]);
  if(secondary_port){
   check(secondary_port>=1024 && secondary_port<=65535,"invalid secondary local port");
   check(session->set_operating_mode(OperatingMode::NativeMultistream,error),"native multistream setup failed");
   MultistreamConfiguration configuration;
   MultistreamDestination secondary;secondary.id="synthetic-secondary";secondary.name="Synthetic local secondary";
   secondary.target.server_url="rtmp://127.0.0.1:"+std::to_string(secondary_port)+"/local";secondary.target.stream_key="synthetic";
   configuration.secondary_destinations.push_back(std::move(secondary));session->multistream.set(std::move(configuration));
  }
  audio_t *tone=nullptr;
  struct ToneGuard{audio_t *&audio;~ToneGuard(){if(audio)audio_output_close(audio);}} tone_guard{tone};
  std::uint64_t sample=0;
  if(guard_test){
   audio_output_info input{};input.name="Isolated guard programme tone";input.samples_per_sec=48000;input.format=AUDIO_FORMAT_FLOAT_PLANAR;input.speakers=SPEAKERS_STEREO;input.input_param=&sample;
   input.input_callback=[](void *data,uint64_t start,uint64_t,uint64_t *timestamp,uint32_t active,audio_output_data *mixes){
    auto &sample=*static_cast<std::uint64_t *>(data);
    for(std::size_t mix=0;mix<MAX_AUDIO_MIXES;++mix)if(active&(1u<<mix))for(int channel=0;channel<2;++channel)for(std::size_t i=0;i<AUDIO_OUTPUT_FRAMES;++i)
     reinterpret_cast<float *>(mixes[mix].data[channel])[i]=0.2f*std::sin(float((sample+i)%48000)*6.28318530718f*440.0f/48000.0f);
    sample+=AUDIO_OUTPUT_FRAMES;*timestamp=start;return true;
   };
   check(audio_output_open(&tone,&input)==AUDIO_OUTPUT_SUCCESS,"isolated programme tone unavailable");
  }
  for(int cycle=-1;cycle<2;++cycle){
   obs_data_t *settings=obs_data_create();obs_data_set_int(settings,"bitrate",400);obs_data_set_int(settings,"keyint_sec",1);
   obs_data_set_string(settings,"preset",encoder=="obs_x264" ? "ultrafast" : "p5");obs_data_set_string(settings,"rate_control","CBR");
   auto *v=obs_video_encoder_create(encoder.c_str(),"Prebuffer synthetic H264",settings,nullptr);
   obs_data_set_int(settings,"bitrate",96);
   auto *a=obs_audio_encoder_create("ffmpeg_aac","Prebuffer synthetic AAC",settings,0,nullptr);
   obs_data_release(settings);check(v && a,"programme encoders unavailable");
   struct Encoders{obs_encoder_t *v,*a;~Encoders(){obs_encoder_release(v);obs_encoder_release(a);}} enc{v,a};
   obs_encoder_set_video(v,obs_get_video());obs_encoder_set_audio(a,tone ? tone : obs_get_audio());
   settings=obs_data_create();
   obs_data_set_int(settings,"programme_audio",static_cast<int64_t>(reinterpret_cast<intptr_t>(a)));
   obs_data_set_int(settings,"programme_service",static_cast<int64_t>(reinterpret_cast<intptr_t>(service)));
   OutputGuard out{obs_output_create("active_delay_rtmp_output","Prebuffer synthetic capture",settings,nullptr)};
   obs_data_release(settings);check(out.output,"production output unavailable");
   obs_output_set_video_encoder(out.output,v);
   check((obs_output_get_flags(out.output)&OBS_OUTPUT_SERVICE)==0,"arming must not activate an OBS network service");
   check(obs_output_start(out.output),"production Arm failed");
   if(cycle<0){obs_output_force_stop(out.output);continue;}
   const auto deadline=std::chrono::steady_clock::now()+7s;
   while(session->pipeline_status().phase!=BroadcastPhase::Ready && session->pipeline_status().error.empty() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(10ms);
   const auto status=session->pipeline_status();
   if(!status.error.empty()){
    std::cerr<<status.error<<'\n';
    for(auto *encoder_handle:{v,a}){
     uint8_t *header=nullptr;size_t size=0;
     if(obs_encoder_get_extra_data(encoder_handle,&header,&size) && size<=4096){
      std::cerr<<"SYNTHETIC_HEADER "<<obs_encoder_get_id(encoder_handle)<<" ";
      const char *digits="0123456789abcdef";
      for(size_t i=0;i<size;++i)std::cerr<<digits[header[i]>>4]<<digits[header[i]&15];
      std::cerr<<'\n';
     }
    }
   }
   check(status.phase==BroadcastPhase::Ready,"real programme capture did not become Ready");
   check(service_reads==0 && session->consumers.dispatcher.consumer_count()==0,"Arm must not read destinations or create a network consumer");
   check(session->controller.delay.status().current_delay>=1s,"Ready must have actual compressed history");
   std::this_thread::sleep_for(2200ms);
   check(session->pipeline_status().phase==BroadcastPhase::Ready,"real rolling capture must remain Ready while waiting");
   check(session->controller.delay.status().current_delay<3s,"real rolling capture must not retain stale historical prefix");
   if(cycle==1){
    programme_green=true;
    check(session->start_broadcast(error),"explicit ready Start rejected");
    check(!session->start_broadcast(error),"repeated Start must be rejected");
    const auto broadcast_deadline=std::chrono::steady_clock::now()+7s;
    while(session->pipeline_status().phase!=BroadcastPhase::Broadcasting && session->pipeline_status().error.empty() && std::chrono::steady_clock::now()<broadcast_deadline)std::this_thread::sleep_for(10ms);
    if(!session->pipeline_status().error.empty())std::cerr<<session->pipeline_status().error<<'\n';
    check(session->pipeline_status().phase==BroadcastPhase::Broadcasting,"real local delayed broadcast did not publish programme boundary");
    check(service_reads>0,"explicit Start must read synthetic destination");
    std::this_thread::sleep_for(3300ms);
    if(!session->pipeline_status().error.empty())std::cerr<<session->pipeline_status().error<<'\n';
    check(session->pipeline_status().phase==BroadcastPhase::Broadcasting,"local broadcast failed");
    if(guard_test){
     for(const auto request:{TransitionRequest{TransitionAction::SetDelay,2s},TransitionRequest{TransitionAction::SetDelay,1s},TransitionRequest{TransitionAction::EmergencyDump,{}},TransitionRequest{TransitionAction::ReturnLive,{}}}){
      check(session->request_transition(request,error),"native transition rejected");
      const auto until=std::chrono::steady_clock::now()+4500ms;
      do {std::this_thread::sleep_for(10ms);}while(session->pipeline_status().transition_pending && session->pipeline_status().error.empty() && std::chrono::steady_clock::now()<until);
      if(!session->pipeline_status().error.empty())std::cerr<<session->pipeline_status().error<<'\n';
      check(!session->pipeline_status().transition_pending && session->pipeline_status().error.empty(),"native paced transition did not complete");
      const auto expected=request.action==TransitionAction::EmergencyDump ? 1s : request.action==TransitionAction::ReturnLive ? 0s : request.target;
      check(session->controller.delay.status().target_delay==expected,"native transition lost selected target");
      std::this_thread::sleep_for(350ms);
     }
    }
    const auto soak_start=std::chrono::steady_clock::now();
    for(int second=0;second<soak_seconds;++second){
     std::this_thread::sleep_until(soak_start+std::chrono::seconds(second+1));
     const auto live=session->pipeline_status();
     check(live.phase==BroadcastPhase::Broadcasting && live.error.empty(),"sustained native broadcast failed");
     check(obs_output_active(out.output),"sustained native output stopped unexpectedly");
     if(second%60==0 || second+1==soak_seconds)
      std::cout<<"SOAK seconds="<<second+1<<" buffered_bytes="<<session->controller.delay.status().buffered_bytes<<std::endl;
    }
   }
   obs_output_force_stop(out.output);
   const auto stop_deadline=std::chrono::steady_clock::now()+5s;
   while(!session->pipeline_status().stopped && std::chrono::steady_clock::now()<stop_deadline)std::this_thread::sleep_for(10ms);
   check(session->pipeline_status().stopped && session->controller.delay.status().buffered_bytes==0,"native disarm must stop owner and clear compressed history");
  }
  std::cout<<"Production native capture: two arm/ready/rolling/disarm cycles; real H264/AAC; off-air service gate; delayed loopback broadcast\n";
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
