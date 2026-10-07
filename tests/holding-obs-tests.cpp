#include <obs.h>
#include <media-io/audio-io.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "holding-obs-audio.hpp"
#include "holding-obs-capture.hpp"
#include "holding-obs-packet.hpp"
#include "transition-test-headers.hpp"
void check(bool v, const char *s);
void codec_monitor_rejects_native_changes()
{
 using namespace active_delay;
 const auto headers=programme_test_headers();
 const auto &avc=headers.avc_decoder_configuration;
 const size_t sps_size=(size_t(avc[6])<<8)|avc[7],pps=8+sps_size;
 const size_t pps_size=(size_t(avc[pps+1])<<8)|avc[pps+2];
 std::vector<uint8_t> parameter_sets{0,0,0,1};
 parameter_sets.insert(parameter_sets.end(),avc.begin()+8,avc.begin()+8+sps_size);
 parameter_sets.insert(parameter_sets.end(),{0,0,1});
 parameter_sets.insert(parameter_sets.end(),avc.begin()+pps+3,avc.begin()+pps+3+pps_size);
 std::vector<uint8_t> idr{0,0,0,1,0x65,0x88,0x84};
 auto make=[](std::vector<uint8_t> &data,bool audio){encoder_packet p{};p.type=audio ? OBS_ENCODER_AUDIO : OBS_ENCODER_VIDEO;p.data=data.data();p.size=data.size();p.timebase_num=1;p.timebase_den=1000000;p.sys_dts_usec=1000000;return p;};
 for(int scenario=0;scenario<5;++scenario){
  ObsCaptureCodecMonitor monitor;
  auto video=make(idr,false);auto audio_data=std::vector<uint8_t>{0,0,0,1,0x67,0xff};auto audio=make(audio_data,true);
  (void)monitor.observe(video,avc);
  auto first=monitor.observe(audio,headers.aac_audio_specific_config);
  check(first.headers.has_value(),"both native codec snapshots must produce headers");
  (void)monitor.observe(video,avc);(void)monitor.observe(audio,headers.aac_audio_specific_config);
  auto repeat=make(parameter_sets,false);(void)monitor.observe(repeat,avc);
  bool rejected=false;
  try {
   if(scenario==0){auto changed=headers.aac_audio_specific_config;changed[0]^=1;(void)monitor.observe(audio,changed);}
   if(scenario==1){auto changed=avc;changed.back()^=1;(void)monitor.observe(video,changed);}
   if(scenario==2 || scenario==3){auto changed=parameter_sets;changed[scenario==2 ? 6 : changed.size()-1]^=1;auto inband=make(changed,false);(void)monitor.observe(inband,avc);}
   if(scenario==4)(void)monitor.observe(video,std::span<const uint8_t>{});
  }catch(const std::exception &){rejected=true;}
  check(rejected,"native snapshot/in-band change must fail before affected packet admission, even with stale extradata");
 }
}
void check(bool v, const char *s) { if (!v) throw std::runtime_error(s); }
struct ObservedAudio {
	std::mutex mutex;
	std::condition_variable wake;
	unsigned blocks = 0;
	bool zero = true, timing = true;
	uint64_t previous = 0;
	static void receive(void *param, size_t, audio_data *data)
	{
		auto &o = *static_cast<ObservedAudio *>(param);
		std::scoped_lock lock(o.mutex);
		for (unsigned ch = 0; ch < 2; ++ch) {
			const auto *p = reinterpret_cast<const float *>(data->data[ch]);
			for (unsigned i = 0; i < data->frames; ++i) o.zero &= p[i] == 0.0f;
		}
		if (o.previous) {
			const auto delta = data->timestamp - o.previous;
			const auto expected = audio_frames_to_ns(48000, data->frames);
			o.timing &= delta >= expected - 1 && delta <= expected + 1;
		}
		o.previous = data->timestamp; ++o.blocks; o.wake.notify_all();
	}
};
int main()
{
	try {

		using namespace active_delay;

		codec_monitor_rejects_native_changes();
		uint8_t annex_b[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
		encoder_packet raw{};
		raw.type = OBS_ENCODER_VIDEO; raw.data = annex_b; raw.size = sizeof(annex_b);
		raw.pts = 90'900; raw.dts = 90'000; raw.timebase_num = 1; raw.timebase_den = 90'000;
		raw.dts_usec = 1'000'000; raw.sys_dts_usec = 1'000'200;
		const auto aligned = copy_holding_encoder_packet(raw);
		check(aligned.dts_us == 1'000'200 && aligned.pts_us == 1'010'200 && aligned.keyframe,
			"uninterleaved holding packet did not preserve system-clock DTS/composition offset");

		for (int scenario=0;scenario<4;++scenario) {
			auto invalid=raw;
			invalid.pts=invalid.dts+(scenario==0 ? 27000 : scenario==1 ? -1 : 1);
			if(scenario>=2)invalid.type=OBS_ENCODER_AUDIO;
			if(scenario==3)invalid.timebase_den=2000000; // sub-microsecond mismatch must not round to equality
			bool rejected=false;
			try{(void)copy_holding_encoder_packet(invalid);}catch(const std::exception &){rejected=true;}
			check(rejected,"native ingress must reject unsupported CTS/audio timing before rounding or owner admission");
		}

		check(obs_startup("en-US", nullptr, nullptr), "isolated libobs test core did not initialize");
		struct CoreGuard { ~CoreGuard() { obs_shutdown(); } } core_guard;
		HoldingSilentAudio audio;
		std::string error;
		check(audio.open(48000, SPEAKERS_STEREO, error), "silent audio output failed to open");
		ObservedAudio observed;
		check(audio_output_connect(audio.get(), 0, nullptr, ObservedAudio::receive, &observed), "audio connect failed");
		{
			std::unique_lock lock(observed.mutex);
			check(observed.wake.wait_for(lock, std::chrono::seconds(2), [&] { return observed.blocks >= 16; }), "timed silence not produced");
			check(observed.zero && observed.timing, "silent PCM samples or sample clock invalid");
		}
		audio_output_disconnect(audio.get(), 0, ObservedAudio::receive, &observed);
		audio.close(); audio.close();
		unsigned stopped_count;
		{ std::scoped_lock lock(observed.mutex); stopped_count = observed.blocks; }
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		{ std::scoped_lock lock(observed.mutex); check(stopped_count == observed.blocks, "callbacks survived close"); }
		for (int i = 0; i < 20; ++i) { check(audio.open(48000, SPEAKERS_STEREO, error), "repeat audio open failed"); audio.close(); }
		check(!audio.open(0, SPEAKERS_STEREO, error) && !audio.get(), "invalid audio rate accepted");
		auto capture = make_obs_holding_capture({});
		check(!capture->prepare(error) && error.find("HOLDING_SCENE_MISSING") != std::string::npos,
			"missing scene did not fail closed before accessing the host");
		capture->stop(); capture->stop();
		ObsHoldingConfig missing_config; missing_config.scene_name = "No such holding scene";
		auto missing = make_obs_holding_capture(missing_config);
		check(!missing->prepare(error) && error.find("HOLDING_SCENE_MISSING") != std::string::npos, "named missing scene accepted");
		auto *scene = obs_scene_create("Holding ownership test");
		check(scene, "synthetic scene create failed");
		auto *source = obs_scene_get_source(scene);
		auto *weak = obs_source_get_weak_source(source);
		{
			auto *view = obs_view_create();
			check(view, "independent view creation failed");
			obs_view_set_source(view, 0, source);
			auto *selected = obs_view_get_source(view, 0);
			check(selected == source && !obs_source_active(source), "auxiliary view activated programme audio");
			obs_source_release(selected);
			check(!obs_get_output_source(0), "auxiliary view modified programme output channel");
			obs_view_destroy(view);
			ObsHoldingConfig bad_encoder;
			bad_encoder.scene_name = "Holding ownership test";
			bad_encoder.video_encoder_id = "missing_h264_encoder";
			auto unavailable = make_obs_holding_capture(bad_encoder);
			check(!unavailable->prepare(error) && error.find("HOLDING_ENCODER_UNAVAILABLE") != std::string::npos,
				"missing independent encoder did not fail closed");
			unavailable->stop();
		}
		// OBS 32.2's main canvas also owns public scenes until removal.
		obs_source_remove(source);
		obs_scene_release(scene);
		check(obs_weak_source_expired(weak), "partial adapter preparation retained scene ownership");
		obs_weak_source_release(weak);

		std::cout << "Holding OBS-linked tests passed (no host graphics)\n";
	} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}