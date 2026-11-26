#pragma once

#include "Core/Errors.h"
#include "Target/Process.h"
#include "Target/Rebase.h"

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
        std::uint64_t Size = 0;
    };

    struct ExportSymbol
    {
        std::uint64_t Address = 0;
        // The name in the export table (_wtoi), or OrdinalN for an export that
        // has only an ordinal.
        std::string Name;
        // The name Cheat Engine shows for it (wtoi), see CheatEngineExportName.
        std::string Shown;
        // How many bytes past Address the name still reaches, Cheat Engine's
        // symbol size: up to the next export, at most 64 KB, 0 for the last.
        std::uint64_t Reach = 0;
    };

    // The name Cheat Engine gives an export (what dbghelp makes of it with
    // SYMOPT_UNDNAME): C++ names undecorated to the bare name
    // (?_Xlength_error@std@@YAXPEBD@Z -> std::_Xlength_error), C names without
    // one leading _, @ or ? and without their @-decoration (_wtoi -> wtoi,
    // _Sleep@4 -> Sleep). Empty for a name Cheat Engine leaves out.
    std::string CheatEngineExportName(std::string_view raw);

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

    enum class AddressStyle
    {
        Name,
        ModuleOffset,
        Plain,
        Rebased
    };

    class Symbols
    {
    public:
        void Attach(std::shared_ptr<Process> process);
        void Detach();
        const std::shared_ptr<Process>& Target() const { return m_process; }

        Status Register(std::string_view name, std::uint64_t address, std::string_view owner = {}, bool replace = true);
        bool Unregister(std::string_view name);
        bool SetSize(std::string_view name, std::uint64_t size);
        std::optional<std::string> RegisteredName(std::uint64_t address) const;
        std::uint64_t RegisteredSize(std::uint64_t address) const;
        void UnregisterOwner(std::string_view owner);
        std::optional<std::uint64_t> FindRegistered(std::string_view name) const;
        std::vector<UserSymbol> Registered() const;
        std::uint64_t Generation() const { return m_generation.load(); }

        std::optional<std::uint64_t> Resolve(std::string_view name) const;
        std::uint64_t ResolveExport(std::string_view module, std::string_view name) const;
        std::string Describe(std::uint64_t address) const;
        std::string ModuleOffset(std::uint64_t address) const;
        std::string AddressText(std::uint64_t address, AddressStyle style) const;
        // The address as a rebased module shows it (see Rebases), or nullopt
        // outside every rebased module.
        std::optional<std::uint64_t> Rebased(std::uint64_t address) const;
        std::optional<RebaseHit> FromRebased(std::uint64_t shown) const;
        Rebases& Rebase() { return m_rebases; }
        const Rebases& Rebase() const { return m_rebases; }
        std::optional<std::string> ExactName(std::uint64_t address) const;
        // The closest name at or below an address, at most reach bytes away:
        // a registered symbol, a module base or an export (what ExactName
        // names). Gives the function a disassembly line is in.
        std::optional<std::pair<std::uint64_t, std::string>> Containing(std::uint64_t address, std::uint64_t reach) const;
        std::optional<std::uint64_t> RegisteredIn(std::uint64_t from, std::uint64_t to) const;
        std::optional<std::uint64_t> RegisteredBelow(std::uint64_t address, std::uint64_t floor) const;
        std::optional<std::uint64_t> ExportIn(std::uint64_t from, std::uint64_t to) const;
        std::optional<std::uint64_t> ExportBelow(std::uint64_t address, std::uint64_t floor) const;
        bool LabelOnly(std::uint64_t address) const;
        std::vector<ExportSymbol> ExportsOf(const ModuleInfo& module) const;

        Evaluation Evaluate(std::string_view expression, const ExtraResolver& extra = {}) const;
        void SetLookupCallback(LookupCallback callback) { m_lookup = std::move(callback); }
        void SetSymbolProvider(SymbolForward forward, SymbolReverse reverse);

        static bool IsNameChar(char c);

    private:
        struct ExportTable
        {
            std::vector<ExportSymbol> ByAddress;
            // Lower-case export table names, and the shown names that differ
            // from them: a name in the table always wins over a shown one.
            std::unordered_map<std::string, std::uint64_t> ByName;
            std::unordered_map<std::string, std::uint64_t> ByShown;
        };

        std::shared_ptr<const ExportTable> Exports(const ModuleInfo& module) const;
        static const ExportSymbol* ExportAtOrBelow(const ExportTable& table, std::uint64_t address);
        std::optional<std::uint64_t> ResolveName(std::string_view name, const ExtraResolver& extra) const;
        std::optional<std::uint64_t> ResolveBuiltin(std::string_view name, const ExtraResolver& extra) const;

        std::shared_ptr<Process> m_process;
        Rebases m_rebases;
        LookupCallback m_lookup;
        const UserSymbol* RegisteredAt(std::uint64_t address) const;
        const UserSymbol* SizedAt(std::uint64_t address) const;
        void ForgetAddress(std::uint64_t address, const std::string& key);

        std::map<std::string, UserSymbol> m_registered;
        std::multimap<std::uint64_t, std::string> m_registeredByAddress;
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
