#pragma once

#include "Core/Errors.h"
#include "Target/Process.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Manifold::Target
{
    struct SpeedhackHook
    {
        std::uint64_t Address = 0;
        std::uint64_t Page = 0;
        std::uint32_t Source = 0;
        std::string Name;
        std::vector<std::uint8_t> Original;
        std::vector<std::uint8_t> Patch;
    };

    class Speedhack
    {
    public:
        static constexpr double kMaxSpeed = 1000.0;

        Speedhack() = default;
        ~Speedhack();

        Speedhack(const Speedhack&) = delete;
        Speedhack& operator=(const Speedhack&) = delete;

        void Attach(std::shared_ptr<Process> process);
        void Leave();

        Status SetSpeed(double speed);
        Status Disable();
        Status Remove();

        bool Enabled() const;
        bool Installed() const;
        bool ClockShifted() const;
        double Speed() const;
        std::vector<SpeedhackHook> Hooks() const;
        bool Owns(std::uint64_t address, std::uint64_t size) const;
        std::string AgentName() const;
        static std::string Text(double speed);

    private:
        Status Install();
        Status HookMissing();
        Status WriteSpeed(double speed);
        Status RemoveLocked();
        Status Watch();
        Status Call(std::uint32_t operation);
        bool Adopt();
        bool Usable() const;
        void DropAgent();
        void Forget();

        mutable std::mutex m_lock;
        std::shared_ptr<Process> m_process;
        std::uint64_t m_block = 0;
        std::uint64_t m_call = 0;
        std::uint64_t m_imageBase = 0;
        std::uint64_t m_imageSize = 0;
        std::uint64_t m_entries[4] = {};
        std::vector<SpeedhackHook> m_hooks;
        std::vector<std::uint64_t> m_pages;
        std::uint32_t m_serial = 0;
        double m_speed = 1.0;
        bool m_installed = false;
        bool m_enabled = false;
        bool m_shifted = false;
    };
}
