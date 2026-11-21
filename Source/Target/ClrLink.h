#pragma once

#include "Core/Errors.h"
#include "Target/ClrProtocol.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;

    struct ClrRuntimeInfo
    {
        std::string Flavour;      // ".NET" or ".NET Framework"
        std::string Version;      // the runtime module's file version
        std::string RuntimePath;  // coreclr.dll / clr.dll in the game
        std::string DacPath;      // the data access library the helper loaded
        int PointerSize = 8;
        bool TypeStatics = false; // .NET 9+: statics are found per type
    };

    // The program's end of the .NET helper: starts ManifoldClr.exe (or
    // ManifoldClr32.exe for a 32-bit game) next to the program, hands it a
    // read-only handle to the game and talks to it over a named pipe. Every
    // call has a deadline; a helper that misses it is ended, so a stuck DAC
    // can never hang the program. Calls are serialized.
    class ClrLink
    {
    public:
        ClrLink() = default;
        ~ClrLink();
        ClrLink(const ClrLink&) = delete;
        ClrLink& operator=(const ClrLink&) = delete;

        Status Start(const Process& process);
        void Stop();
        bool Running() const;
        const ClrRuntimeInfo& Info() const { return m_info; }

        // Sends a request and returns the reply body after its status byte,
        // or nullopt with the reason in error.
        std::optional<std::vector<std::uint8_t>> Call(ClrProtocol::Writer& request, unsigned timeoutMs, std::string& error);

        static std::filesystem::path HelperPath(bool target32);

    private:
        bool Transfer(void* buffer, std::size_t size, bool write, unsigned timeoutMs);
        void EndHelper();

        mutable std::mutex m_lock;
        void* m_pipe = nullptr;
        void* m_event = nullptr;
        void* m_helper = nullptr;
        ClrRuntimeInfo m_info;
    };
}
