#include "App/TargetSession.h"

#include <vector>

namespace Manifold
{
    void TargetSession::Attach(std::shared_ptr<Target::Process> process)
    {
        m_process = std::move(process);
        m_symbols.Attach(m_process);
        m_codeMap.Attach(m_process, &m_mono);
        ++m_generation;
    }

    void TargetSession::Reattach()
    {
        if (!m_process)
            return;
        m_process->RefreshModules();
        m_symbols.Attach(m_process);
        m_codeMap.Attach(m_process, &m_mono);
    }

    void TargetSession::Detach()
    {
        m_mono.Stop();
        m_pdbActive = false;
        m_symbols.SetSymbolProvider({}, {});
        m_pdb.End();
        m_symbols.Detach();
        m_codeMap.Detach();
        m_process.reset();
        ++m_generation;
    }

    bool TargetSession::LoadSymbols(bool windowsSymbols, double now)
    {
        if (!m_process)
            return false;
        m_process->RefreshModules();
        std::vector<Target::ModuleInfo> list;
        if (const auto modules = m_process->Modules())
            list.assign(modules->begin(), modules->end());
        if (!m_pdb.Begin(m_process->Handle(), std::move(list), windowsSymbols))
            return false;
        m_symbolsStarted = now;
        m_pdbActive = true;
        RefreshSymbolProvider();
        return true;
    }

    void TargetSession::RefreshSymbolProvider()
    {
        const bool pdb = m_pdbActive;
        if (!pdb && !m_mono.Present())
        {
            m_symbols.SetSymbolProvider({}, {});
            return;
        }
        m_symbols.SetSymbolProvider(
            [this, pdb](std::string_view name) -> std::optional<std::uint64_t>
            {
                if (pdb)
                    if (const auto address = m_pdb.Resolve(name))
                        return address;
                if (m_mono.Present())
                    if (const auto address = m_mono.AddressOf(name))
                        return address;
                return std::nullopt;
            },
            [this, pdb](std::uint64_t address) -> std::optional<std::string>
            {
                if (pdb)
                    if (const auto name = m_pdb.Describe(address))
                        return name;
                // JIT-compiled Mono methods live outside every loaded PE module
                // (the runtime allocates its own code pages), so only for an
                // address in no module is a Mono lookup worth the remote-call
                // cost; the common in-module path never touches the runtime, and
                // MethodNameAt caches so repeat lines are free.
                if (m_mono.Present() && m_process && !m_process->ModuleAt(address))
                    if (const auto name = m_mono.MethodNameAt(address))
                        return name;
                return std::nullopt;
            });
    }
}
