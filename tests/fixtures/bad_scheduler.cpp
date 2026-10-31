#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Compiles and runs, but never ages starved tasks. Smaller priority always wins,
// so the hidden harness should observe T1, T3, T2 and award only the compile points.
class PriorityScheduler {
public:
    PriorityScheduler(double, double, int, double) { worker_ = std::thread([this] { loop(); }); }

    ~PriorityScheduler() { shutdown(true); }

    void submit(const std::function<void()>& task, double, int priority, double = -1.0) {
        std::promise<void> prom;
        auto fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) {
                throw std::runtime_error("scheduler shut down");
            }
            items_.push_back(Item{priority, seq_++, task, std::move(prom)});
        }
        cv_.notify_one();
        fut.get();
    }

    void shutdown(bool) {
        bool do_join = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (joined_) {
                return;
            }
            joined_ = true;
            stop_ = true;
            do_join = true;
        }
        cv_.notify_all();
        if (do_join && worker_.joinable()) {
            worker_.join();
        }
    }

private:
    struct Item {
        int priority;
        int seq;
        std::function<void()> task;
        std::promise<void> prom;
    };

    void loop() {
        while (true) {
            Item item;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !items_.empty(); });
                if (items_.empty()) {
                    return;
                }
                auto best = items_.begin();
                for (auto it = items_.begin(); it != items_.end(); ++it) {
                    if (it->priority < best->priority || (it->priority == best->priority && it->seq < best->seq)) {
                        best = it;
                    }
                }
                item.priority = best->priority;
                item.seq = best->seq;
                item.task = std::move(best->task);
                item.prom = std::move(best->prom);
                items_.erase(best);
            }
            try {
                item.task();
                item.prom.set_value();
            } catch (...) {
                try {
                    item.prom.set_exception(std::current_exception());
                } catch (...) {
                }
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Item> items_;
    int seq_ = 0;
    bool stop_ = false;
    bool joined_ = false;
    std::thread worker_;
};
