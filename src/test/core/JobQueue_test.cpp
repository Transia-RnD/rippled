#include <test/jtx/Env.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/insight/NullCollector.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/core/Job.h>
#include <xrpl/core/JobQueue.h>
#include <xrpl/core/PerfLog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xrpl::test {

//------------------------------------------------------------------------------

namespace {

// Minimal PerfLog stub so a JobQueue can be constructed in isolation.
class NullPerfLog : public perf::PerfLog
{
    void
    rpcStart(std::string const&, std::uint64_t) override
    {
    }
    void
    rpcFinish(std::string const&, std::uint64_t) override
    {
    }
    void
    rpcError(std::string const&, std::uint64_t) override
    {
    }
    void
    jobQueue(JobType) override
    {
    }
    void
    jobStart(
        JobType,
        std::chrono::microseconds,
        std::chrono::time_point<std::chrono::steady_clock>,
        int) override
    {
    }
    void
    jobFinish(JobType, std::chrono::microseconds, int) override
    {
    }
    [[nodiscard]] json::Value
    countersJson() const override
    {
        return {};
    }
    [[nodiscard]] json::Value
    currentJson() const override
    {
        return {};
    }
    void
    resizeJobs(int) override
    {
    }
    void
    rotate() override
    {
    }
};

}  // namespace

