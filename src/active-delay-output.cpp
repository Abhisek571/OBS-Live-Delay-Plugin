#include "active-delay-output.hpp"

#include "diagnostic-error.hpp"
#include "ffmpeg-rtmp-connection.hpp"
#include "multi-target-sender.hpp"
#include "network-packet-consumer.hpp"
#include "obs-packet-copy.hpp"

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
constexpr unsigned int kCodecHeaderRetryVideoFrames = 120;

struct OutputData {
	OutputData(obs_output_t *output_, std::shared_ptr<ActiveDelaySession> session_)
		: output(output_), session(std::move(session_))
	{
	}
	obs_output_t *output = nullptr;
	std::shared_ptr<ActiveDelaySession> session;
	std::mutex pipeline_mutex;
	std::shared_ptr<ReleasedPacketConsumer> consumer;
	std::shared_ptr<MultiTargetSender> multi_target_sender;
	ReleasedPacketDispatcher::ConsumerId consumer_id = 0;
	RtmpTarget target;
	std::uint64_t packet_epoch = 0;
	std::atomic_bool active = false;
	std::atomic_bool capture_started = false;
	std::atomic_bool failure_signaled = false;
	std::atomic_bool pipeline_ready = false;
	std::atomic_uint codec_header_wait_frames = 0;
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
	auto *context = static_cast<OutputData *>(data);
	context->session->consumers.dispatcher.remove_consumer(context->consumer_id);
	context->session->end_consumer_lifecycle();
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
		error = diagnostic_error(DiagnosticCode::OutputCodecHeadersUnavailable,
			"The active-delay output requires one H.264 video encoder and one AAC audio encoder");
		return false;
	}

	std::uint8_t *extra_data = nullptr;
	std::size_t extra_size = 0;
	if (!obs_encoder_get_extra_data(video_encoder, &extra_data, &extra_size)) {
		error = diagnostic_error(DiagnosticCode::OutputCodecHeadersUnavailable,
			"The H.264 encoder did not provide codec configuration");
		return false;
	}
	std::uint8_t *avc_header = nullptr;
	const auto avc_size = obs_parse_avc_header(&avc_header, extra_data, extra_size);
	if (!avc_header || avc_size == 0) {
		bfree(avc_header);
		error = diagnostic_error(DiagnosticCode::OutputCodecHeadersUnavailable,
			"The H.264 encoder configuration could not be converted to AVC format");
		return false;
	}
	headers.avc_decoder_configuration.assign(avc_header, avc_header + avc_size);
	bfree(avc_header);

	extra_data = nullptr;
	extra_size = 0;
	if (!obs_encoder_get_extra_data(audio_encoder, &extra_data, &extra_size) || !extra_data || extra_size == 0) {
		error = diagnostic_error(DiagnosticCode::OutputCodecHeadersUnavailable,
			"The AAC encoder did not provide AudioSpecificConfig");
		return false;
	}
	headers.aac_audio_specific_config.assign(extra_data, extra_data + extra_size);
	return true;
}

void signal_failure(OutputData *context, DiagnosticCode diagnostic, std::string error, int output_code)
{
	if (context->failure_signaled.exchange(true, std::memory_order_acq_rel))
		return;
	error = diagnostic_error(diagnostic, error);
	context->active.store(false, std::memory_order_release);
	context->session->controller.delay.reset_for_discontinuity(error);
	const auto discontinuity_failures = context->session->consumers.dispatcher.dispatch_discontinuity({context->packet_epoch, error});
	if (!discontinuity_failures.empty())
		blog(LOG_WARNING, "[active-live-delay] %s",
			diagnostic_error(DiagnosticCode::PacketDiscontinuityNotificationFailed,
				"Consumer discontinuity notification failed: " + discontinuity_failures.front().error).c_str());
	obs_output_set_last_error(context->output, error.c_str());
	obs_output_signal_stop(context->output, output_code);
}

