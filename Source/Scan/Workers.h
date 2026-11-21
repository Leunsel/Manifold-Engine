#pragma once

#include "Core/MainThread.h"

#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Manifold::Scan
{
    int WorkerCount(int wanted);
    void PreferFastCores();

    template <class Work>
    void RunWorkers(int threads, const wchar_t* name, Work&& work)
    {
        if (threads <= 1)
        {
            PreferFastCores();
            work(0);
            return;
        }
        // An exception must not leave a worker thread (that ends the program); the first one
        // is thrown again on the calling thread once every worker has stopped.
        std::mutex failedLock;
        std::exception_ptr failed;
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        try
        {
            for (int index = 0; index < threads; ++index)
            {
                pool.emplace_back([&work, &failedLock, &failed, name, index]
                    {
                        try
                        {
                            SetThreadName(name);
                            PreferFastCores();
                            work(index);
                        }
                        catch (...)
                        {
                            std::lock_guard lock(failedLock);
                            if (!failed)
                                failed = std::current_exception();
                        }
                    });
            }
        }
        catch (...)
        {
            for (std::thread& thread : pool)
                thread.join();
            throw;
        }
        for (std::thread& thread : pool)
            thread.join();
        if (failed)
            std::rethrow_exception(failed);
    }
}