class JobQueue_test : public beast::unit_test::Suite
{
    // Spin-wait for `pred` up to `timeout`; returns true if it became true.
    template <class Pred>
    static bool
    waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds{10})
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return true;
    }

    // Verifies that reserving worker slots keeps a consensus-critical job from
    // being starved by a flood of long-running non-consensus jobs, and that
    // every job still eventually completes.
    void
    testConsensusReservation()
    {
        testcase("consensus thread reservation");

        using namespace std::chrono_literals;

        Logs logs{beast::Severity::Fatal};
        auto perfLog = std::make_unique<NullPerfLog>();
        auto collector = beast::insight::NullCollector::make();

        int const threadCount = 3;
        int const reserved = 1;  // general = 2 non-consensus slots
        JobQueue jq(threadCount, collector, logs.journal("JobQueueTest"), logs, *perfLog, reserved);

        // Gate that blocks every non-consensus job until released.
        std::mutex gm;
        std::condition_variable gcv;
        bool release = false;

        std::atomic<int> clientsRunning{0};
        std::atomic<int> clientsDone{0};
        std::atomic<bool> consensusRan{false};

        int const clientCount = 6;
        for (int i = 0; i < clientCount; ++i)
        {
            BEAST_EXPECT(jq.addJob(JtClient, "flood", [&]() {
                ++clientsRunning;
                std::unique_lock lk(gm);
                gcv.wait(lk, [&] { return release; });
                lk.unlock();
                ++clientsDone;
            }));
        }

        // Wait until the general (non-reserved) slots are saturated. Only
        // `general` == 2 non-consensus jobs may run at once; the rest wait.
        BEAST_EXPECT(waitFor([&] { return clientsRunning.load() == threadCount - reserved; }));
        // The reservation must prevent a client from stealing the reserved slot.
        BEAST_EXPECT(clientsRunning.load() == threadCount - reserved);
        BEAST_EXPECT(clientsDone.load() == 0);

        // Post a consensus-critical job. It must run on the reserved slot even
        // though every general slot is occupied and more clients are waiting.
        BEAST_EXPECT(jq.addJob(JtNetopTimer, "consensus", [&]() { consensusRan = true; }));

        BEAST_EXPECT(waitFor([&] { return consensusRan.load(); }));
        // Proof of non-starvation: consensus ran while clients are still blocked.
        BEAST_EXPECT(consensusRan.load());
        BEAST_EXPECT(clientsDone.load() == 0);

        // Release the flood; everything must drain with no deadlock.
        {
            std::scoped_lock lk(gm);
            release = true;
        }
        gcv.notify_all();

        BEAST_EXPECT(waitFor([&] { return clientsDone.load() == clientCount; }));
        BEAST_EXPECT(clientsDone.load() == clientCount);

        jq.stop();
    }

    void
    testAddJob()
    {
        jtx::Env env{*this};

        JobQueue& jQueue = env.app().getJobQueue();
        {
            // addJob() should run the Job (and return true).
            std::atomic<bool> jobRan{false};
            BEAST_EXPECT(
                jQueue.addJob(JtClient, "JobAddTest1", [&jobRan]() { jobRan = true; }) == true);

            // Wait for the Job to run.
            while (!jobRan)
                ;
        }
        {
            // If the JobQueue is stopped, we should no
            // longer be able to add Jobs (and calling addJob() should
            // return false).
            using namespace std::chrono_literals;
            jQueue.stop();

            // The Job should never run, so having the Job access this
            // unprotected variable on the stack should be completely safe.
            // Not recommended for the faint of heart...
            bool unprotected = false;
            BEAST_EXPECT(jQueue.addJob(JtClient, "JobAddTest2", [&unprotected]() {
                unprotected = false;
            }) == false);
        }
    }

    void
    testPostCoro()
    {
        jtx::Env env{*this};

        JobQueue& jQueue = env.app().getJobQueue();
        {
            // Test repeated post()s until the Coro completes.
            std::atomic<int> yieldCount{0};
            auto const coro = jQueue.postCoro(
                JtClient,
                "PostCoroTest1",
                [&yieldCount](std::shared_ptr<JobQueue::Coro> const& coroCopy) {
                    while (++yieldCount < 4)
                        coroCopy->yield();
                });
            BEAST_EXPECT(coro != nullptr);

            // Wait for the Job to run and yield.
            while (yieldCount == 0)
                ;

            // Now re-post until the Coro says it is done.
            int old = yieldCount;
            while (coro->runnable())
            {
                BEAST_EXPECT(coro->post());
                while (old == yieldCount)
                {
                }
                coro->join();
                BEAST_EXPECT(++old == yieldCount);
            }
            BEAST_EXPECT(yieldCount == 4);
        }
        {
            // Test repeated resume()s until the Coro completes.
            int yieldCount{0};
            auto const coro = jQueue.postCoro(
                JtClient,
                "PostCoroTest2",
                [&yieldCount](std::shared_ptr<JobQueue::Coro> const& coroCopy) {
                    while (++yieldCount < 4)
                        coroCopy->yield();
                });
            if (!coro)
            {
                // There's no good reason we should not get a Coro, but we
                // can't continue without one.
                BEAST_EXPECT(false);
                return;
            }

            // Wait for the Job to run and yield.
            coro->join();

            // Now resume until the Coro says it is done.
            int old = yieldCount;
            while (coro->runnable())
            {
                coro->resume();  // Resume runs synchronously on this thread.
                BEAST_EXPECT(++old == yieldCount);
            }
            BEAST_EXPECT(yieldCount == 4);
        }
        {
            // If the JobQueue is stopped, we should no
            // longer be able to add a Coro (and calling postCoro() should
            // return false).
            using namespace std::chrono_literals;
            jQueue.stop();

            // The Coro should never run, so having the Coro access this
            // unprotected variable on the stack should be completely safe.
            // Not recommended for the faint of heart...
            bool unprotected = false;
            auto const coro = jQueue.postCoro(
                JtClient, "PostCoroTest3", [&unprotected](std::shared_ptr<JobQueue::Coro> const&) {
                    unprotected = false;
                });
            BEAST_EXPECT(coro == nullptr);
        }
    }

public:
    void
    run() override
    {
        testAddJob();
        testPostCoro();
        testConsensusReservation();
    }
};

BEAST_DEFINE_TESTSUITE(JobQueue, core, xrpl);

}  // namespace xrpl::test
