#include "multi-target-sender.hpp"

#include "diagnostic-error.hpp"

#include <algorithm>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

namespace active_delay {
namespace {
RtmpConnectionFactory assign_connection(RtmpConnectionFactory &factory)
{
	auto connection = std::make_shared<std::unique_ptr<IRtmpConnection>>();
	std::exception_ptr creation_error;
	try {
		*connection = factory();
	} catch (...) {
		creation_error = std::current_exception();
	}
	return [connection = std::move(connection), creation_error, allocate = factory, first = true]() mutable {
		if (creation_error)
			std::rethrow_exception(creation_error);
		if (first) { first = false; return std::move(*connection); }
		return allocate();
	};
}
} // namespace

MultiTargetSender::MultiTargetSender(RtmpConnectionFactory factory, SenderConfig config)
	: factory_([allocate = std::make_shared<RtmpConnectionFactory>(std::move(factory)),
		guard = std::make_shared<std::mutex>()] {
		std::scoped_lock lock(*guard);
		return (*allocate)();
	}), config_(config)
{
}

MultiTargetSender::~MultiTargetSender()
{
	stop();
}

bool MultiTargetSender::start(RtmpTarget primary, std::string primary_name, MultistreamConfiguration configuration,
	FlvCodecHeaders headers, PrimaryFailureCallback on_primary_failure, std::string &error)
{
	stop();
	if (!validate_multistream_configuration(configuration, primary, error))
		return false;

	struct PendingWorker {
		Worker worker;
		RtmpTarget target;
	};
	std::vector<PendingWorker> pending;
	pending.push_back({{"primary", primary_name.empty() ? "Primary OBS service" : std::move(primary_name), true,
		std::make_shared<NetworkPacketConsumer>(assign_connection(factory_), config_)}, std::move(primary)});
	for (const auto &destination : configuration.secondary_destinations) {
		if (!destination.enabled)
			continue;
		pending.push_back({{destination.id, safe_destination_label(destination), false,
			std::make_shared<NetworkPacketConsumer>(assign_connection(factory_), config_)}, destination.target});
	}
	std::vector<Worker> workers;
	workers.reserve(pending.size());
	for (auto &entry : pending)
		workers.push_back(entry.worker);

	struct StartResult { bool started; std::string error; };
	std::vector<std::future<StartResult>> starts;
	starts.reserve(pending.size());
	for (std::size_t index = 0; index < pending.size(); ++index) {
		const auto target = pending[index].target;
		auto consumer = workers[index].consumer;
		auto target_headers = headers;
		auto callback = workers[index].primary ? on_primary_failure : PrimaryFailureCallback{};
		starts.emplace_back(std::async(std::launch::async,
			[consumer = std::move(consumer), target, headers = std::move(target_headers), callback = std::move(callback)]() mutable {
				std::string start_error;
				const bool started = consumer->start(std::move(target), std::move(headers), std::move(callback), start_error);
				return StartResult{started, std::move(start_error)};
			}));
	}

	bool primary_started = false;
	for (std::size_t index = 0; index < starts.size(); ++index) {
		auto result = starts[index].get();
		if (result.started) {
			if (workers[index].primary)
				primary_started = true;
		} else if (workers[index].primary) {
			error = diagnostic_error(DiagnosticCode::MultiTargetStartupFailed,
				result.error.empty() ? "The primary RTMP destination did not start"
							 : "Primary RTMP destination did not start: " + result.error);
		} else {
			workers[index].isolated_error = diagnostic_error(DiagnosticCode::SecondaryTargetFailed,
				result.error.empty() ? "The secondary RTMP destination did not start"
							 : "Secondary RTMP destination did not start: " + result.error);
		}
	}
	if (!primary_started) {
		for (auto &worker : workers)
			worker.consumer->request_stop();
		for (auto &worker : workers)
			worker.consumer->stop();
		return false;
	}
	{
		std::scoped_lock lock(mutex_);
		workers_ = std::move(workers);
	}
	return true;
}

void MultiTargetSender::consume(const std::shared_ptr<const ReleasedPacketBatch> &batch)
{
	std::vector<Worker> workers;
	{
		std::scoped_lock lock(mutex_);
		workers = workers_;
	}
	for (auto &worker : workers) {
		if (!worker.primary && !worker.isolated_error.empty())
			continue;
		try {
			worker.consumer->consume(batch);
		} catch (const std::exception &exception) {
			if (worker.primary)
				throw;
			// Do not join a slow network worker from the encoded-packet callback.
			// Mark it unavailable and let normal output shutdown own the join.
			worker.consumer->request_stop();
			std::scoped_lock lock(mutex_);
			for (auto &stored : workers_) {
				if (stored.consumer == worker.consumer)
					stored.isolated_error = diagnostic_error(DiagnosticCode::SecondaryTargetFailed,
						"Secondary delivery stopped: " + std::string(exception.what()));
			}
		}
	}
}

void MultiTargetSender::discontinuity(const PacketDiscontinuity &event)
{
	std::vector<Worker> workers;
	{
		std::scoped_lock lock(mutex_);
		workers = workers_;
	}
	for (auto &worker : workers) {
		if (!worker.primary && !worker.isolated_error.empty())
			continue;
		try {
			worker.consumer->discontinuity(event);
		} catch (const std::exception &exception) {
			if (worker.primary)
				throw;
			worker.consumer->request_stop();
			std::scoped_lock lock(mutex_);
			for (auto &stored : workers_)
				if (stored.consumer == worker.consumer)
					stored.isolated_error = diagnostic_error(DiagnosticCode::SecondaryTargetFailed,
						"Secondary discontinuity handling failed: " + std::string(exception.what()));
		}
	}
}

void MultiTargetSender::stop() noexcept
{
	std::vector<Worker> workers;
	{
		std::scoped_lock lock(mutex_);
		workers = workers_;
		++stops_in_progress_;
	}
	for (auto &worker : workers)
		worker.consumer->request_stop();
	for (auto &worker : workers)
		worker.consumer->stop();
	{
		std::scoped_lock lock(mutex_);
		// Erase only this shutdown's consumers, never a newly started session
		// with the same destination IDs. Keep rows visible until joins finish.
		std::erase_if(workers_, [&workers](const Worker &stored) {
			return std::any_of(workers.begin(), workers.end(), [&stored](const Worker &stopped) {
				return stored.consumer == stopped.consumer;
			});
		});
		--stops_in_progress_;
	}
}

MultiTargetStatus MultiTargetSender::status() const
{
	MultiTargetStatus result;
	std::scoped_lock lock(mutex_);
	SenderState primary_state = SenderState::Stopped;
	for (const auto &worker : workers_) {
		auto sender = worker.consumer->status();
		if (!worker.isolated_error.empty()) {
			sender.state = SenderState::Failed;
			sender.error = worker.isolated_error;
		}
		result.sent_bytes += sender.sent_bytes;
		if (worker.primary)
			primary_state = sender.state;
		result.destinations.push_back({worker.id, worker.name, worker.primary, std::move(sender)});
	}
	result.aggregate_state = stops_in_progress_ != 0 ? SenderState::Stopping : primary_state;
	return result;
}

bool MultiTargetSender::delivery_ready(std::uint64_t epoch, std::uint64_t ticket, bool boundary) const
{
 std::scoped_lock lock(mutex_);
 if(workers_.empty())return false;
 bool ready=true;
 for(const auto &worker:workers_) {
  const auto status=worker.consumer->status();
  auto &wait=boundary ? worker.boundary_wait : worker.ticket_wait;
  if(!worker.primary && (!worker.isolated_error.empty() || status.state==SenderState::Failed))continue;
  // A secondary that is reconnecting, or reconnected but not yet resumed at a
  // fresh keyframe, drops media instead of queueing it, so it cannot stall the
  // others. Its reconnect budget and bounded queue decide failure, not this gate.
  const bool resuming=status.state==SenderState::Reconnecting ||
   (status.state==SenderState::Running && status.reconnect_count!=0 && status.published_epoch==0);
  if(!worker.primary && resuming){wait.since.reset();continue;}
  const bool delivered=boundary ? worker.consumer->boundary_delivered(epoch) : worker.consumer->delivered(epoch,ticket);
  if(delivered)continue;
  if(worker.primary){ready=false;continue;}
  const auto now=std::chrono::steady_clock::now();
  // Measure time without delivery progress, not time on one query: callers
  // alternate between tickets each tick. A wait left unpolled has ended.
  const auto progress=boundary ? status.published_epoch : (status.delivered_epoch==epoch ? status.delivery_ticket : 0);
  if(!wait.since || wait.epoch!=epoch || wait.progress!=progress || now-wait.polled>std::chrono::milliseconds(250)) {
   wait.epoch=epoch;wait.progress=progress;wait.since=now;
  }
  wait.polled=now;
  if(now-*wait.since<std::chrono::seconds(1)){ready=false;continue;}
  worker.isolated_error="TRANSITION_SECONDARY_TIMEOUT: boundary delivery stalled";
  worker.consumer->request_stop(); // owner shutdown joins; never wait here
 }
 return ready;
}

bool MultiTargetSender::delivered(std::uint64_t epoch, std::uint64_t ticket) const
{
 return delivery_ready(epoch,ticket,false);
}

bool MultiTargetSender::boundary_delivered(std::uint64_t epoch) const
{
 return delivery_ready(epoch,0,true);
}

} // namespace active_delay
