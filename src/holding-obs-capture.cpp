#include "holding-obs-capture.hpp"
#include "holding-obs-audio.hpp"
#include "holding-obs-packet.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace active_delay {
namespace {
class ObsHoldingBackend final : public HoldingCaptureBackend {
public:
	explicit ObsHoldingBackend(ObsHoldingConfig config) : config_(std::move(config)) {}
	~ObsHoldingBackend() override { stop(); }
	bool prepare(HoldingFeed &feed, std::string &error) override;
	bool start(std::string &error) override;
	void stop() noexcept override;
	static void register_outputs() { register_output(); }
private:
	struct Endpoint { ObsHoldingBackend *owner; obs_output_t *output = nullptr; };
	static const char *output_name(void *) { return "Active Delay isolated holding capture"; }
	static void *output_create(obs_data_t *settings, obs_output_t *output)
	{
		auto *endpoint = reinterpret_cast<Endpoint *>(static_cast<intptr_t>(obs_data_get_int(settings, "holding_context")));
		if (endpoint) endpoint->output = output;
		return endpoint;
	}
	static void output_destroy(void *) {} // Backend lifetime spans output release.
	static bool output_start(void *param)
	{
		auto &endpoint = *static_cast<Endpoint *>(param);
		return obs_output_can_begin_data_capture(endpoint.output, 0) &&
			obs_output_initialize_encoders(endpoint.output, 0) && obs_output_begin_data_capture(endpoint.output, 0);
	}
	static void output_stop(void *param, uint64_t)
	{
		auto &endpoint = *static_cast<Endpoint *>(param);
		output_stopped(param, nullptr);
		// Also signals the stopping event for an inactive/partial start.
		obs_output_end_data_capture(endpoint.output);
	}
	static void output_stopped(void *param, calldata_t *) noexcept
	{
		auto &self = *static_cast<Endpoint *>(param)->owner;
		if (!self.capturing_.exchange(false, std::memory_order_acq_rel) || !self.feed_) return;
		try { self.feed_->fail("HOLDING_ENCODER_STOPPED: independent holding capture stopped"); } catch (...) {}
	}
	static void output_packet(void *param, encoder_packet *packet) noexcept;
	static void register_output();
	ObsCaptureCodecMonitor codecs_;
	bool create_endpoint(Endpoint &endpoint, const char *id, const char *name, bool video);
	ObsHoldingConfig config_;
	HoldingFeed *feed_ = nullptr;
	obs_source_t *scene_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr; // Borrowed from the owned view mix.
	HoldingSilentAudio silence_;
	obs_encoder_t *video_encoder_ = nullptr, *audio_encoder_ = nullptr;
	Endpoint video_output_{this}, audio_output_{this};
	std::atomic_bool capturing_ = false;
};
void ObsHoldingBackend::register_output()
{
	static std::once_flag registered;
	std::call_once(registered, [] {
		for (const bool video : {true, false}) {
			obs_output_info info{};
			info.id = video ? "active_delay_holding_video_capture" : "active_delay_holding_audio_capture";
			// Separate single-media capture outputs bypass libobs's AV
			// interleaver, which has no public byte/time cap when a peer stalls.
			info.flags = (video ? OBS_OUTPUT_VIDEO : OBS_OUTPUT_AUDIO) | OBS_OUTPUT_ENCODED;
			info.get_name = output_name;
			info.create = output_create; info.destroy = output_destroy;
			info.start = output_start; info.stop = output_stop;
			info.encoded_packet = output_packet;
			info.encoded_video_codecs = video ? "h264" : nullptr;
			info.encoded_audio_codecs = video ? nullptr : "aac";
			obs_register_output(&info);
		}
	});
}
bool ObsHoldingBackend::create_endpoint(Endpoint &endpoint, const char *id, const char *name, bool video)
{
	auto *settings = obs_data_create();
	// Ephemeral private capture settings, never persisted into OBS configuration.
	obs_data_set_int(settings, "holding_context", static_cast<int64_t>(reinterpret_cast<intptr_t>(&endpoint)));
	endpoint.output = obs_output_create(id, name, settings, nullptr);
	obs_data_release(settings);
	if (!endpoint.output) return false;
	if (video) obs_output_set_video_encoder(endpoint.output, video_encoder_);
	else obs_output_set_audio_encoder(endpoint.output, audio_encoder_, 0);
	signal_handler_connect(obs_output_get_signal_handler(endpoint.output), "stop", output_stopped, &endpoint);
	return true;
}
bool ObsHoldingBackend::prepare(HoldingFeed &feed, std::string &error)
{
	feed_ = &feed;
	if (config_.scene_name.empty()) {
		error = "HOLDING_SCENE_MISSING: select a holding scene";
		return false;
	}
	if (!obs_initialized()) {
		error = "HOLDING_HOST_UNAVAILABLE: OBS is not initialized";
		return false;
	}
	scene_ = obs_get_source_by_name(config_.scene_name.c_str());
	if (!scene_ || !obs_source_is_scene(scene_) || obs_source_removed(scene_)) {
		error = "HOLDING_SCENE_MISSING: selected holding scene is unavailable";
		return false;
	}
	const auto *video_codec = obs_get_encoder_codec(config_.video_encoder_id.c_str());
	const auto *audio_codec = obs_get_encoder_codec(config_.audio_encoder_id.c_str());
	if (!video_codec || !audio_codec || std::strcmp(video_codec, "h264") || std::strcmp(audio_codec, "aac")) {
		error = "HOLDING_ENCODER_UNAVAILABLE: registered independent H.264/AAC encoders required";
		return false;
	}
	obs_video_info video_info{};
	if (!obs_get_video_info(&video_info)) {
		error = "HOLDING_VIDEO_UNAVAILABLE: OBS video is not initialized";
		return false;
	}
	view_ = obs_view_create();
	if (!view_) { error = "HOLDING_VIEW_FAILED: isolated view unavailable"; return false; }
	obs_view_set_source(view_, 0, scene_); // AUX_VIEW activates video, not programme audio.
	video_ = obs_view_add2(view_, &video_info);
	if (!video_) { error = "HOLDING_VIEW_FAILED: isolated video mix unavailable"; return false; }
	if (!silence_.open(config_.sample_rate, config_.speakers, error)) return false;
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", config_.video_bitrate_kbps);
	obs_data_set_int(settings, "keyint_sec", 1);
	video_encoder_ = obs_video_encoder_create(config_.video_encoder_id.c_str(), "Active Delay holding H.264", settings, nullptr);
	obs_data_release(settings);
	settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", config_.audio_bitrate_kbps);
	audio_encoder_ = obs_audio_encoder_create(config_.audio_encoder_id.c_str(), "Active Delay holding AAC", settings, 0, nullptr);
	obs_data_release(settings);
	const char *vcodec = video_encoder_ ? obs_encoder_get_codec(video_encoder_) : nullptr;
	const char *acodec = audio_encoder_ ? obs_encoder_get_codec(audio_encoder_) : nullptr;
	if (!vcodec || !acodec || std::strcmp(vcodec, "h264") || std::strcmp(acodec, "aac")) {
		error = "HOLDING_ENCODER_UNAVAILABLE: independent H.264/AAC encoders unavailable";
		return false;
	}
	obs_encoder_set_video(video_encoder_, video_);
	obs_encoder_set_audio(audio_encoder_, silence_.get());
	register_output();
	if (!create_endpoint(video_output_, "active_delay_holding_video_capture", "Active Delay holding video", true) ||
		!create_endpoint(audio_output_, "active_delay_holding_audio_capture", "Active Delay holding audio", false)) {
		error = "HOLDING_OUTPUT_FAILED: private single-media encoded capture unavailable";
		return false;
	}
	return true;
}
bool ObsHoldingBackend::start(std::string &error)
{
	if (!video_output_.output || !audio_output_.output || !scene_ || obs_source_removed(scene_) ||
		!obs_output_initialize_encoders(video_output_.output, 0) || !obs_output_initialize_encoders(audio_output_.output, 0)) {
		error = "HOLDING_START_FAILED: independent holding encoders could not initialize";
		return false;
	}
	codecs_.reset();
	capturing_.store(true, std::memory_order_release);
	if (!obs_output_start(video_output_.output) || !obs_output_start(audio_output_.output)) {
		error = "HOLDING_START_FAILED: holding scene or independent encoder could not start";
		return false;
	}
	return true;
}
void ObsHoldingBackend::output_packet(void *param, encoder_packet *packet) noexcept
{
	auto &self = *static_cast<Endpoint *>(param)->owner;
	if (!self.capturing_.load(std::memory_order_acquire) || !self.feed_) return;
	try {
		if (!packet || !self.scene_ || obs_source_removed(self.scene_)) {
			self.feed_->fail("HOLDING_ENCODER_STOPPED: scene or encoder stopped producing holding media");
			return;
		}
		if (!packet->data || !packet->size || packet->size > HoldingFeed::queue_byte_limit) {
			self.feed_->fail("HOLDING_PACKET_INVALID: missing or oversized holding packet");
			return;
		}
		const auto state = self.feed_->status().state;
		if (state == HoldingState::Failed || state == HoldingState::Stopped) return;
		auto monitored=self.codecs_.observe(*packet,packet->type==OBS_ENCODER_VIDEO ? self.video_encoder_ : self.audio_encoder_);
		if(monitored.headers)self.feed_->set_headers(std::move(*monitored.headers));
		self.feed_->ingest(std::move(monitored.packet));
	} catch (...) {
		// No exception or owner callback may cross libobs's encoder boundary.
		try { self.feed_->fail("HOLDING_PACKET_FAILED: holding packet processing failed"); } catch (...) {}
	}
}
void ObsHoldingBackend::stop() noexcept
{
	// end_data_capture unregisters encoder callbacks. Release the output (and
	// its end-capture worker) before encoders, owned audio, view and scene.
	capturing_.store(false, std::memory_order_release);
	// Request both stops before waiting for either native teardown worker.
	for (auto *endpoint : {&video_output_, &audio_output_})
		if (endpoint->output && obs_output_active(endpoint->output)) obs_output_force_stop(endpoint->output);
	for (auto *endpoint : {&video_output_, &audio_output_}) {
		if (endpoint->output) obs_output_release(endpoint->output);
		endpoint->output = nullptr;
	}
	if (video_encoder_) obs_encoder_release(video_encoder_);
	if (audio_encoder_) obs_encoder_release(audio_encoder_);
	video_encoder_ = audio_encoder_ = nullptr;
	silence_.close();
	if (view_) { obs_view_remove(view_); obs_view_destroy(view_); }
	view_ = nullptr; video_ = nullptr;
	if (scene_) obs_source_release(scene_);
	scene_ = nullptr;
	feed_ = nullptr;
}
} // namespace
void register_obs_holding_outputs()
{
	ObsHoldingBackend::register_outputs();
}
std::unique_ptr<HoldingCapture> make_obs_holding_capture(ObsHoldingConfig config)
{
	return std::make_unique<HoldingCapture>(std::make_unique<ObsHoldingBackend>(std::move(config)));
}
} // namespace active_delay