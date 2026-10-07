#pragma once
#include "released-packet-dispatcher.hpp"
#include <optional>
namespace active_delay {
enum class TransitionAction { SetDelay, ReturnLive, EmergencyDump };
// epoch is reserved internally by owner admission; direct serialized callers
// leave it zero. It is not a timestamp or a transport wire field.
struct TransitionRequest { TransitionAction action; Microseconds target{}; std::uint64_t epoch = 0; };
// Serialized session-owner API. No driver/network waits occur here.
class TransitionCoordinator {
public:
 TransitionCoordinator(DelayController &controller, ReleasedPacketDispatcher &dispatcher,
  std::uint64_t epoch, FlvCodecHeaders programme_headers);
 void holding_ready(FlvCodecHeaders headers);
 void request(TransitionRequest request);
 void programme(EncodedPacket packet);
 void holding(std::vector<EncodedPacket> packets);
 void check_deadline(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
 [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
 [[nodiscard]] bool programme_selected() const noexcept { return selected_ == Feed::Programme && (!paced_programme_ || programme_boundary_sent_); }
private:
 enum class Feed { Programme, Holding };
 Feed published_feed_=Feed::Programme;
 bool invalidate(std::uint64_t reserved_epoch = 0);
 void publish(Feed feed, std::vector<EncodedPacket> packets);
 void release_ready(std::vector<EncodedPacket> packets);
 void record_publication(const std::vector<EncodedPacket> &packets, Feed feed);
 std::int64_t published_output_=-1000,published_audio_end_=0,published_video_tail_=-1000;
 bool drain_required_=false,guard_active_=false,paced_holding_=false,paced_programme_=false;
 bool programme_boundary_sent_=false,programme_boundary_delivered_=false;
 std::chrono::steady_clock::time_point tick_now_=std::chrono::steady_clock::now(),resume_origin_;
 std::int64_t resume_time_=0;
 std::int64_t audio_end_=0,video_tail_=-1000,drain_time_=0;
 std::int64_t programme_frame_us_=33334,holding_frame_us_=33334;
 std::vector<std::uint8_t> silent_aac_;
 std::vector<EncodedPacket> waiting_programme_;
 std::size_t waiting_bytes_=0,scheduled_bytes_=0;
 std::deque<EncodedPacket> scheduled_;
 std::optional<FlvCodecHeaders> scheduled_headers_;
 std::optional<std::chrono::steady_clock::time_point> drain_ack_;
 std::uint64_t ticket_=0,drain_ticket_=0,holding_ticket_=0,programme_ticket_=0;
 DelayController &controller_;
 ReleasedPacketDispatcher &dispatcher_;
 FlvCodecHeaders programme_headers_, holding_headers_;
 std::uint64_t epoch_;
 Feed desired_ = Feed::Programme;
 std::optional<Feed> selected_;
 std::vector<EncodedPacket> boundary_;
 std::size_t boundary_bytes_ = 0;
 std::optional<std::int64_t> cutoff_;
 std::int64_t offset_ = 0, last_output_ = -1000;
 std::chrono::steady_clock::time_point boundary_deadline_ = std::chrono::steady_clock::now()+std::chrono::seconds(5);
 std::optional<std::chrono::steady_clock::time_point> programme_deadline_;
 std::chrono::steady_clock::time_point programme_key_deadline_ = std::chrono::steady_clock::now()+std::chrono::seconds(5);
};
} // namespace active_delay
