#include "multi-target-sender.hpp"

#include "diagnostic-error.hpp"

#include <future>
#include <stdexcept>
#include <utility>

namespace active_delay {
MultiTargetSender::MultiTargetSender(RtmpConnectionFactory factory, SenderConfig config)
	: factory_(std::move(factory)), config_(config)
{
}

bool MultiTargetSender::start(RtmpTarget primary, std::string primary_name, MultistreamConfiguration configuration,
	FlvCodecHeaders headers, PrimaryFailureCallback on_primary_failure, std::string &error)
{
	stop();
	if (!validate_multistream_configuration(configuration, error))
		return false;
	// Starting targets concurrently must not require connection factories supplied
	// by integrations or tests to be re-entrant.  Only construction is
	// serialized; each resulting sender still connects and runs independently.
	auto factory_mutex = std::make_shared<std::mutex>();
	auto synchronized_factory = [factory = factory_, factory_mutex]() mutable {
		std::scoped_lock lock(*factory_mutex);
		return factory();
	};

	struct PendingWorker {
		Worker worker;
		RtmpTarget target;
	};
	std::vector<PendingWorker> pending;
	pending.push_back({{"primary", primary_name.empty() ? "Primary OBS service" : std::move(primary_name), true,
		std::make_shared<NetworkPacketConsumer>(synchronized_factory, config_)}, std::move(primary)});
	for (const auto &destination : configuration.secondary_destinations) {
		if (!destination.enabled)
			continue;
		pending.push_back({{destination.id, safe_destination_label(destination), false,
			std::make_shared<NetworkPacketConsumer>(synchronized_factory, config_)}, destination.target});
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
			std::scoped_lock lock(mutex_);
			for (auto &stored : workers_) {
				if (stored.id == worker.id)
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
			std::scoped_lock lock(mutex_);
			for (auto &stored : workers_)
				if (stored.id == worker.id)
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
		workers.swap(workers_);
	}
	for (auto &worker : workers)
		worker.consumer->stop();
}

MultiTargetStatus MultiTargetSender::status() const
{
	MultiTargetStatus result;
	std::scoped_lock lock(mutex_);
	bool primary_running = false;
	for (const auto &worker : workers_) {
		auto sender = worker.consumer->status();
		if (!worker.isolated_error.empty()) {
			sender.state = SenderState::Failed;
			sender.error = worker.isolated_error;
		}
		result.sent_bytes += sender.sent_bytes;
		if (worker.primary)
			primary_running = sender.state == SenderState::Running || sender.state == SenderState::Reconnecting;
		result.destinations.push_back({worker.id, worker.name, worker.primary, std::move(sender)});
	}
	result.aggregate_state = primary_running ? SenderState::Running
		: (result.destinations.empty() ? SenderState::Stopped : SenderState::Failed);
	return result;
}

} // namespace active_delay
