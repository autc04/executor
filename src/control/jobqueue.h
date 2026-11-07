#pragma once

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace Executor::control
{

/* One serialized unit of input work.  Jobs run one at a time on the emulator
 * thread and may split themselves into steps separated by delays (a click is
 * move / down / gap / up). */
class Job
{
public:
    virtual ~Job() = default;

    /* Time to wait before the next run(). */
    virtual std::chrono::milliseconds delay() const = 0;

    /* Runs the next step and returns true when the job is finished. */
    virtual bool run(boost::json::object &result) = 0;

    std::atomic<bool> cancelled{false};
};

using JobDone = std::function<void(boost::json::object)>;

/* Orders input jobs and spaces their steps in time.  Jobs, and the guest state
 * they touch, live on the emulator thread; a helper thread only wakes the
 * emulator at the next deadline. */
class JobQueue
{
public:
    static JobQueue &instance();

    void start();
    void stop();

    /* Thread-safe.  `done` is invoked exactly once, from the emulator thread. */
    void submit(std::shared_ptr<Job> job, JobDone done);

    /* Called on the emulator thread. */
    bool busy();

private:
    JobQueue() = default;

    struct Entry
    {
        std::shared_ptr<Job> job;
        JobDone done;
    };

    void pump();
    void timerMain();

    std::mutex mutex_;
    std::condition_variable timerCond_;
    std::deque<Entry> pending_;
    std::unique_ptr<Entry> active_;
    std::chrono::steady_clock::time_point dueAt_;
    bool hasDeadline_ = false;
    bool stopped_ = false;
    bool started_ = false;
    std::thread timerThread_;
};

}
