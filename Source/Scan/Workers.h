#pragma once

#include "Core/MainThread.h"

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
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        for (int index = 0; index < threads; ++index)
        {
            pool.emplace_back([&work, name, index]
                {
                    SetThreadName(name);
                    PreferFastCores();
                    work(index);
                });
        }
        for (std::thread& thread : pool)
            thread.join();
    }
}
