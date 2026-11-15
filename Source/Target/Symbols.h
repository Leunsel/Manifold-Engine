#pragma once

#include "Core/Errors.h"
#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::Target
{
    struct UserSymbol
    {
        std::string Name;
        std::uint64_t Address = 0;
        std::string Owner;
    };

    struct ExportSymbol
    {
        std::uint64_t Address = 0;
        std::string Name;
    };

    struct Evaluation
    {
        std::optional<std::uint64_t> Value;
        Status Error;
        bool UsedPointer = false;

        bool Ok() const { return Value.has_value(); }
    };

    using ExtraResolver = std::function<std::optional<std::uint64_t>(std::string_view name)>;
    using LookupCallback = std::function<std::optional<std::uint64_t>(std::string_view name, bool first)>;
    using SymbolForward = std::function<std::optional<std::uint64_t>(std::string_view name)>;
    using SymbolReverse = std::function<std::optional<std::string>(std::uint64_t address)>;

    class Symbols
    {
    public:
        void Attach(std::shared_ptr<Process> process);
        void Detach();
        const std::shared_ptr<Process>& Target() const { return m_process; }

        Status Register(std::string_view name, std::uint64_t address, std::string_view owner = {}, bool replace = true);
        bool Unregister(std::string_view name);
        void UnregisterOwner(std::string_view owner);
        std::optional<std::uint64_t> FindRegistered(std::string_view name) const;
        std::vector<UserSymbol> Registered() const;
        std::uint64_t Generation() const { return m_generation.load(); }

        std::optional<std::uint64_t> Resolve(std::string_view name) const;
        std::uint64_t ResolveExport(std::string_view module, std::string_view name) const;
        std::string Describe(std::uint64_t address) const;
        std::string ModuleOffset(std::uint64_t address) const;
        std::optional<std::string> ExactName(std::uint64_t address) const;
        std::vector<ExportSymbol> ExportsOf(const ModuleInfo& module) const;

        Evaluation Evaluate(std::string_view expression, const ExtraResolver& extra = {}) const;
        void SetLookupCallback(LookupCallback callback) { m_lookup = std::move(callback); }
        void SetSymbolProvider(SymbolForward forward, SymbolReverse reverse);

        static bool IsNameChar(char c);

    private:
        struct ExportTable
        {
            std::vector<ExportSymbol> ByAddress;
            std::unordered_map<std::string, std::uint64_t> ByName;
        };

        std::shared_ptr<const ExportTable> Exports(const ModuleInfo& module) const;
        std::optional<std::uint64_t> ResolveName(std::string_view name, const ExtraResolver& extra) const;
        std::optional<std::uint64_t> ResolveBuiltin(std::string_view name, const ExtraResolver& extra) const;

        std::shared_ptr<Process> m_process;
        LookupCallback m_lookup;
        std::map<std::string, UserSymbol> m_registered;
        mutable std::mutex m_registeredLock;
        std::atomic<std::uint64_t> m_generation = 1;
        mutable std::mutex m_exportLock;
        mutable std::unordered_map<std::uint64_t, std::shared_ptr<ExportTable>> m_exports;
        std::atomic<bool> m_hasProvider{ false };
        mutable std::mutex m_providerLock;
        SymbolForward m_forward;
        SymbolReverse m_reverse;
    };
}
