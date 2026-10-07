#pragma once
#include "holding-capture.hpp"
#include "transition-coordinator.hpp"
#include "rtmp-sender.hpp"
#include <functional>
#include <condition_variable>
#include <thread>
namespace active_delay {
enum class BroadcastPhase { Stopped, Arming, Filling, Ready, Connecting, Broadcasting, Stopping, Failed };
struct PipelineStatus { bool ready=false; bool holding_ready=false; bool stopped=true; std::string error; bool transition_pending=false; BroadcastPhase phase=BroadcastPhase::Stopped; };
// The delay controller reports Live when idle and Delayed while an armed buffer
// fills, so its state only describes what viewers see while this is true.
inline bool broadcast_on_air(const PipelineStatus &s) {
 return !s.stopped && (s.phase==BroadcastPhase::Connecting || s.phase==BroadcastPhase::Broadcasting);
}
class PipelineOwner {
public:
 using HoldingFactory=std::function<std::unique_ptr<HoldingCapture>()>;
 using ConsumerStart=std::function<std::shared_ptr<ReleasedPacketConsumer>(FlvCodecHeaders,SenderErrorCallback,std::string&)>;
 using Finish=std::function<void(const std::string&)>;
 using ConsumerReady=std::function<bool(const std::shared_ptr<ReleasedPacketConsumer>&)>;
 PipelineOwner(DelayController &,ReleasedPacketDispatcher &,std::uint64_t,HoldingFactory,ConsumerStart,Finish,
  Microseconds prebuffer_target={}, ConsumerReady consumer_ready={});
 ~PipelineOwner();
 void start();
 bool start_broadcast(std::string &error);
 void headers(FlvCodecHeaders);
 void enqueue(EncodedPacket);
 std::uint64_t capture_token() const;
 bool enqueue(EncodedPacket, std::uint64_t capture_token);
 bool enqueue_uninterleaved(EncodedPacket, std::uint64_t capture_token);
 bool request(TransitionRequest,std::string &error);
 void failure(std::string);
 void request_stop() noexcept;
 void stop() noexcept;
 [[nodiscard]] PipelineStatus status() const;
 [[nodiscard]] std::shared_ptr<ReleasedPacketConsumer> consumer_snapshot() const;
private:
 void run() noexcept;
 void clear_capture_locked();
 struct FailureState { std::mutex mutex; std::string error; };
 DelayController &controller_;
 ReleasedPacketDispatcher &dispatcher_;
 std::uint64_t epoch_;
 HoldingFactory holding_factory_;
 ConsumerStart consumer_start_;
 Finish finish_;
 const Microseconds prebuffer_target_;
 ConsumerReady consumer_ready_;
 bool broadcast_requested_=false;
 std::chrono::steady_clock::time_point last_video_{}, last_audio_{}, last_key_{};
 std::shared_ptr<FailureState> failure_=std::make_shared<FailureState>();
 mutable std::mutex mutex_;
 std::mutex lifecycle_mutex_;
 std::condition_variable wake_;
 std::thread worker_;
 std::optional<FlvCodecHeaders> headers_;
 std::deque<TransitionRequest> requests_;
 std::shared_ptr<ReleasedPacketConsumer> consumer_;
 std::vector<EncodedPacket> packets_;
 std::deque<EncodedPacket> video_ingress_, audio_ingress_;
 std::optional<std::int64_t> video_input_, audio_input_;
 std::size_t ingress_bytes_=0;
 std::size_t packet_bytes_=0;
 bool stopping_=false;
 std::uint64_t capture_epoch_=1;
 PipelineStatus status_;
};
} // namespace active_delay