bool start_pipeline(OutputData *context, FlvCodecHeaders headers, std::string &error)
{
	try {
		const auto mode = context->session->operating_mode();
		std::shared_ptr<ReleasedPacketConsumer> consumer;
		std::shared_ptr<MultiTargetSender> multi_target_sender;
		if (mode == OperatingMode::NativeMultistream) {
			multi_target_sender = std::make_shared<MultiTargetSender>([] {
				return std::make_unique<FfmpegRtmpConnection>();
			});
			if (!multi_target_sender->start(context->target, "Primary OBS service", context->session->multistream.snapshot(),
				std::move(headers),
				[context](const std::string &sender_error) {
					signal_failure(context, DiagnosticCode::RtmpConnectionFailed, sender_error, OBS_OUTPUT_DISCONNECTED);
				}, error)) {
				error = diagnostic_error(DiagnosticCode::MultiTargetStartupFailed, error);
				blog(LOG_ERROR, "[active-live-delay] Native multistream startup failed: %s", error.c_str());
				return false;
			}
			consumer = multi_target_sender;
			context->session->multistream.set_status(multi_target_sender->status());
		} else {
			auto network_consumer = std::make_shared<NetworkPacketConsumer>([] {
				return std::make_unique<FfmpegRtmpConnection>();
			});
			if (!network_consumer->start(context->target, std::move(headers),
				[context](const std::string &sender_error) {
					signal_failure(context, DiagnosticCode::RtmpConnectionFailed, sender_error, OBS_OUTPUT_DISCONNECTED);
				}, error)) {
				error = diagnostic_error(DiagnosticCode::RtmpSenderStartupFailed, error);
				blog(LOG_ERROR, "[active-live-delay] RTMP sender startup failed: %s", error.c_str());
				return false;
			}
			consumer = network_consumer;
		}
		{
			std::scoped_lock lock(context->pipeline_mutex);
			context->consumer = std::move(consumer);
			context->multi_target_sender = std::move(multi_target_sender);
			context->consumer_id = context->session->consumers.dispatcher.add_consumer(context->consumer);
		}
		context->pipeline_ready.store(true, std::memory_order_release);
		return true;
	} catch (const std::exception &exception) {
		error = diagnostic_error(DiagnosticCode::NetworkConsumerStartupFailed,
			std::string("Unable to initialize the FLV/RTMP pipeline: ") + exception.what());
		return false;
	}
}

