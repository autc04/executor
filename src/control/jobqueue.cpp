#include <control/jobqueue.h>

#include <control/thread_util.h>

#include <base/emulator_thread.h>

#include <utility>
#include <vector>

using namespace Executor::control;
using namespace std::chrono_literals;

JobQueue &JobQueue::instance()
{
    static JobQueue queue;
    return queue;
}

void JobQueue::start()
{
    std::lock_guard lk(mutex_);

    if(started_)
        return;

    started_ = true;
    timerThread_ = std::thread([this] { timerMain(); });
}

void JobQueue::stop()
{
    {
        std::lock_guard lk(mutex_);
        stopped_ = true;
        pending_.clear();
        active_.reset();
    }

    timerCond_.notify_all();

    if(timerThread_.joinable())
        timerThread_.join();
}

void JobQueue::submit(std::shared_ptr<Job> job, JobDone done)
{
    constexpr size_t kMaxPending = 32;
    bool busy = false;

    {
        std::lock_guard lk(mutex_);

        if(stopped_)
            return; /* the caller's deadline covers this */

        if(pending_.size() >= kMaxPending)
            busy = true;
        else
            pending_.push_back(Entry{ std::move(job), std::move(done) });
    }

    if(busy)
    {
        boost::json::object result;
        result["ok"] = false;
        result["code"] = "busy";
        result["error"] = "too many pending input jobs";
        done(std::move(result));
        return;
    }

    Executor::runOnEmulatorThread([] { JobQueue::instance().pump(); });
}

bool JobQueue::busy()
{
    std::lock_guard lk(mutex_);
    return active_ != nullptr || !pending_.empty();
}

void JobQueue::pump()
{
    std::vector<std::pair<JobDone, boost::json::object>> completions;

    {
        std::lock_guard lk(mutex_);

        auto now = std::chrono::steady_clock::now();

        for(;;)
        {
            if(!active_)
            {
                if(pending_.empty())
                {
                    hasDeadline_ = false;
                    break;
                }

                active_ = std::make_unique<Entry>(std::move(pending_.front()));
                pending_.pop_front();
                dueAt_ = now + active_->job->delay();
            }

            if(now < dueAt_)
            {
                hasDeadline_ = true;
                break;
            }

            boost::json::object result;

            if(active_->job->cancelled)
            {
                result["ok"] = false;
                result["code"] = "cancelled";
                result["error"] = "job cancelled";
            }
            else if(!active_->job->run(result))
            {
                now = std::chrono::steady_clock::now();
                dueAt_ = now + active_->job->delay();
                continue;
            }

            completions.emplace_back(std::move(active_->done), std::move(result));
            active_.reset();
            now = std::chrono::steady_clock::now();
        }
    }

    timerCond_.notify_all();

    for(auto &c : completions)
        c.first(std::move(c.second));
}

void JobQueue::timerMain()
{
    blockEmulatorSignal();

    std::unique_lock lk(mutex_);

    while(!stopped_)
    {
        if(!hasDeadline_)
        {
            timerCond_.wait(lk);
            continue;
        }

        if(timerCond_.wait_until(lk, dueAt_) != std::cv_status::timeout)
            continue;

        hasDeadline_ = false;
        lk.unlock();
        Executor::runOnEmulatorThread([] { JobQueue::instance().pump(); });
        lk.lock();
    }
}
