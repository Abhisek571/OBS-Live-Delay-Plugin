#include "active-delay-output.hpp"

#include "ffmpeg-rtmp-connection.hpp"
#include "flv-muxer.hpp"
#include "rtmp-sender.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <obs-avc.h>
#include <util/bmem.h>
}

namespace active_delay {
namespace {
struct OutputData {
	OutputData(obs_output_t *output_, std::shared_ptr<ActiveDelaySession> session_)
		: output(output_), session(std::move(session_)), sender([] {
			return std::make_unique<FfmpegRtmpConnection>();
		})
	{
	}
	obs_output_t *output = nullptr;
	std::shared_ptr<ActiveDelaySession> session;
	std::mutex pipeline_mutex;
	std::unique_ptr<FlvMuxer> muxer;
	RtmpSender sender;
	std::atomic_bool active = false;
	std::atomic_bool capture_started = false;
	std::atomic_bool failure_signaled = false;
};

std::shared_ptr<ActiveDelaySession> registered_session;

const char *output_name(void *)
{
	return "OBS Active Live Delay (RTMP)";
}

void *output_create(obs_data_t *, obs_output_t *output)
{
	if (!registered_session)
		return nullptr;
	return new OutputData(output, registered_session);
}

void output_destroy(void *data)
{
	delete static_cast<OutputData *>(data);
}

std::string connect_info(obs_service_t *service, obs_service_connect_info type)
{
	const auto *value = obs_service_get_connect_info(service, static_cast<std::uint32_t>(type));
	return value ? value : "";
}

bool read_codec_headers(obs_output_t *output, FlvCodecHeaders &headers, std::string &error)
{
	auto *video_encoder = obs_output_get_video_encoder(output);
	auto *audio_encoder = obs_output_get_audio_encoder(output, 0);
	if (!video_encoder || !audio_encoder) {
		error = "The active-delay output requires one H.264 video encoder and one AAC audio encoder";
		return false;
	}

	std::uint8_t *extra_data = nullptr;
	std::size_t extra_size = 0;
	if (!obs_encoder_get_extra_data(video_encoder, &extra_data, &extra_size)) {
		error = "The H.264 encoder did not provide codec configuration";
		return false;
	}
	std::uint8_t *avc_header = nullptr;
	const auto avc_size = obs_parse_avc_header(&avc_header, extra_data, extra_size);
	if (!avc_header || avc_size == 0) {
		bfree(avc_header);
		error = "The H.264 encoder configuration could not be converted to AVC format";
		return false;
	}
	headers.avc_decoder_configuration.assign(avc_header, avc_header + avc_size);
	bfree(avc_header);

	extra_data = nullptr;
	extra_size = 0;
	if (!obs_encoder_get_extra_data(audio_encoder, &extra_data, &extra_size) || !extra_data || extra_size == 0) {
		error = "The AAC encoder did not provide AudioSpecificConfig";
		return false;
	}
	headers.aac_audio_specific_config.assign(extra_data, extra_data + extra_size);
	return true;
}

void signal_failure(OutputData *context, std::string error, int code)
{
	if (context->failure_signaled.exchange(true, std::memory_order_acq_rel))
		return;
	context->active.store(false, std::memory_order_release);
	context->session->controller.reset_for_discontinuity(error);
	obs_output_set_last_error(context->output, error.c_str());
	obs_output_signal_stop(context->output, code);
}

bool output_start(void *data)
{
	auto *context = static_cast<OutputData *>(data);
	blog(LOG_INFO, "[active-live-delay] Custom output start callback entered");
	context->sender.stop();
	const auto preserve_controller =
		context->session->preserve_controller_on_next_output_start.exchange(false, std::memory_order_acq_rel);
	if (!preserve_controller) {
		context->session->controller.return_live();
		context->session->controller.take_ready_packets();
	}
	context->failure_signaled.store(false, std::memory_order_release);
	if (!obs_output_can_begin_data_capture(context->output, 0)) {
		obs_output_set_last_error(context->output, "OBS reported that encoded capture cannot begin yet");
		blog(LOG_ERROR, "[active-live-delay] OBS refused encoded capture before encoder initialization");
		return false;
	}
	if (!obs_output_initialize_encoders(context->output, 0)) {
		const auto *encoder_error = obs_output_get_last_error(context->output);
		std::string message = "Unable to initialize the assigned H.264/AAC encoders";
		if (encoder_error && *encoder_error)
			message += std::string(": ") + encoder_error;
		obs_output_set_last_error(context->output, message.c_str());
		blog(LOG_ERROR, "[active-live-delay] %s", message.c_str());
		return false;
	}

	auto *service = obs_output_get_service(context->output);
	if (!service) {
		obs_output_set_last_error(context->output, "No streaming service is assigned to the active-delay output");
		return false;
	}

	FlvCodecHeaders headers;
	std::string error;
	if (preserve_controller) {
		std::scoped_lock lock(context->session->codec_headers_mutex);
		if (context->session->cached_codec_headers)
			headers = *context->session->cached_codec_headers;
		else
			error = "No cached H.264/AAC codec configuration is available for the delayed-output handoff";
	}
	if (!error.empty() || (!preserve_controller && !read_codec_headers(context->output, headers, error))) {
		obs_output_set_last_error(context->output, error.c_str());
		return false;
	}

	try {
		auto muxer = std::make_unique<FlvMuxer>(std::move(headers));
		RtmpTarget target;
		target.server_url = connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		target.stream_key = connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
		target.username = connect_info(service, OBS_SERVICE_CONNECT_INFO_USERNAME);
		target.password = connect_info(service, OBS_SERVICE_CONNECT_INFO_PASSWORD);
		if (!context->sender.start(std::move(target), muxer->sequence_headers(),
			[context](const std::string &sender_error) {
				signal_failure(context, sender_error, OBS_OUTPUT_DISCONNECTED);
			},
			error)) {
			obs_output_set_last_error(context->output, error.c_str());
			blog(LOG_ERROR, "[active-live-delay] RTMP sender startup failed: %s", error.c_str());
			return false;
		}
		{
			std::scoped_lock lock(context->pipeline_mutex);
			context->muxer = std::move(muxer);
		}
	} catch (const std::exception &exception) {
		error = std::string("Unable to initialize the FLV/RTMP pipeline: ") + exception.what();
		context->sender.stop();
		obs_output_set_last_error(context->output, error.c_str());
		return false;
	}

	context->active.store(true, std::memory_order_release);
	context->capture_started.store(true, std::memory_order_release);
	if (!obs_output_begin_data_capture(context->output, 0)) {
		context->active.store(false, std::memory_order_release);
		context->capture_started.store(false, std::memory_order_release);
		context->sender.stop();
		std::scoped_lock lock(context->pipeline_mutex);
		context->muxer.reset();
		obs_output_set_last_error(context->output, "OBS refused to begin encoded data capture");
		blog(LOG_ERROR, "[active-live-delay] OBS refused to begin encoded data capture after RTMP connected");
		return false;
	}
	if (preserve_controller)
		clear_cached_codec_headers(*context->session);
	blog(LOG_INFO, "[active-live-delay] Custom output is connected and capturing encoded packets");
	return true;
}

void output_stop(void *data, std::uint64_t)
{
	auto *context = static_cast<OutputData *>(data);
	context->active.store(false, std::memory_order_release);
	const auto was_capturing = context->capture_started.exchange(false, std::memory_order_acq_rel);
	context->sender.stop();
	{
		std::scoped_lock lock(context->pipeline_mutex);
		context->muxer.reset();
	}
	context->session->controller.return_live();
	if (was_capturing)
		obs_output_end_data_capture(context->output);
}

void output_packet(void *data, encoder_packet *packet)
{
	auto *context = static_cast<OutputData *>(data);
	if (!context->active.load(std::memory_order_acquire))
		return;
	if (!packet) {
		signal_failure(context, "The encoder stopped producing packets", OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	context->session->controller.ingest(copy_encoder_packet(*packet));
	const auto controller_status = context->session->controller.status();
	if (controller_status.state == DelayState::Error) {
		signal_failure(context, controller_status.error.empty() ? "The delay controller stopped" : controller_status.error,
			OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	auto ready = context->session->controller.take_ready_packets();
	if (ready.empty())
		return;

	try {
		std::vector<FlvTag> tags;
		std::string error;
		bool muxed = false;
		{
			std::scoped_lock lock(context->pipeline_mutex);
			if (!context->muxer)
				return;
			muxed = context->muxer->mux(std::move(ready), tags, error);
		}
		if (!muxed) {
			if (context->active.load(std::memory_order_acquire))
				signal_failure(context, std::move(error), OBS_OUTPUT_ENCODE_ERROR);
			return;
		}
		if (!context->sender.enqueue(std::move(tags), error) && context->active.load(std::memory_order_acquire))
			signal_failure(context, std::move(error), OBS_OUTPUT_DISCONNECTED);
	} catch (const std::exception &exception) {
		if (context->active.load(std::memory_order_acquire))
			signal_failure(context, std::string("FLV pipeline failed: ") + exception.what(), OBS_OUTPUT_ENCODE_ERROR);
	}
}

std::uint64_t output_total_bytes(void *data)
{
	return static_cast<OutputData *>(data)->sender.status().sent_bytes;
}

obs_output_info output_info = {
	.id = "active_delay_rtmp_output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
	.get_name = output_name,
	.create = output_create,
	.destroy = output_destroy,
	.start = output_start,
	.stop = output_stop,
	.encoded_packet = output_packet,
	.get_total_bytes = output_total_bytes,
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "aac",
	.protocols = "RTMP;RTMPS",
};
} // namespace

EncodedPacket copy_encoder_packet(const encoder_packet &packet)
{
	EncodedPacket copy;
	copy.kind = packet.type == OBS_ENCODER_VIDEO ? PacketKind::Video : PacketKind::Audio;
	copy.payload.assign(packet.data, packet.data + packet.size);
	copy.pts_us = packet.timebase_den != 0
		? (packet.pts * 1'000'000LL * packet.timebase_num) / packet.timebase_den
		: packet.dts_usec;
	copy.dts_us = packet.dts_usec;
	copy.keyframe = packet.keyframe;
	return copy;
}

bool cache_active_codec_headers(obs_output_t *output, ActiveDelaySession &session, std::string &error)
{
	FlvCodecHeaders headers;
	if (!read_codec_headers(output, headers, error))
		return false;
	std::scoped_lock lock(session.codec_headers_mutex);
	session.cached_codec_headers = std::move(headers);
	return true;
}

void clear_cached_codec_headers(ActiveDelaySession &session)
{
	std::scoped_lock lock(session.codec_headers_mutex);
	session.cached_codec_headers.reset();
}

void register_active_delay_output(std::shared_ptr<ActiveDelaySession> session)
{
	registered_session = std::move(session);
	obs_register_output(&output_info);
}

} // namespace active_delay
