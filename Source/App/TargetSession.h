#pragma once

#include "Target/CodeMap.h"
#include "Target/Mono.h"
#include "Target/PdbSymbols.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Manifold
{
    class TargetSession
    {
    public:
        TargetSession() = default;

        TargetSession(const TargetSession&) = delete;
        TargetSession& operator=(const TargetSession&) = delete;

        const std::shared_ptr<Target::Process>& Process() const { return m_process; }
        bool Attached() const { return m_process != nullptr; }
        std::uint64_t Generation() const { return m_generation; }
        Target::Symbols& Symbols() { return m_symbols; }
        const Target::Symbols& Symbols() const { return m_symbols; }
        Target::PdbSymbols& Pdb() { return m_pdb; }
        const Target::PdbSymbols& Pdb() const { return m_pdb; }
        Target::MonoBridge& Mono() { return m_mono; }
        const Target::MonoBridge& Mono() const { return m_mono; }
        Target::CodeMap& CodeMap() { return m_codeMap; }
        const Target::CodeMap& CodeMap() const { return m_codeMap; }
        double SymbolsStarted() const { return m_symbolsStarted; }

        void Attach(std::shared_ptr<Target::Process> process);
        void Reattach();
        void Detach();
        bool LoadSymbols(bool windowsSymbols, double now);
        void RefreshSymbolProvider();
        void AddExtraSymbols(Target::SymbolForward forward);

    private:
        std::shared_ptr<Target::Process> m_process;
        std::uint64_t m_generation = 0;
        Target::Symbols m_symbols;
        Target::PdbSymbols m_pdb;
        Target::MonoBridge m_mono;
        Target::CodeMap m_codeMap;
        bool m_pdbActive = false;
        std::vector<Target::SymbolForward> m_extraSymbols;
        double m_symbolsStarted = 0.0;
    };
}
