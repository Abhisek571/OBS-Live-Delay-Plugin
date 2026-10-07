#include "ffmpeg-rtmp-connection.hpp"
#include "rtmp-sender.hpp"
#include "flv-muxer.hpp"
#include "pipeline-owner.hpp"
#include "network-packet-consumer.hpp"
#include "multi-target-sender.hpp"
#include "transition-test-headers.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <chrono>
#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace active_delay;
using namespace std::chrono_literals;

namespace {
class SyntheticHolding final : public HoldingCaptureBackend {
public:
    bool prepare(HoldingFeed &, std::string &) override { return true; }
    bool start(std::string &) override { return true; }
    void stop() noexcept override {}
};
}

// Only the Python fixture supplies endpoints: numeric loopback, ephemeral port.
int main(int argc, char **argv)
{
    try {
        if (argc < 3) throw std::runtime_error("mode and loopback port required");
        const std::string mode = argv[1];
        const int port = std::stoi(argv[2]);
        if (port < 1 || port > 65535) throw std::runtime_error("invalid port");
        RtmpTarget target{"rtmps://127.0.0.1:" + std::to_string(port) + "/live",
            "synthetic-test", {}, {}, mode == "deadline" ? 250ms : 5s};
        const auto begin = std::chrono::steady_clock::now();
        auto elapsed = [](auto start) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        };
        std::cout << "avformat=" << avformat_version() << '\n';
        if (mode == "isolation") {
            if (argc != 4) throw std::runtime_error("numeric primary port required");
            const int primary_port = std::stoi(argv[3]);
            if (primary_port < 1 || primary_port > 65535) throw std::runtime_error("invalid primary port");
            RtmpTarget primary{"rtmp://127.0.0.1:" + std::to_string(primary_port) + "/live",
                "synthetic-primary", {}, {}, 1s};
            std::string url, error;
            if (!build_rtmp_publish_url(primary, url, error)) throw std::runtime_error(error);
            std::atomic_bool stopped = false;
            std::atomic_int received = 0;
            std::atomic_bool media_received = false;
            const std::vector<std::uint8_t> marker{0x17, 1, 0, 0, 0, 0, 0, 0, 2, 0x65, 0x55};
            std::vector<std::uint8_t> wire;
            auto receiver = std::async(std::launch::async, [&] {
                struct State { std::atomic_bool &stop; std::chrono::steady_clock::time_point deadline; } state{stopped, begin + 4s};
                AVIOInterruptCB cb{[](void *opaque) {
                    const auto &s = *static_cast<State *>(opaque);
                    return s.stop || std::chrono::steady_clock::now() >= s.deadline ? 1 : 0;
                }, &state};
                AVDictionary *options = nullptr;
                av_dict_set(&options, "rtmp_listen", "1", 0);
                av_dict_set(&options, "timeout", "2", 0);
                AVIOContext *io = nullptr;
                const auto result = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, &cb, &options);
                av_dict_free(&options);
                if (result >= 0) {
                    unsigned char byte;
                    while (!stopped && avio_read(io, &byte, 1) == 1) {
                        wire.push_back(byte);
                        ++received;
                        if (wire.size() >= marker.size() && std::equal(marker.begin(), marker.end(), wire.end() - marker.size()))
                            media_received = true;
                    }
                }
                if (io) avio_closep(&io);
                return result;
            });
            SenderConfig config;
            config.reconnect_attempts = 1;
            config.reconnect_delay = 20ms; // only listener startup race may need retry
            std::atomic_bool primary_failed = false;
            MultiTargetSender multi([] { return std::make_unique<FfmpegRtmpConnection>(); }, config);
            MultistreamConfiguration destinations;
            destinations.secondary_destinations.push_back({"tls-fault", "Synthetic TLS fault", target});
            const bool started = multi.start(primary, "Synthetic primary", destinations, programme_test_headers(),
                [&](const std::string &) { primary_failed = true; }, error);
            bool isolated = false;
            const auto deadline = begin + 3s;
            while (started && std::chrono::steady_clock::now() < deadline) {
                const auto status = multi.status();
                isolated = status.destinations.size() == 2 &&
                    status.destinations[0].sender.state == SenderState::Running &&
                    status.destinations[1].sender.state == SenderState::Failed;
                if (isolated) break;
                std::this_thread::sleep_for(1ms);
            }
            const auto before = received.load();
            if (isolated) {
                auto batch = std::make_shared<ReleasedPacketBatch>();
                batch->packets.push_back({PacketKind::Video, {0, 0, 0, 2, 0x65, 0x55}, 10000, 10000, true});
                batch->packets.push_back({PacketKind::Audio, {0x21, 0x10}, 11000, 11000, false});
                multi.consume(batch);
            }
            const auto delivered_deadline = std::chrono::steady_clock::now() + 500ms;
            while (!media_received && std::chrono::steady_clock::now() < delivered_deadline)
                std::this_thread::sleep_for(1ms);
            const bool progressed = received > before;
            const auto stop_begin = std::chrono::steady_clock::now();
            multi.stop();
            const auto ms = elapsed(stop_begin);
            stopped = true;
            const auto result = receiver.get();
            std::cout << "mode=isolation primary_received_bytes=" << received << " progressed_after_tls_failure=" << progressed
                      << " media_received=" << media_received << " primary_failed=" << primary_failed << " stop_ms=" << ms << '\n';
            if (!isolated || !progressed || !media_received || primary_failed || result < 0 || ms >= 1500)
                throw std::runtime_error("TLS secondary failure must not stop actual RTMP primary delivery");
            return 0;
        }
        if (mode == "owner-stop" || mode == "owner-reject") {
            DelayController controller;
            ReleasedPacketDispatcher dispatcher;
            std::promise<void> failed, release;
            auto failure = failed.get_future();
            auto released = release.get_future().share();
            SenderConfig config;
            config.reconnect_attempts = 0;
            PipelineOwner owner(controller, dispatcher, 1,
                [] { return std::make_unique<HoldingCapture>(std::make_unique<SyntheticHolding>()); },
                [&](FlvCodecHeaders headers, SenderErrorCallback fail, std::string &error)
                    -> std::shared_ptr<ReleasedPacketConsumer> {
                    auto consumer = std::make_shared<NetworkPacketConsumer>(
                        [] { return std::make_unique<FfmpegRtmpConnection>(); }, config);
                    if (!consumer->start(target, std::move(headers), [&, fail](const std::string &message) {
                        fail(message);
                        failed.set_value();
                    }, error)) return {};
                    // Hold the factory return to force actual TLS failure before installation.
                    if (mode == "owner-reject") released.wait();
                    return consumer;
                }, {});
            owner.start();
            owner.headers(programme_test_headers());
            bool latched = true;
            if (mode == "owner-reject") {
                latched = failure.wait_for(2s) == std::future_status::ready;
                const auto status = owner.status();
                latched = latched && !status.ready && status.phase == BroadcastPhase::Failed && !status.error.empty();
                release.set_value();
            } else {
                std::string command;
                std::getline(std::cin, command);
            }
            const auto stop_begin = std::chrono::steady_clock::now();
            owner.request_stop();
            const auto admission_ms = elapsed(stop_begin);
            owner.stop();
            const auto ms = elapsed(stop_begin);
            std::cout << "mode=" << mode << " request_stop_ms=" << admission_ms << " joined_stop_ms=" << ms
                      << " failure_before_install=" << latched << " consumers=" << dispatcher.consumer_count() << '\n';
            if (!latched || admission_ms >= 100 || ms >= 1500 || dispatcher.consumer_count() != 0 || !owner.status().stopped)
                throw std::runtime_error("owner must latch TLS failure and quiesce pending transport off callback");
            return 0;
        }
        if (mode == "sender-stop") {
            RtmpSender sender([] { return std::make_unique<FfmpegRtmpConnection>(); }, {});
            for (int cycle = 0; cycle < 3; ++cycle) {
                std::string error;
                if (!sender.start(target, FlvMuxer({{1}, {0x12, 0x10}}).sequence_headers(), {}, error))
                    throw std::runtime_error("sender start rejected");
                std::string command;
                std::getline(std::cin, command); // fixture observed actual TLS ClientHello
                const auto stop_begin = std::chrono::steady_clock::now();
                sender.stop();
                const auto ms = elapsed(stop_begin);
                std::cout << "cycle=" << cycle << " stop_ms=" << ms << std::endl;
                if (ms >= 1500) throw std::runtime_error("TLS sender stop exceeded observation budget");
            }
            return 0;
        }
        if (mode == "cafile") {
            if (argc != 4) throw std::runtime_error("synthetic CA path required");
            AVDictionary *options = nullptr;
            av_dict_set(&options, "tls_verify", "1", 0);
            av_dict_set(&options, "ca_file", argv[3], 0);
            av_dict_set(&options, "rw_timeout", "1000000", 0);
            AVIOInterruptCB cb{[](void *p) {
                return std::chrono::steady_clock::now() - *static_cast<std::chrono::steady_clock::time_point *>(p) > 2s ? 1 : 0;
            }, const_cast<std::chrono::steady_clock::time_point *>(&begin)};
            AVIOContext *io = nullptr;
            const auto result = avio_open2(&io, ("tls://127.0.0.1:" + std::to_string(port)).c_str(),
                AVIO_FLAG_WRITE, &cb, &options);
            av_dict_free(&options);
            if (io) avio_closep(&io);
            std::cout << "cafile_result=" << result << " connect_ms=" << elapsed(begin) << '\n';
            // Exact linked Schannel backend does not implement ca_file trust.
            return result < 0 ? 0 : 1;
        }
        FfmpegRtmpConnection connection;
        std::string error;
        auto result = std::async(std::launch::async, [&] { return connection.connect(target, error); });
        auto measured_begin = begin;
        if (mode == "cancel") {
            std::string command;
            std::getline(std::cin, command);
            measured_begin = std::chrono::steady_clock::now();
            connection.interrupt();
        }
        const bool ready = result.wait_for(1500ms) == std::future_status::ready;
        if (!ready) connection.interrupt();
        const bool connected = result.get();
        const auto ms = elapsed(measured_begin);
        const auto close_begin = std::chrono::steady_clock::now();
        connection.close();
        const auto close_ms = elapsed(close_begin);
        std::cout << "mode=" << mode << " connect_or_cancel_ms=" << ms << " close_ms=" << close_ms
                  << " connected=" << connected << " error=" << error << '\n';
        if (!ready || connected || ms >= 1500 || close_ms >= 1000)
            throw std::runtime_error("TLS fault must fail and clean up within observation budget");
        if (error.find("synthetic-test") != std::string::npos || error.find("127.0.0.1") != std::string::npos)
            throw std::runtime_error("operational error leaked endpoint");
        if (mode == "reject" && error.find("certificate trust and hostname") == std::string::npos)
            throw std::runtime_error("RTMPS error must explain certificate trust and hostname requirements");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}