bool output_start(void *data)
{
	auto *context = static_cast<OutputData *>(data);
	blog(LOG_INFO, "[active-live-delay] Custom output start callback entered");
	std::string lifecycle_error;
	const auto mode = context->session->operating_mode();
	if ((mode != OperatingMode::DirectSingle && mode != OperatingMode::NativeMultistream) ||
		!context->session->begin_consumer_lifecycle(mode, lifecycle_error)) {
		const auto error = diagnostic_error(DiagnosticCode::OperatingModeConflict, lifecycle_error);
		obs_output_set_last_error(context->output, error.c_str());
		return false;
	}
	context->session->consumers.dispatcher.remove_consumer(context->consumer_id);
	context->consumer_id = 0;
	context->consumer.reset();
	context->multi_target_sender.reset();
	const auto preserve_controller =
		context->session->controller.preserve_on_next_output_start.exchange(false, std::memory_order_acq_rel);
	if (!preserve_controller) {
		context->session->controller.delay.return_live();
		context->session->controller.delay.take_ready_packets();
	}
	context->failure_signaled.store(false, std::memory_order_release);
	context->pipeline_ready.store(false, std::memory_order_release);
	context->codec_header_wait_frames.store(0, std::memory_order_release);
	{
		std::scoped_lock lock(context->pipeline_mutex);
		context->consumer.reset();
		context->multi_target_sender.reset();
	}
	if (!obs_output_can_begin_data_capture(context->output, 0)) {
		context->session->end_consumer_lifecycle();
		const auto error = diagnostic_error(DiagnosticCode::OutputCaptureNotReady,
			"OBS reported that encoded capture cannot begin yet");
		obs_output_set_last_error(context->output, error.c_str());
		blog(LOG_ERROR, "[active-live-delay] %s", error.c_str());
		return false;
	}
	if (!obs_output_initialize_encoders(context->output, 0)) {
		context->session->end_consumer_lifecycle();
		const auto *encoder_error = obs_output_get_last_error(context->output);
		std::string message = "Unable to initialize the assigned H.264/AAC encoders";
		if (encoder_error && *encoder_error)
			message += std::string(": ") + encoder_error;
		message = diagnostic_error(DiagnosticCode::OutputEncoderInitializationFailed, message);
		obs_output_set_last_error(context->output, message.c_str());
		blog(LOG_ERROR, "[active-live-delay] %s", message.c_str());
		return false;
	}

	auto *service = obs_output_get_service(context->output);
	if (!service) {
		context->session->end_consumer_lifecycle();
		const auto error = diagnostic_error(DiagnosticCode::OutputServiceMissing,
			"No streaming service is assigned to the active-delay output");
		obs_output_set_last_error(context->output, error.c_str());
		return false;
	}

	context->target.server_url = connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	context->target.stream_key = connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	context->target.username = connect_info(service, OBS_SERVICE_CONNECT_INFO_USERNAME);
	context->target.password = connect_info(service, OBS_SERVICE_CONNECT_INFO_PASSWORD);

	std::string error;
	if (preserve_controller) {
		FlvCodecHeaders headers;
		{
			std::scoped_lock lock(context->session->codec.mutex);
			if (context->session->codec.cached_headers)
				headers = *context->session->codec.cached_headers;
			else
				error = diagnostic_error(DiagnosticCode::OutputCodecHeadersUnavailable,
					"No cached H.264/AAC codec configuration is available for the delayed-output handoff");
		}
		if (!error.empty() || !start_pipeline(context, std::move(headers), error)) {
			context->session->end_consumer_lifecycle();
			obs_output_set_last_error(context->output, error.c_str());
			return false;
		}
	} else {
		blog(LOG_INFO,
			"[active-live-delay] Direct capture initialized; waiting for the first H.264 frame before RTMP connect");
	}

	context->active.store(true, std::memory_order_release);
	context->capture_started.store(true, std::memory_order_release);
	context->packet_epoch = context->session->begin_packet_epoch();
	if (preserve_controller)
		context->session->controller.delay.begin_timestamp_epoch();
	if (!obs_output_begin_data_capture(context->output, 0)) {
		context->active.store(false, std::memory_order_release);
		context->capture_started.store(false, std::memory_order_release);
		context->pipeline_ready.store(false, std::memory_order_release);
		context->session->consumers.dispatcher.remove_consumer(context->consumer_id);
		context->consumer_id = 0;
		std::scoped_lock lock(context->pipeline_mutex);
		context->consumer.reset();
		context->multi_target_sender.reset();
		context->session->end_consumer_lifecycle();
		const auto capture_error = diagnostic_error(DiagnosticCode::OutputCaptureStartFailed,
			"OBS refused to begin encoded data capture");
		obs_output_set_last_error(context->output, capture_error.c_str());
		blog(LOG_ERROR, "[active-live-delay] %s", capture_error.c_str());
		return false;
	}
	if (preserve_controller)
		clear_cached_codec_headers(*context->session);
	blog(LOG_INFO, preserve_controller ? "[active-live-delay] Custom output is connected and capturing encoded packets"
					  : "[active-live-delay] Custom output is capturing; RTMP connect is pending codec headers");
	return true;
}

void output_stop(void *data, std::uint64_t)
{
	auto *context = static_cast<OutputData *>(data);
	context->active.store(false, std::memory_order_release);
	context->pipeline_ready.store(false, std::memory_order_release);
	const auto was_capturing = context->capture_started.exchange(false, std::memory_order_acq_rel);
	const auto discontinuity_failures =
		context->session->consumers.dispatcher.dispatch_discontinuity({context->packet_epoch, "Output stopped"});
	if (!discontinuity_failures.empty())
		blog(LOG_WARNING, "[active-live-delay] %s",
			diagnostic_error(DiagnosticCode::PacketDiscontinuityNotificationFailed,
				"Consumer shutdown notification failed: " + discontinuity_failures.front().error).c_str());
	context->session->consumers.dispatcher.remove_consumer(context->consumer_id);
	context->consumer_id = 0;
	{
		std::scoped_lock lock(context->pipeline_mutex);
		context->consumer.reset();
		context->multi_target_sender.reset();
	}
	context->session->controller.delay.return_live();
	context->session->multistream.set_status({});
	context->session->end_consumer_lifecycle();
	if (was_capturing)
		obs_output_end_data_capture(context->output);
}

