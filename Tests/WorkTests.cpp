#include "TestKit.h"

#include "Core/BackgroundWork.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace Manifold::Tests
{
    namespace
    {
        void SettleWork(BackgroundWork& work, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (work.Busy() && std::chrono::steady_clock::now() < until)
            {
                work.Drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            work.Drain();
        }

        void FinishOnCallerThread()
        {
            BackgroundWork work;
            const std::thread::id caller = std::this_thread::get_id();
            std::thread::id jobThread;
            std::thread::id finishThread;
            int result = 0;
            const int id = work.Start({ "Adding", "math", true, false }, [&](WorkProgress& progress) -> BackgroundWork::Finish
                {
                    jobThread = std::this_thread::get_id();
                    progress.SetText("Adding numbers");
                    progress.SetSteps(0, 10);
                    int sum = 0;
                    for (int i = 1; i <= 10; ++i)
                    {
                        sum += i;
                        progress.Step();
                    }
                    return [&, sum]()
                        {
                            finishThread = std::this_thread::get_id();
                            result = sum;
                        };
                });
            CHECK(id > 0);
            CHECK(work.Running(id));
            CHECK(work.Busy("math"));
            CHECK_EQ(work.Find("math"), id);
            CHECK_EQ(result, 0);
            SettleWork(work, 2000);
            CHECK_EQ(result, 55);
            CHECK(jobThread != caller);
            CHECK(finishThread == caller);
            CHECK(!work.Running(id));
            CHECK(!work.Busy());
        }

        void ListAndCancel()
        {
            BackgroundWork work;
            std::atomic<bool> started{ false };
            bool cancelledSeen = false;
            const int id = work.Start({ "Waiting", "wait", true, false }, [&](WorkProgress& progress) -> BackgroundWork::Finish
                {
                    progress.SetText("Waiting for a cancel");
                    progress.SetSteps(3, 4);
                    started = true;
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!progress.Cancelled() && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    const bool cancelled = progress.Cancelled();
                    return [&, cancelled]() { cancelledSeen = cancelled; };
                });
            const int quiet = work.Start({ "Hidden", "quiet", false, true }, [](WorkProgress&) -> BackgroundWork::Finish { return {}; });
            while (!started)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto info = work.Info("wait");
            CHECK(info.has_value());
            if (info)
            {
                CHECK_EQ(info->Id, id);
                CHECK_EQ(info->Title, std::string("Waiting"));
                CHECK_EQ(info->Text, std::string("Waiting for a cancel"));
                CHECK(info->Fraction > 0.74 && info->Fraction < 0.76);
                CHECK(info->Cancellable);
                CHECK(!info->Cancelling);
            }
            CHECK(!work.Info("quiet").has_value());
            for (const WorkInfo& listed : work.List())
                CHECK(listed.Id != quiet);
            work.Cancel(id);
            const auto cancelling = work.Info("wait");
            CHECK(!cancelling || cancelling->Cancelling);
            SettleWork(work, 3000);
            CHECK(cancelledSeen);
            CHECK(!work.Busy());
        }

        void CompleteAndStop()
        {
            BackgroundWork work;
            int finished = 0;
            work.Start({ "Slow", "slow", true, false }, [&](WorkProgress&) -> BackgroundWork::Finish
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    return [&]() { ++finished; };
                });
            work.CompleteGroup("slow");
            CHECK_EQ(finished, 1);
            CHECK(!work.Busy("slow"));
            work.CompleteGroup("slow");
            CHECK_EQ(finished, 1);

            int dropped = 0;
            std::atomic<bool> sawCancel{ false };
            work.Start({ "Endless", "endless", true, false }, [&](WorkProgress& progress) -> BackgroundWork::Finish
                {
                    while (!progress.Cancelled())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    sawCancel = true;
                    return [&]() { ++dropped; };
                });
            work.Stop();
            CHECK(sawCancel.load());
            CHECK_EQ(dropped, 0);
            CHECK(!work.Busy());
            CHECK_EQ(work.Start({ "After stop", "late", false, false }, [](WorkProgress&) -> BackgroundWork::Finish { return {}; }), 0);
        }

        void FailingJob()
        {
            BackgroundWork work;
            work.Start({ "Throws", "throw", false, false }, [](WorkProgress&) -> BackgroundWork::Finish { throw std::runtime_error("expected in the test"); });
            SettleWork(work, 2000);
            CHECK(!work.Busy());
        }
    }

    int RunWorkTests()
    {
        const int before = Totals().Failures;
        FinishOnCallerThread();
        ListAndCancel();
        CompleteAndStop();
        FailingJob();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("work", "Background work", 20, [](const SuiteContext&) { return RunWorkTests(); });
}
