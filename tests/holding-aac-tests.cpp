#include "holding-obs-audio.hpp"
#include <obs.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

void check(bool v, const char *message) { if (!v) throw std::runtime_error(message); }
struct AacCapture {
	obs_output_t *output = nullptr;
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<std::vector<uint8_t>> packets;
	std::vector<int64_t> timestamps;
	bool failed = false;
	static const char *name(void *) { return "Holding silent AAC validation"; }
	static void *create(obs_data_t *settings, obs_output_t *output)
	{
		auto *self = reinterpret_cast<AacCapture *>(static_cast<intptr_t>(obs_data_get_int(settings, "test_context")));
		if (self) self->output = output;
		return self;
	}
	static void destroy(void *) {}
	static bool start(void *p)
	{
		auto &s = *static_cast<AacCapture *>(p);
		return obs_output_can_begin_data_capture(s.output, 0) && obs_output_initialize_encoders(s.output, 0) &&
			obs_output_begin_data_capture(s.output, 0);
	}
	static void stop(void *p, uint64_t) { obs_output_end_data_capture(static_cast<AacCapture *>(p)->output); }
	static void packet(void *p, encoder_packet *packet) noexcept
	{
		auto &s = *static_cast<AacCapture *>(p);
		try {
			std::scoped_lock lock(s.mutex);
			if (!packet || !packet->data || !packet->size) s.failed = true;
			else {
				s.packets.emplace_back(packet->data, packet->data + packet->size);
				s.timestamps.push_back(packet->dts_usec);
			}
			s.wake.notify_all();
		} catch (...) { /* test reports timeout rather than throwing through C */ }
	}
};
int main(int argc, char **argv)
{
	try {
		check(argc == 4, "AAC test requires module DLL, module data and ADTS artifact paths");
		// libobs loads modules with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which
		// intentionally ignores PATH. Register dependencies only in this process.
		const auto module_root = std::filesystem::path(argv[1]).parent_path().parent_path().parent_path();
		const auto bin = module_root / "bin" / "64bit";
		const auto directory = AddDllDirectory(bin.c_str());
		check(directory, "AAC test dependency directory unavailable");
		struct DllGuard { DLL_DIRECTORY_COOKIE cookie; ~DllGuard() { RemoveDllDirectory(cookie); } } dll_guard{directory};
		check(obs_startup("en-US", nullptr, nullptr), "libobs test startup failed");
		struct Guard { ~Guard() { obs_shutdown(); } } guard;
		obs_module_t *module = nullptr;
		check(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(module), "installed AAC module load failed");
		active_delay::HoldingSilentAudio silence;
		std::string error;
		check(silence.open(48000, SPEAKERS_STEREO, error), "owned silent audio open failed");
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", 128);
		auto *encoder = obs_audio_encoder_create("ffmpeg_aac", "Holding AAC validation", settings, 0, nullptr);
		obs_data_release(settings);
		check(encoder, "real AAC encoder unavailable");
		struct EncoderGuard { obs_encoder_t *p; ~EncoderGuard() { obs_encoder_release(p); } } encoder_guard{encoder};
		obs_encoder_set_audio(encoder, silence.get());
		obs_output_info info{};
		info.id = "holding_aac_validation"; info.flags = OBS_OUTPUT_AUDIO | OBS_OUTPUT_ENCODED;
		info.get_name = AacCapture::name; info.create = AacCapture::create; info.destroy = AacCapture::destroy;
		info.start = AacCapture::start; info.stop = AacCapture::stop; info.encoded_packet = AacCapture::packet;
		info.encoded_audio_codecs = "aac";
		obs_register_output(&info);
		AacCapture captured;
		settings = obs_data_create();
		obs_data_set_int(settings, "test_context", static_cast<int64_t>(reinterpret_cast<intptr_t>(&captured)));
		auto *output = obs_output_create(info.id, "Holding AAC test output", settings, nullptr);
		obs_data_release(settings);
		check(output, "AAC validation output unavailable");
		struct OutputGuard {
			obs_output_t *p;
			~OutputGuard() { if (obs_output_active(p)) obs_output_force_stop(p); obs_output_release(p); }
		} output_guard{output};
		obs_output_set_audio_encoder(output, encoder, 0);
		check(obs_encoder_audio(encoder) == silence.get() && obs_output_audio(output) == silence.get(),
			"AAC encoder or output is not attached exclusively to owned silence");
		check(obs_output_start(output), "real silent AAC capture start failed");
		{
			std::unique_lock lock(captured.mutex);
			check(captured.wake.wait_for(lock, std::chrono::seconds(5), [&] { return captured.failed || captured.packets.size() >= 48; }), "real AAC readiness timeout");
			check(!captured.failed, "AAC encoder failed");
		}
		uint8_t *extra = nullptr; size_t extra_size = 0;
		check(obs_encoder_get_extra_data(encoder, &extra, &extra_size) && extra_size >= 2, "real AAC codec header unavailable");
		check(extra[0] == 0x11 && (extra[1] & 0xf8) == 0x90, "unexpected 48 kHz stereo AAC-LC config");
		obs_output_force_stop(output);
		obs_output_release(output); output_guard.p = nullptr;
		std::ofstream artifact(argv[3], std::ios::binary);
		check(artifact.good(), "AAC artifact open failed");
		for (size_t i = 0; i < captured.packets.size(); ++i) {
			if (i) {
				const auto delta = captured.timestamps[i] - captured.timestamps[i - 1];
				check(delta == 21333 || delta == 21334, "AAC timestamps do not follow 1024-frame sample clock");
			}
			const auto &packet = captured.packets[i];
			const auto size = packet.size() + 7;
			check(size < 8192, "AAC packet too large for ADTS");
			const uint8_t adts[] = {0xff, 0xf1, 0x4c, static_cast<uint8_t>(0x80 | (size >> 11)),
				static_cast<uint8_t>(size >> 3), static_cast<uint8_t>((size << 5) | 0x1f), 0xfc};
			artifact.write(reinterpret_cast<const char *>(adts), sizeof(adts));
			artifact.write(reinterpret_cast<const char *>(packet.data()), static_cast<std::streamsize>(packet.size()));
		}
		check(artifact.good(), "AAC artifact write failed");
		std::cout << "Real OBS AAC packets=" << captured.packets.size() << "; 48k stereo; headers and sample-clock verified; decode artifact written\n";
	} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}