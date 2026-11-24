#pragma once

#include "Core/Errors.h"
#include "Target/CrashProtocol.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Manifold::Target
{
    class Process;

    // One exception the agent noted, as the host reads it from the ring.
    struct CrashRecord
    {
        std::uint64_t Number = 0;
        std::uint32_t Code = 0;
        std::uint32_t Flags = 0;
        std::uint32_t ThreadId = 0;
        std::uint64_t Tick = 0;
        std::uint64_t Address = 0;
        std::vector<std::uint64_t> Info;
        std::array<std::uint64_t, Crash::kRegisters> Registers{};
        std::vector<std::uint64_t> Stack;
        std::vector<std::uint8_t> Bytes;

        std::uint64_t Ip() const { return Registers[Crash::RegIp]; }
        std::uint64_t Sp() const { return Registers[Crash::RegSp]; }
    };

    struct CrashCapture
    {
        std::vector<CrashRecord> Records;  // oldest first
        std::uint64_t Noted = 0;           // every record the agent ever wrote, also the ones the ring dropped
        bool Is64Bit = true;
        bool Watching = false;
    };

    // The crash watch's part in the process: the agent library notes the
    // exceptions that end a process in a ring that lives in a section this
    // program owns, so it can still be read after the process died.
    class CrashWatch
    {
    public:
        CrashWatch() = default;
        ~CrashWatch();

        CrashWatch(const CrashWatch&) = delete;
        CrashWatch& operator=(const CrashWatch&) = delete;

        // Takes over the agent of an earlier session when it already watches.
        void Attach(std::shared_ptr<Process> process);
        // Stops the agent (while the process lives) and drops the ring.
        void Leave();
        // Loads the agent when needed and starts it on a fresh ring.
        Status Start();
        bool Running() const;
        // The ring as it is now; works after the process ended.
        CrashCapture Capture() const;

    private:
        Status Call(std::uint32_t operation);
        Status MapRing();
        void Unmap();

        mutable std::mutex m_lock;
        std::shared_ptr<Process> m_process;
        void* m_mapping = nullptr;
        Crash::Ring* m_ring = nullptr;
        std::uint64_t m_block = 0;
        std::uint64_t m_call = 0;
        std::uint32_t m_serial = 0;
        bool m_running = false;
    };
}
