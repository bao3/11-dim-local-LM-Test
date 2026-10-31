#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Reference PriorityScheduler used only by --self-test. Not part of the benchmark binary.
class PriorityScheduler {
public:
    PriorityScheduler(double capacity, double refill_rate, int max_concurrency, double starvation_timeout)
        : capacity_(capacity),
          tokens_(capacity),
          refill_rate_(refill_rate),
          max_conc_(max_concurrency),
          starve_(starvation_timeout),
          last_refill_(std::chrono::steady_clock::now()) {
        disp_ = std::thread([this] { run(); });
    }

    ~PriorityScheduler() {
        try {
            shutdown(true);
        } catch (...) {
        }
    }

    void submit(const std::function<void()>& task, double cost, int priority, double timeout_sec = -1.0) {
        auto job = std::make_shared<Job>();
        job->fn = task;
        job->cost = cost;
        job->priority = priority;
        job->timeout_sec = timeout_sec;
        job->enqueued = std::chrono::steady_clock::now();
        std::future<void> fut;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!accept_ || stopped_) {
                throw std::runtime_error("scheduler shut down");
            }
            job->seq = seq_++;
            fut = job->prom.get_future();
            q_.push_back(job);
        }
        cv_.notify_all();
        fut.get();
    }

    void shutdown(bool wait = true) {
        if (shutdown_started_.exchange(true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            accept_ = false;
            if (!wait) {
                for (auto& job : q_) {
                    try {
                        job->prom.set_exception(std::make_exception_ptr(std::runtime_error("shutdown")));
                    } catch (...) {
                    }
                }
                q_.clear();
            }
        }
        cv_.notify_all();
        {
            std::unique_lock<std::mutex> lk(mu_);
            drain_cv_.wait(lk, [&] { return q_.empty() && running_ == 0; });
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
        if (disp_.joinable()) {
            disp_.join();
        }
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lk(workers_mu_);
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    struct Job {
        std::function<void()> fn;
        double cost = 0;
        int priority = 0;
        double timeout_sec = -1;
        std::uint64_t seq = 0;
        bool aged = false;
        std::chrono::steady_clock::time_point enqueued{};
        std::promise<void> prom;
    };

    static bool better(const std::shared_ptr<Job>& a, const std::shared_ptr<Job>& b) {
        if (a->aged != b->aged) {
            return a->aged;
        }
        if (a->aged && b->aged) {
            return a->seq < b->seq;
        }
        if (a->priority != b->priority) {
            return a->priority < b->priority;
        }
        return a->seq < b->seq;
    }

    void refill_locked() {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_refill_).count();
        if (dt > 0) {
            tokens_ = std::min(capacity_, tokens_ + dt * refill_rate_);
            last_refill_ = now;
        }
    }

    void execute(const std::shared_ptr<Job>& job) {
        try {
            if (job->timeout_sec < 0.0) {
                job->fn();
            } else {
                struct State {
                    std::mutex mu;
                    std::condition_variable cv;
                    bool finished = false;
                    std::exception_ptr error;
                };
                auto state = std::make_shared<State>();
                std::thread runner([job, state] {
                    try {
                        job->fn();
                    } catch (...) {
                        state->error = std::current_exception();
                    }
                    std::lock_guard<std::mutex> lk(state->mu);
                    state->finished = true;
                    state->cv.notify_one();
                });
                bool timed_out = false;
                {
                    std::unique_lock<std::mutex> lk(state->mu);
                    timed_out = !state->cv.wait_for(lk, std::chrono::duration<double>(job->timeout_sec),
                                                    [&] { return state->finished; });
                }
                if (timed_out) {
                    runner.detach();
                    throw std::runtime_error("run timeout");
                }
                runner.join();
                if (state->error) {
                    std::rethrow_exception(state->error);
                }
            }
            job->prom.set_value();
        } catch (...) {
            try {
                job->prom.set_exception(std::current_exception());
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (running_ > 0) {
                --running_;
            }
        }
        drain_cv_.notify_all();
        cv_.notify_all();
    }

    void run() {
        while (true) {
            std::shared_ptr<Job> chosen;
            {
                std::unique_lock<std::mutex> lk(mu_);
                refill_locked();
                const auto now = std::chrono::steady_clock::now();
                for (auto it = q_.begin(); it != q_.end();) {
                    auto job = *it;
                    const double waited = std::chrono::duration<double>(now - job->enqueued).count();
                    if (job->timeout_sec >= 0.0 && waited >= job->timeout_sec) {
                        it = q_.erase(it);
                        try {
                            job->prom.set_exception(std::make_exception_ptr(std::runtime_error("queue timeout")));
                        } catch (...) {
                        }
                        continue;
                    }
                    if (waited > starve_) {
                        job->aged = true;
                    }
                    ++it;
                }
                int best = -1;
                for (int i = 0; i < static_cast<int>(q_.size()); ++i) {
                    if (best < 0 || better(q_[static_cast<std::size_t>(i)], q_[static_cast<std::size_t>(best)])) {
                        best = i;
                    }
                }
                if (best >= 0 && running_ < max_conc_ && tokens_ + 1e-4 >= q_[static_cast<std::size_t>(best)]->cost) {
                    chosen = q_[static_cast<std::size_t>(best)];
                    q_.erase(q_.begin() + best);
                    tokens_ -= chosen->cost;
                    if (tokens_ < 0) {
                        tokens_ = 0;
                    }
                    ++running_;
                } else if (stopped_ && q_.empty() && running_ == 0) {
                    break;
                } else {
                    if (q_.empty() && running_ == 0) {
                        drain_cv_.notify_all();
                    }
                    cv_.wait_for(lk, std::chrono::milliseconds(5));
                }
            }
            if (!chosen) {
                continue;
            }
            std::thread worker([this, chosen] { execute(chosen); });
            {
                std::lock_guard<std::mutex> lk(workers_mu_);
                workers_.push_back(std::move(worker));
            }
        }
    }

    std::mutex mu_;
    std::mutex workers_mu_;
    std::condition_variable cv_;
    std::condition_variable drain_cv_;
    std::deque<std::shared_ptr<Job>> q_;
    std::vector<std::thread> workers_;
    double capacity_;
    double tokens_;
    double refill_rate_;
    int max_conc_;
    double starve_;
    int running_ = 0;
    std::uint64_t seq_ = 0;
    bool accept_ = true;
    bool stopped_ = false;
    std::atomic<bool> shutdown_started_{false};
    std::chrono::steady_clock::time_point last_refill_;
    std::thread disp_;
};
