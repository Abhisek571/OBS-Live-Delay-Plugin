#include "active-delay-output.hpp"
#include "diagnostic-error.hpp"
#include "ffmpeg-rtmp-connection.hpp"
#include "holding-obs-capture.hpp"
#include "holding-obs-packet.hpp"
#include "network-packet-consumer.hpp"
#include "obs-packet-copy.hpp"
#include <atomic>
#include <memory>
#include <mutex>
namespace active_delay {
namespace {
struct OutputData {
 obs_output_t *output;
 std::shared_ptr<ActiveDelaySession> session;
 std::shared_ptr<PipelineOwner> owner;
 obs_output_t *audio_output=nullptr;
 obs_encoder_t *audio_encoder=nullptr;
 obs_service_t *service=nullptr;
 std::mutex mutex;
 // The owner's finish callback and output_stop run on different threads; libobs
 // does not support concurrent stops of the same output.
 std::mutex audio_stop_mutex;
 std::shared_ptr<ReleasedPacketConsumer> consumer;
 std::atomic_bool active=false;
 ObsCaptureCodecMonitor codecs;
 std::shared_ptr<std::atomic_uint64_t> generation=std::make_shared<std::atomic_uint64_t>(0);
 OutputData(obs_output_t *o,std::shared_ptr<ActiveDelaySession> s):output(o),session(std::move(s)){}
};
std::shared_ptr<ActiveDelaySession> registered_session;
const char *output_name(void *) {return "OBS Active Live Delay (RTMP)";}
void *output_create(obs_data_t *settings,obs_output_t *output) {
 if(!registered_session)return nullptr;
 auto *context=new OutputData(output,registered_session);
 context->audio_encoder=obs_encoder_get_ref(reinterpret_cast<obs_encoder_t *>(static_cast<intptr_t>(obs_data_get_int(settings,"programme_audio"))));
 context->service=obs_service_get_ref(reinterpret_cast<obs_service_t *>(static_cast<intptr_t>(obs_data_get_int(settings,"programme_service"))));
 obs_data_erase(settings,"programme_audio");obs_data_erase(settings,"programme_service");
 return context;
}
void output_destroy(void *data) {
 auto *context=static_cast<OutputData *>(data);
 context->active=false;
 if(context->owner) context->owner->stop(); // always external to owner/encoder/sender workers
 if(context->audio_output){
  if(obs_output_active(context->audio_output))obs_output_force_stop(context->audio_output);
  obs_output_release(context->audio_output);
 }
 obs_encoder_release(context->audio_encoder);obs_service_release(context->service);
 context->session->end_consumer_lifecycle();
 delete context;
}
std::string connect_info(obs_service_t *service,obs_service_connect_info type) {
 const auto *value=obs_service_get_connect_info(service,static_cast<uint32_t>(type));return value ? value : "";
}
// OBS stop signals synchronously invoke listeners. Never signal from sender,
// encoder, or owner workers: a listener can release the output and join it.
struct FinishNotice { std::shared_ptr<obs_weak_output_t> output; std::string error;
 std::shared_ptr<std::atomic_uint64_t> generation; uint64_t value; };
void finish_on_ui(void *data) {
 std::unique_ptr<FinishNotice> notice(static_cast<FinishNotice *>(data));
 if(notice->generation->load()!=notice->value) return;
 auto *output=obs_weak_output_get_output(notice->output.get());
 if(!output) return;
 if(!notice->error.empty()) {
  obs_output_set_last_error(output,notice->error.c_str());
  obs_output_signal_stop(output,OBS_OUTPUT_ERROR);
 } else obs_output_end_data_capture(output);
 obs_output_release(output);
}
bool output_start(void *data) {
 auto *context=static_cast<OutputData *>(data);
 std::string error;
 const auto mode=context->session->operating_mode();
 if((mode!=OperatingMode::DirectSingle && mode!=OperatingMode::NativeMultistream) ||
    !context->session->begin_consumer_lifecycle(mode,error)) return false;
 if(context->owner) context->owner->stop();
 const auto prebuffer_target=context->session->prebuffer_target();
 if(prebuffer_target<=Microseconds{}) {
  obs_output_set_last_error(context->output,"PREBUFFER_TARGET_INVALID: arm a positive delay before broadcasting");
  context->session->end_consumer_lifecycle();return false;
 }
 context->codecs.reset();
 if(!obs_output_can_begin_data_capture(context->output,0) || !obs_output_initialize_encoders(context->output,0)) {
  context->session->end_consumer_lifecycle(); return false;
 }
 auto *service=context->service;
 if(!service) {context->session->end_consumer_lifecycle();return false;}
 if(context->audio_output){obs_output_release(context->audio_output);context->audio_output=nullptr;}
 auto *capture_settings=obs_data_create();
 obs_data_set_int(capture_settings,"programme_context",static_cast<int64_t>(reinterpret_cast<intptr_t>(context)));
 context->audio_output=obs_output_create("active_delay_programme_audio_capture","Active Delay programme audio",capture_settings,nullptr);
 obs_data_release(capture_settings);
 if(!context->audio_output || !context->audio_encoder){context->session->end_consumer_lifecycle();return false;}
 obs_output_set_audio_encoder(context->audio_output,context->audio_encoder,0);
 ObsHoldingConfig config;
 config.scene_name=context->session->holding_scene();
 obs_audio_info audio{};
 if(obs_get_audio_info(&audio)){config.sample_rate=audio.samples_per_sec;config.speakers=audio.speakers;}
 auto weak=std::shared_ptr<obs_weak_output_t>(obs_output_get_weak_output(context->output),obs_weak_output_release);
 const auto epoch=context->session->begin_packet_epoch();
 const auto generation=context->generation;
 const auto value=++*generation;
 context->owner=std::make_shared<PipelineOwner>(context->session->controller.delay,context->session->consumers.dispatcher,epoch,
  [config]{return make_obs_holding_capture(config);},
  [context,service,mode](FlvCodecHeaders headers,SenderErrorCallback failure,std::string &error)->std::shared_ptr<ReleasedPacketConsumer>{
   // No credentials read or network consumer constructed until explicit Start.
   RtmpTarget target;
   target.server_url=connect_info(service,OBS_SERVICE_CONNECT_INFO_SERVER_URL);
   target.stream_key=connect_info(service,OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
   target.username=connect_info(service,OBS_SERVICE_CONNECT_INFO_USERNAME);
   target.password=connect_info(service,OBS_SERVICE_CONNECT_INFO_PASSWORD);
   std::shared_ptr<ReleasedPacketConsumer> consumer;
   if(mode==OperatingMode::NativeMultistream){
    auto multi=std::make_shared<MultiTargetSender>([]{return std::make_unique<FfmpegRtmpConnection>();},SenderConfig{});
    if(!multi->start(target,"Primary OBS service",context->session->multistream.snapshot(),std::move(headers),std::move(failure),error))return {};
    consumer=multi;
   }else{
    auto single=std::make_shared<NetworkPacketConsumer>([]{return std::make_unique<FfmpegRtmpConnection>();},SenderConfig{});
    if(!single->start(target,std::move(headers),std::move(failure),error))return {};
    consumer=single;
   }
   {std::scoped_lock lock(context->mutex);context->consumer=consumer;}
   return consumer;
  },[context,weak,generation,value,session=context->session](const std::string &error){
   context->active=false;
   {std::scoped_lock lock(context->audio_stop_mutex);
    if(context->audio_output && obs_output_active(context->audio_output))obs_output_force_stop(context->audio_output);}
   session->multistream.set_status({});session->end_consumer_lifecycle();
   if(!error.empty()) obs_queue_task(OBS_TASK_UI,finish_on_ui,new FinishNotice{weak,error,generation,value},false);
  },prebuffer_target,[](const std::shared_ptr<ReleasedPacketConsumer> &consumer){
   if(auto single=std::dynamic_pointer_cast<NetworkPacketConsumer>(consumer))
    return single->status().state==SenderState::Running;
   if(auto multi=std::dynamic_pointer_cast<MultiTargetSender>(consumer)) {
    const auto status=multi->status();bool primary=false;
    for(const auto &destination:status.destinations){
     if(destination.primary)primary=destination.sender.state==SenderState::Running;
     if(destination.sender.state==SenderState::Starting || destination.sender.state==SenderState::Reconnecting)return false;
    }
    return primary;
   }
   return false;
  });
 context->session->attach_pipeline(context->owner);
 context->active=true;
 // RTMP owns reconnection; libobs must not restart a stale owner behind it.
 obs_output_set_reconnect_settings(context->output,0,0);
 try{context->owner->start();}catch(const std::exception &e){
  context->active=false;obs_output_set_last_error(context->output,e.what());obs_output_end_data_capture(context->output);
  context->session->end_consumer_lifecycle();return false;
 }
 if(!obs_output_start(context->audio_output) || !obs_output_begin_data_capture(context->output,0)) {
  context->active=false;context->owner->request_stop();return false;
 }
 return true;
}
void output_stop(void *data,uint64_t) {
 auto *context=static_cast<OutputData *>(data);
 context->active=false;
 {std::scoped_lock lock(context->audio_stop_mutex);
  if(context->audio_output && obs_output_active(context->audio_output))obs_output_force_stop(context->audio_output);}
 if(context->owner) context->owner->request_stop();
 obs_output_end_data_capture(context->output);
 // Owner teardown finishes asynchronously; no joins on the UI/capture callback.
}
void output_packet(void *data,encoder_packet *packet) {
 auto *context=static_cast<OutputData *>(data);
 if(!context->active.load() || !context->owner)return;
 if(!packet){context->owner->failure("PROGRAMME_ENCODER_STOPPED");return;}
 try{
  const auto token=context->owner->capture_token();
  // Sample the calling encoder only; validate snapshots and in-band AVC
  // parameter sets before this packet can enter rolling programme history.
  auto monitored=context->codecs.observe(*packet,packet->type==OBS_ENCODER_VIDEO ? obs_output_get_video_encoder(context->output) : context->audio_encoder);
  if(monitored.headers)context->owner->headers(std::move(*monitored.headers));
  // Single-media native outputs bypass the unbounded libobs AV interleaver.
  (void)context->owner->enqueue_uninterleaved(std::move(monitored.packet),token);
 }catch(const std::exception &e){context->owner->failure(e.what());}
}
uint64_t output_total_bytes(void *data) {
 auto *context=static_cast<OutputData *>(data);
 std::shared_ptr<ReleasedPacketConsumer> consumer;
 {std::scoped_lock lock(context->mutex);consumer=context->consumer;}
 if(auto multi=std::dynamic_pointer_cast<MultiTargetSender>(consumer)){
  const auto status=multi->status();context->session->multistream.set_status(status);return status.sent_bytes;
 }
 if(auto single=std::dynamic_pointer_cast<NetworkPacketConsumer>(consumer))return single->status().sent_bytes;
 return 0;
}
obs_output_info output_info={
 .id="active_delay_rtmp_output",.flags=OBS_OUTPUT_VIDEO|OBS_OUTPUT_ENCODED,
 .get_name=output_name,.create=output_create,.destroy=output_destroy,.start=output_start,.stop=output_stop,
 .encoded_packet=output_packet,.get_total_bytes=output_total_bytes,
 .encoded_video_codecs="h264",
};
void *audio_create(obs_data_t *settings,obs_output_t *) {
 return reinterpret_cast<OutputData *>(static_cast<intptr_t>(obs_data_get_int(settings,"programme_context")));
}
bool audio_start(void *data) {
 auto *context=static_cast<OutputData *>(data);
 return obs_output_can_begin_data_capture(context->audio_output,0) &&
  obs_output_initialize_encoders(context->audio_output,0) && obs_output_begin_data_capture(context->audio_output,0);
}
void audio_stop(void *data,uint64_t) {
 auto *context=static_cast<OutputData *>(data);
 if(context->active && context->owner)context->owner->failure("PROGRAMME_AUDIO_STOPPED");
 obs_output_end_data_capture(context->audio_output);
}
obs_output_info audio_info={
 .id="active_delay_programme_audio_capture",.flags=OBS_OUTPUT_AUDIO|OBS_OUTPUT_ENCODED,
 .get_name=output_name,.create=audio_create,.destroy=[](void *){},.start=audio_start,.stop=audio_stop,
 .encoded_packet=output_packet,.encoded_audio_codecs="aac",
};
}
void register_active_delay_output(std::shared_ptr<ActiveDelaySession> session){registered_session=std::move(session);obs_register_output(&audio_info);obs_register_output(&output_info);}
} // namespace active_delay