void output_packet(void *data, encoder_packet *packet)
{
	auto *context = static_cast<OutputData *>(data);
	if (!context->active.load(std::memory_order_acquire))
		return;
	if (!packet) {
		signal_failure(context, DiagnosticCode::OutputEncoderStopped, "The encoder stopped producing packets",
			OBS_OUTPUT_ENCODE_ERROR);
		return;
	}
	if (!context->pipeline_ready.load(std::memory_order_acquire)) {
		if (packet->type != OBS_ENCODER_VIDEO)
			return;
		FlvCodecHeaders headers;
		std::string startup_error;
		if (!read_codec_headers(context->output, headers, startup_error)) {
			const auto waited = context->codec_header_wait_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (waited == 1)
				blog(LOG_INFO, "[active-live-delay] Waiting for both H.264 and AAC codec headers");
			if (waited < kCodecHeaderRetryVideoFrames)
				return;
			signal_failure(context, DiagnosticCode::OutputCodecHeadersUnavailable,
				startup_error.empty() ? "Unable to start RTMP after the first video frame" : std::move(startup_error),
				OBS_OUTPUT_ENCODE_ERROR);
			return;
		}
		if (!start_pipeline(context, std::move(headers), startup_error)) {
			signal_failure(context, DiagnosticCode::RtmpSenderStartupFailed,
				startup_error.empty() ? "Unable to start RTMP after codec headers became ready" : std::move(startup_error),
				OBS_OUTPUT_DISCONNECTED);
			return;
		}
		blog(LOG_INFO, "[active-live-delay] Codec headers are ready; RTMP connected from the first video frame");
	}

	context->session->controller.delay.ingest(copy_encoder_packet(*packet));
	const auto controller_status = context->session->controller.delay.status();
	if (controller_status.state == DelayState::Error) {
		signal_failure(context, DiagnosticCode::OutputControllerFailed,
			controller_status.error.empty() ? "The delay controller stopped" : controller_status.error,
			OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	auto ready = context->session->controller.delay.take_ready_packets();
	if (ready.empty())
		return;

	auto batch = std::make_shared<const ReleasedPacketBatch>(ReleasedPacketBatch{context->packet_epoch, std::move(ready)});
	const auto failures = context->session->consumers.dispatcher.dispatch(std::move(batch));
	{
		std::scoped_lock lock(context->pipeline_mutex);
		if (context->multi_target_sender)
			context->session->multistream.set_status(context->multi_target_sender->status());
	}
	if (!failures.empty() && context->active.load(std::memory_order_acquire))
		signal_failure(context, DiagnosticCode::PacketConsumerFailed,
			"Network packet consumer failed: " + failures.front().error, OBS_OUTPUT_ENCODE_ERROR);
}

std::uint64_t output_total_bytes(void *data)
{
	auto *context = static_cast<OutputData *>(data);
	std::scoped_lock lock(context->pipeline_mutex);
	if (context->multi_target_sender)
		return context->multi_target_sender->status().sent_bytes;
	if (const auto network_consumer = std::dynamic_pointer_cast<NetworkPacketConsumer>(context->consumer))
		return network_consumer->status().sent_bytes;
	return 0;
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

bool cache_active_codec_headers(obs_output_t *output, ActiveDelaySession &session, std::string &error)
{
	FlvCodecHeaders headers;
	if (!read_codec_headers(output, headers, error))
		return false;
	std::scoped_lock lock(session.codec.mutex);
	session.codec.cached_headers = std::move(headers);
	return true;
}

void clear_cached_codec_headers(ActiveDelaySession &session)
{
	std::scoped_lock lock(session.codec.mutex);
	session.codec.cached_headers.reset();
}

void register_active_delay_output(std::shared_ptr<ActiveDelaySession> session)
{
	registered_session = std::move(session);
	obs_register_output(&output_info);
}

} // namespace active_delay
