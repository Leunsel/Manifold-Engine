#pragma once

#include "Core/Errors.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Manifold::Target
{
    class Process;

    std::string AgentFileName(bool is64Bit);
    std::filesystem::path AgentFilePath(bool is64Bit);

    // ∑ Every part that needs the agent library (the debugger, the code filter,
    //   the speedhack, the crash watch) loads it through here, also when it is
    //   loaded already: the load keeps it in place while the part starts, and it
    //   is counted for the agent's lifetime (Target/AgentLifeProtocol.h), which
    //   gives every counted load back once no part needs the agent. No part
    //   frees the library itself any more.
    Status LoadAgent(const Process& process);

    // How long the agent waits, idle, before it unloads (tests make it short).
    void SetAgentGrace(std::uint32_t milliseconds);
    std::uint32_t AgentGrace();
}
