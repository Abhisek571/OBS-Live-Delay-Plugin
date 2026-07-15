#include "active-delay-output.hpp"

#include <atomic>
#include <memory>

namespace active_delay {
namespace {
struct OutputData {
	explicit OutputData(obs_output_t *output_) : output(output_) {}
	obs_output_t *output = nullptr;
	DelayController controller;
	std::atomic<std::uint64_t> accepted_bytes = 0;
};

const char *output_name(void *)
{
	return "OBS Active Live Delay (RTMP)";
}

void *output_create(obs_data_t *, obs_output_t *output)
{
	return new OutputData(output);
}

void output_destroy(void *data)
{
	delete static_cast<OutputData *>(data);
}

bool output_start(void *data)
{
	auto *context = static_cast<OutputData *>(data);
	if (!obs_output_can_begin_data_capture(context->output, 0) ||
	    !obs_output_initialize_encoders(context->output, 0)) {
		obs_output_set_last_error(context->output, "Unable to initialize the assigned H.264/AAC encoders");
		return false;
	}

	// The controller is intentionally independent of the network sender. A
	// production RTMP/FLV transport is attached here before packets are exposed
	// to end users; until then the output must not claim a successful stream.
	obs_output_set_last_error(context->output,
		"RTMP transport is not linked. Build with the release RTMP/FLV transport module.");
	return false;
}

void output_stop(void *data, std::uint64_t)
{
	auto *context = static_cast<OutputData *>(data);
	context->controller.return_live();
	obs_output_end_data_capture(context->output);
}

void output_packet(void *data, encoder_packet *packet)
{
	auto *context = static_cast<OutputData *>(data);
	if (!packet) {
		context->controller.reset_for_discontinuity("The encoder stopped producing packets");
		obs_output_signal_stop(context->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	EncodedPacket copy;
	copy.kind = packet->type == OBS_ENCODER_VIDEO ? PacketKind::Video : PacketKind::Audio;
	copy.payload.assign(packet->data, packet->data + packet->size);
	copy.pts_us = packet->timebase_den != 0
		? (packet->pts * 1'000'000LL * packet->timebase_num) / packet->timebase_den
		: packet->dts_usec;
	copy.dts_us = packet->dts_usec;
	copy.keyframe = packet->keyframe;
	context->accepted_bytes.fetch_add(copy.payload.size(), std::memory_order_relaxed);
	context->controller.ingest(std::move(copy));
}

std::uint64_t output_total_bytes(void *data)
{
	return static_cast<OutputData *>(data)->accepted_bytes.load(std::memory_order_relaxed);
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
	.protocols = "rtmp",
};
} // namespace

void register_active_delay_output()
{
	obs_register_output(&output_info);
}

} // namespace active_delay
