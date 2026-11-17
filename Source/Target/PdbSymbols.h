#pragma once

#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Manifold::Target
{
    class PdbSymbols
    {
    public:
        PdbSymbols() = default;
        ~PdbSymbols();

        PdbSymbols(const PdbSymbols&) = delete;
        PdbSymbols& operator=(const PdbSymbols&) = delete;

        bool Begin(void* processHandle, std::vector<ModuleInfo> modules, bool windowsSymbols);
        void End();
        void Cancel() { m_stop = true; }
        bool Stopping() const { return m_stop.load(); }
        bool Active() const { return m_active.load(); }

        std::optional<std::uint64_t> Resolve(std::string_view name) const;
        std::optional<std::string> Describe(std::uint64_t address) const;

        bool Loaded() const { return m_loaded.load(); }
        bool WaitUntilLoaded(unsigned timeoutMs);
        int ModulesDone() const { return m_done.load(); }
        int ModulesTotal() const { return m_total.load(); }

    private:
        struct Named
        {
            std::uint64_t Address;
            std::string Name;
        };

        void Worker(std::vector<ModuleInfo> modules, bool windowsSymbols);

        std::thread m_thread;
        std::atomic<bool> m_active{ false };
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_loaded{ false };
        std::atomic<bool> m_any{ false };
        std::atomic<int> m_done{ 0 };
        std::atomic<int> m_total{ 0 };

        void* m_handle = nullptr;

        mutable std::mutex m_lock;
        std::unordered_map<std::string, std::uint64_t> m_byName;
        std::vector<Named> m_byAddress;
    };
}
