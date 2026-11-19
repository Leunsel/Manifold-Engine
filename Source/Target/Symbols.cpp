#include "Target/Symbols.h"

#include "Core/Text.h"

#include <algorithm>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kExportReach = 0x2000;

        class Parser
        {
        public:
            Parser(std::string_view text, const Symbols& symbols, const ExtraResolver& extra, const std::shared_ptr<Process>& process)
                : m_text(text), m_symbols(symbols), m_extra(extra), m_process(process)
            {
            }

            Evaluation Run()
            {
                Evaluation result;
                SkipSpace();
                if (m_at >= m_text.size())
                {
                    result.Error = Status::Fail(ErrorCode::ExprSyntax, m_text);
                    return result;
                }
                const std::optional<std::uint64_t> value = Expression();
                SkipSpace();
                if (!m_error.Ok())
                {
                    result.Error = m_error;
                    return result;
                }
                if (!value || m_at != m_text.size())
                {
                    result.Error = Status::Fail(ErrorCode::ExprSyntax, m_text);
                    return result;
                }
                result.Value = *value;
                result.UsedPointer = m_usedPointer;
                return result;
            }

        private:
            void SkipSpace()
            {
                while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t'))
                    ++m_at;
            }

            bool Take(char c)
            {
                SkipSpace();
                if (m_at < m_text.size() && m_text[m_at] == c)
                {
                    ++m_at;
                    return true;
                }
                return false;
            }

            std::optional<std::uint64_t> Expression()
            {
                std::optional<std::uint64_t> value = Term();
                while (value)
                {
                    if (Take('+'))
                    {
                        const auto right = Term();
                        if (!right)
                            return std::nullopt;
                        *value += *right;
                    }
                    else if (Take('-'))
                    {
                        const auto right = Term();
                        if (!right)
                            return std::nullopt;
                        *value -= *right;
                    }
                    else
                    {
                        break;
                    }
                }
                return value;
            }

            std::optional<std::uint64_t> Term()
            {
                std::optional<std::uint64_t> value = Factor();
                while (value)
                {
                    if (Take('*'))
                    {
                        const auto right = Factor();
                        if (!right)
                            return std::nullopt;
                        *value *= *right;
                    }
                    else if (Take('/'))
                    {
                        const auto right = Factor();
                        if (!right)
                            return std::nullopt;
                        if (*right == 0)
                        {
                            Fail(Status::Fail(ErrorCode::ExprDivideByZero, m_text));
                            return std::nullopt;
                        }
                        *value /= *right;
                    }
                    else
                    {
                        break;
                    }
                }
                return value;
            }

            std::optional<std::uint64_t> Factor()
            {
                SkipSpace();
                if (m_at >= m_text.size())
                    return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));

                const char c = m_text[m_at];
                if (c == '[')
                {
                    ++m_at;
                    const auto inner = Expression();
                    if (!inner || !Take(']'))
                        return inner ? Fail(Status::Fail(ErrorCode::ExprSyntax, m_text)) : std::nullopt;
                    if (!m_process)
                        return Fail(Status::Fail(ErrorCode::NoProcess));
                    m_usedPointer = true;
                    const auto pointer = m_process->ReadPointer(*inner);
                    if (!pointer)
                        return Fail(Status::Fail(ErrorCode::ExprPointerUnreadable, Hex(*inner)));
                    return pointer;
                }
                if (c == '(')
                {
                    ++m_at;
                    const auto inner = Expression();
                    if (!inner || !Take(')'))
                        return inner ? Fail(Status::Fail(ErrorCode::ExprSyntax, m_text)) : std::nullopt;
                    return inner;
                }
                if (c == '-')
                {
                    ++m_at;
                    const auto inner = Factor();
                    if (!inner)
                        return std::nullopt;
                    return ~*inner + 1;
                }
                if (c == '+')
                {
                    ++m_at;
                    return Factor();
                }
                if (c == '#')
                {
                    ++m_at;
                    const std::size_t start = m_at;
                    while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
                        ++m_at;
                    const auto value = ParseDecimal(m_text.substr(start, m_at - start));
                    if (!value)
                        return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));
                    return static_cast<std::uint64_t>(*value);
                }
                if (c == '"' || c == '\'')
                {
                    const std::size_t close = m_text.find(c, m_at + 1);
                    if (close == std::string_view::npos)
                        return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));
                    const std::string_view name = m_text.substr(m_at + 1, close - m_at - 1);
                    m_at = close + 1;
                    return Name(name, false);
                }
                if (c == '$' && m_at + 1 < m_text.size() && IsHexDigit(m_text[m_at + 1]))
                {
                    ++m_at;
                    return Number();
                }
                if (Symbols::IsNameChar(c))
                {
                    if (const auto managed = ManagedName())
                        return managed;
                    std::string module;
                    if (const std::size_t length = SpecialModule(module))
                    {
                        m_at += length;
                        const std::size_t suffix = m_at;
                        while (m_at < m_text.size() && Symbols::IsNameChar(m_text[m_at]))
                            ++m_at;
                        module += m_text.substr(suffix, m_at - suffix);
                        return Name(module, false);
                    }
                    const std::size_t start = m_at;
                    while (m_at < m_text.size() && Symbols::IsNameChar(m_text[m_at]))
                        ++m_at;
                    const std::string_view word = m_text.substr(start, m_at - start);
                    if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
                    {
                        const auto value = ParseHex(word);
                        if (!value)
                            return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));
                        return value;
                    }
                    return Name(word, true);
                }
                return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));
            }

            // A managed method named with its assembly, Assembly-CSharp:Player:TakeDamage:
            // the dash belongs to the assembly name when a ':' follows it in the
            // same word. Taken only when the whole word resolves, so a - b:c
            // still subtracts.
            std::optional<std::uint64_t> ManagedName()
            {
                std::size_t end = m_at;
                while (end < m_text.size() && (Symbols::IsNameChar(m_text[end]) || m_text[end] == '-'))
                    ++end;
                const std::string_view word = m_text.substr(m_at, end - m_at);
                const std::size_t dash = word.find('-');
                const std::size_t colon = word.find(':');
                if (dash == std::string_view::npos || colon == std::string_view::npos || dash > colon || word.back() == '-')
                    return std::nullopt;
                if (const auto value = m_symbols.Resolve(word))
                {
                    m_at = end;
                    return value;
                }
                return std::nullopt;
            }

            std::size_t SpecialModule(std::string& matched) const
            {
                if (!m_process)
                    return 0;
                const ModuleList modules = m_process->Modules();
                if (!modules)
                    return 0;
                const std::string_view rest = m_text.substr(m_at);
                std::size_t best = 0;
                for (const ModuleInfo& module : *modules)
                {
                    if (module.Name.find_first_of("- ") == std::string::npos)
                        continue;
                    const std::size_t dot = module.Name.rfind('.');
                    for (const std::string_view candidate : { std::string_view(module.Name), std::string_view(module.Name).substr(0, dot == std::string::npos ? module.Name.size() : dot) })
                    {
                        if (candidate.size() <= best || candidate.size() > rest.size() || !EqualsNoCase(rest.substr(0, candidate.size()), candidate))
                            continue;
                        const char next = candidate.size() < rest.size() ? rest[candidate.size()] : '\0';
                        if (next != '\0' && next != '!' && Symbols::IsNameChar(next))
                            continue;
                        best = candidate.size();
                        matched = module.Name;
                    }
                }
                return best;
            }

            std::optional<std::uint64_t> Number()
            {
                const std::size_t start = m_at;
                while (m_at < m_text.size() && IsHexDigit(m_text[m_at]))
                    ++m_at;
                const auto value = ParseHex(m_text.substr(start, m_at - start));
                if (!value)
                    return Fail(Status::Fail(ErrorCode::ExprSyntax, m_text));
                return value;
            }

            std::optional<std::uint64_t> Name(std::string_view word, bool mayBeNumber)
            {
                const bool hexLooking = mayBeNumber && !word.empty() && word.size() <= 16 &&
                                        std::all_of(word.begin(), word.end(), [](char ch) { return IsHexDigit(ch); });
                if (hexLooking)
                {
                    if (const auto registered = m_symbols.FindRegistered(word))
                        return registered;
                    if (m_extra)
                    {
                        if (const auto value = m_extra(word))
                            return value;
                    }
                    return ParseHex(word);
                }
                if (m_extra)
                {
                    if (const auto value = m_extra(word))
                        return value;
                }
                if (const auto value = m_symbols.Resolve(word))
                    return value;
                return Fail(Status::Fail(ErrorCode::ExprUnknownSymbol, word));
            }

            std::optional<std::uint64_t> Fail(Status status)
            {
                if (m_error.Ok())
                    m_error = std::move(status);
                return std::nullopt;
            }

            std::string_view m_text;
            const Symbols& m_symbols;
            const ExtraResolver& m_extra;
            const std::shared_ptr<Process>& m_process;
            std::size_t m_at = 0;
            Status m_error;
            bool m_usedPointer = false;
        };
    }

    bool Symbols::IsNameChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '!' || c == '@' || c == '?' ||
               c == ':' || static_cast<unsigned char>(c) >= 0x80;
    }

    void Symbols::Attach(std::shared_ptr<Process> process)
    {
        m_process = std::move(process);
        std::lock_guard lock(m_exportLock);
        m_exports.clear();
        ++m_generation;
    }

    void Symbols::Detach()
    {
        m_process.reset();
        {
            std::lock_guard registered(m_registeredLock);
            m_registered.clear();
            m_registeredByAddress.clear();
        }
        std::lock_guard lock(m_exportLock);
        m_exports.clear();
        ++m_generation;
    }

    Status Symbols::Register(std::string_view name, std::uint64_t address, std::string_view owner, bool replace)
    {
        const std::string key = Lower(Trim(name));
        if (key.empty())
            return Status::Fail(ErrorCode::ExprSyntax, name);
        std::lock_guard lock(m_registeredLock);
        auto found = m_registered.find(key);
        if (found != m_registered.end() && !replace && found->second.Address != address)
            return Status::Fail(ErrorCode::AaSymbolTaken, name);
        if (found != m_registered.end())
            ForgetAddress(found->second.Address, key);
        m_registered[key] = UserSymbol{ Trim(name), address, std::string(owner) };
        m_registeredByAddress.emplace(address, key);
        ++m_generation;
        return Status::Success();
    }

    bool Symbols::Unregister(std::string_view name)
    {
        std::lock_guard lock(m_registeredLock);
        const auto found = m_registered.find(Lower(Trim(name)));
        if (found == m_registered.end())
            return false;
        ForgetAddress(found->second.Address, found->first);
        m_registered.erase(found);
        ++m_generation;
        return true;
    }

    void Symbols::UnregisterOwner(std::string_view owner)
    {
        std::lock_guard lock(m_registeredLock);
        for (auto it = m_registered.begin(); it != m_registered.end();)
        {
            if (it->second.Owner == owner)
            {
                ForgetAddress(it->second.Address, it->first);
                it = m_registered.erase(it);
            }
            else
            {
                ++it;
            }
        }
        ++m_generation;
    }

    void Symbols::ForgetAddress(std::uint64_t address, const std::string& key)
    {
        const auto [first, last] = m_registeredByAddress.equal_range(address);
        for (auto it = first; it != last; ++it)
        {
            if (it->second == key)
            {
                m_registeredByAddress.erase(it);
                return;
            }
        }
    }

    const UserSymbol* Symbols::RegisteredAt(std::uint64_t address) const
    {
        const auto [first, last] = m_registeredByAddress.equal_range(address);
        const std::string* best = nullptr;
        for (auto it = first; it != last; ++it)
        {
            if (!best || it->second < *best)
                best = &it->second;
        }
        if (!best)
            return nullptr;
        const auto found = m_registered.find(*best);
        return found == m_registered.end() ? nullptr : &found->second;
    }

    std::optional<std::uint64_t> Symbols::FindRegistered(std::string_view name) const
    {
        std::lock_guard lock(m_registeredLock);
        const auto found = m_registered.find(Lower(name));
        if (found == m_registered.end())
            return std::nullopt;
        return found->second.Address;
    }

    std::vector<UserSymbol> Symbols::Registered() const
    {
        std::lock_guard lock(m_registeredLock);
        std::vector<UserSymbol> list;
        list.reserve(m_registered.size());
        for (const auto& [key, symbol] : m_registered)
            list.push_back(symbol);
        return list;
    }

    std::shared_ptr<const Symbols::ExportTable> Symbols::Exports(const ModuleInfo& module) const
    {
        {
            std::lock_guard lock(m_exportLock);
            if (const auto found = m_exports.find(module.Base); found != m_exports.end())
                return found->second;
        }
        auto table = std::make_shared<ExportTable>();
        if (m_process)
        {
            const Process& process = *m_process;
            IMAGE_DOS_HEADER dos{};
            DWORD exportRva = 0;
            DWORD exportSize = 0;
            if (process.ReadExact(module.Base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE)
            {
                if (process.Is64Bit())
                {
                    IMAGE_NT_HEADERS64 nt{};
                    if (process.ReadExact(module.Base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE)
                    {
                        exportRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                        exportSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
                    }
                }
                else
                {
                    IMAGE_NT_HEADERS32 nt{};
                    if (process.ReadExact(module.Base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE)
                    {
                        exportRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                        exportSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
                    }
                }
            }
            if (exportRva != 0 && exportSize >= sizeof(IMAGE_EXPORT_DIRECTORY) && exportSize < 64 * 1024 * 1024)
            {
                std::vector<std::uint8_t> block(exportSize);
                if (process.Read(module.Base + exportRva, block.data(), block.size()) == block.size())
                {
                    const auto* directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(block.data());
                    const auto inBlock = [&](DWORD rva, std::size_t bytes) { return rva >= exportRva && rva - exportRva + bytes <= block.size(); };
                    std::vector<DWORD> functions(directory->NumberOfFunctions);
                    std::vector<DWORD> names(directory->NumberOfNames);
                    std::vector<WORD> ordinals(directory->NumberOfNames);
                    const bool functionsRead = !functions.empty() && process.ReadExact(module.Base + directory->AddressOfFunctions, functions.data(), functions.size() * 4);
                    const bool namesRead = !names.empty() && process.ReadExact(module.Base + directory->AddressOfNames, names.data(), names.size() * 4) &&
                                           process.ReadExact(module.Base + directory->AddressOfNameOrdinals, ordinals.data(), ordinals.size() * 2);
                    if (functionsRead && namesRead)
                    {
                        for (std::size_t i = 0; i < names.size(); ++i)
                        {
                            if (ordinals[i] >= functions.size() || !inBlock(names[i], 1))
                                continue;
                            const DWORD function = functions[ordinals[i]];
                            if (function == 0 || inBlock(function, 1))
                                continue;
                            const char* text = reinterpret_cast<const char*>(block.data() + (names[i] - exportRva));
                            const std::size_t limit = block.size() - (names[i] - exportRva);
                            const std::string name(text, strnlen(text, limit));
                            const std::uint64_t address = module.Base + function;
                            table->ByAddress.push_back({ address, name });
                            table->ByName.emplace(Lower(name), address);
                        }
                        std::sort(table->ByAddress.begin(), table->ByAddress.end(), [](const ExportSymbol& a, const ExportSymbol& b) { return a.Address < b.Address; });
                    }
                }
            }
        }
        std::lock_guard lock(m_exportLock);
        m_exports[module.Base] = table;
        return table;
    }

    std::vector<ExportSymbol> Symbols::ExportsOf(const ModuleInfo& module) const
    {
        return Exports(module)->ByAddress;
    }

    std::uint64_t Symbols::ResolveExport(std::string_view moduleName, std::string_view name) const
    {
        if (!m_process)
            return 0;
        const auto module = m_process->FindModule(moduleName);
        if (!module)
            return 0;
        const auto table = Exports(*module);
        const auto found = table->ByName.find(Lower(name));
        return found == table->ByName.end() ? 0 : found->second;
    }

    std::optional<std::uint64_t> Symbols::Resolve(std::string_view name) const
    {
        return ResolveName(name, {});
    }

    void Symbols::SetSymbolProvider(SymbolForward forward, SymbolReverse reverse)
    {
        std::lock_guard lock(m_providerLock);
        m_forward = std::move(forward);
        m_reverse = std::move(reverse);
        m_hasProvider = static_cast<bool>(m_forward) || static_cast<bool>(m_reverse);
    }

    std::optional<std::uint64_t> Symbols::ResolveName(std::string_view name, const ExtraResolver& extra) const
    {
        if (name.empty())
            return std::nullopt;
        if (m_lookup)
        {
            if (const auto value = m_lookup(name, true))
                return value;
        }
        if (const auto value = ResolveBuiltin(name, extra))
            return value;
        if (m_hasProvider.load())
        {
            SymbolForward forward;
            {
                std::lock_guard lock(m_providerLock);
                forward = m_forward;
            }
            if (forward)
            {
                if (const auto value = forward(name))
                    return value;
            }
        }
        if (m_lookup)
            return m_lookup(name, false);
        return std::nullopt;
    }

    std::optional<std::uint64_t> Symbols::ResolveBuiltin(std::string_view name, const ExtraResolver& extra) const
    {
        if (const auto registered = FindRegistered(name))
            return registered;
        if (extra)
        {
            if (const auto value = extra(name))
                return value;
        }
        if (!m_process)
            return std::nullopt;
        if (const auto module = m_process->FindModule(name))
            return module->Base;

        for (const char separator : { '!', '.' })
        {
            const std::size_t at = name.rfind(separator);
            if (at == std::string_view::npos || at == 0 || at + 1 >= name.size())
                continue;
            if (const std::uint64_t address = ResolveExport(name.substr(0, at), name.substr(at + 1)))
                return address;
        }

        if (name.find('.') == std::string_view::npos && name.find('!') == std::string_view::npos)
        {
            const std::string key = Lower(name);
            const ModuleList modules = m_process->Modules();
            for (const ModuleInfo& module : *modules)
            {
                const auto table = Exports(module);
                if (const auto found = table->ByName.find(key); found != table->ByName.end())
                    return found->second;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> Symbols::ExactName(std::uint64_t address) const
    {
        {
            std::lock_guard lock(m_registeredLock);
            if (const UserSymbol* symbol = RegisteredAt(address))
                return symbol->Name;
        }
        if (!m_process)
            return std::nullopt;
        const auto module = m_process->ModuleAt(address);
        if (!module)
            return std::nullopt;
        if (address == module->Base)
            return module->Name;
        const auto table = Exports(*module);
        const auto found = std::lower_bound(table->ByAddress.begin(), table->ByAddress.end(), address,
            [](const ExportSymbol& symbol, std::uint64_t value) { return symbol.Address < value; });
        if (found != table->ByAddress.end() && found->Address == address)
        {
            std::string moduleName = module->Name;
            if (const std::size_t dot = moduleName.rfind('.'); dot != std::string::npos)
                moduleName.resize(dot);
            return moduleName + "." + found->Name;
        }
        return std::nullopt;
    }

    std::optional<std::pair<std::uint64_t, std::string>> Symbols::Containing(std::uint64_t address, std::uint64_t reach) const
    {
        std::optional<std::pair<std::uint64_t, std::string>> best;
        const auto consider = [&](std::uint64_t start, const std::string& name)
            {
                // A registered symbol is considered first, so it keeps the name
                // when an export sits at the same address (as in ExactName).
                if (start <= address && address - start < reach && (!best || start > best->first))
                    best = std::make_pair(start, name);
            };
        std::optional<std::pair<std::uint64_t, std::string>> registered;
        {
            std::lock_guard lock(m_registeredLock);
            if (auto below = m_registeredByAddress.upper_bound(address); below != m_registeredByAddress.begin())
            {
                --below;
                if (const UserSymbol* symbol = RegisteredAt(below->first))
                    registered = std::make_pair(symbol->Address, symbol->Name);
            }
        }
        if (registered && !(m_process && m_process->ModuleAt(registered->first)))
            consider(registered->first, registered->second);
        if (!m_process)
            return best;
        const auto module = m_process->ModuleAt(address);
        if (!module)
            return best;
        consider(module->Base, module->Name);
        const auto table = Exports(*module);
        auto found = std::upper_bound(table->ByAddress.begin(), table->ByAddress.end(), address,
            [](std::uint64_t value, const ExportSymbol& symbol) { return value < symbol.Address; });
        if (found != table->ByAddress.begin())
        {
            --found;
            std::string moduleName = module->Name;
            if (const std::size_t dot = moduleName.rfind('.'); dot != std::string::npos)
                moduleName.resize(dot);
            consider(found->Address, moduleName + "." + found->Name);
        }
        return best;
    }

    std::optional<std::uint64_t> Symbols::RegisteredIn(std::uint64_t from, std::uint64_t to) const
    {
        std::lock_guard lock(m_registeredLock);
        if (const auto above = m_registeredByAddress.upper_bound(from); above != m_registeredByAddress.end() && above->first < to)
            return above->first;
        return std::nullopt;
    }

    std::optional<std::uint64_t> Symbols::RegisteredBelow(std::uint64_t address, std::uint64_t floor) const
    {
        std::lock_guard lock(m_registeredLock);
        if (auto below = m_registeredByAddress.lower_bound(address); below != m_registeredByAddress.begin())
        {
            --below;
            if (below->first >= floor)
                return below->first;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> Symbols::ExportIn(std::uint64_t from, std::uint64_t to) const
    {
        if (!m_process)
            return std::nullopt;
        const auto module = m_process->ModuleAt(from);
        if (!module)
            return std::nullopt;
        const auto table = Exports(*module);
        const auto found = std::upper_bound(table->ByAddress.begin(), table->ByAddress.end(), from,
            [](std::uint64_t value, const ExportSymbol& symbol)
            {
                return value < symbol.Address;
            });
        if (found != table->ByAddress.end() && found->Address < to)
            return found->Address;
        return std::nullopt;
    }

    std::optional<std::uint64_t> Symbols::ExportBelow(std::uint64_t address, std::uint64_t floor) const
    {
        if (!m_process || address == 0)
            return std::nullopt;
        const auto module = m_process->ModuleAt(address - 1);
        if (!module)
            return std::nullopt;
        const auto table = Exports(*module);
        auto found = std::lower_bound(table->ByAddress.begin(), table->ByAddress.end(), address,
            [](const ExportSymbol& symbol, std::uint64_t value)
            {
                return symbol.Address < value;
            });
        if (found == table->ByAddress.begin())
            return std::nullopt;
        --found;
        if (found->Address < floor)
            return std::nullopt;
        return found->Address;
    }

    bool Symbols::LabelOnly(std::uint64_t address) const
    {
        {
            std::lock_guard lock(m_registeredLock);
            if (!RegisteredAt(address))
                return false;
        }
        if (!m_process)
            return false;
        const auto module = m_process->ModuleAt(address);
        if (!module)
            return false;
        const auto table = Exports(*module);
        const auto found = std::lower_bound(table->ByAddress.begin(), table->ByAddress.end(), address,
            [](const ExportSymbol& symbol, std::uint64_t value)
            {
                return symbol.Address < value;
            });
        return found == table->ByAddress.end() || found->Address != address;
    }

    std::string Symbols::ModuleOffset(std::uint64_t address) const
    {
        if (m_process)
        {
            if (const auto module = m_process->ModuleAt(address))
                return "\"" + module->Name + "\"+" + Hex(address - module->Base);
        }
        return Hex(address);
    }

    std::string Symbols::AddressText(std::uint64_t address, AddressStyle style) const
    {
        switch (style)
        {
        case AddressStyle::Name: return Describe(address);
        case AddressStyle::ModuleOffset: return ModuleOffset(address);
        default: return Hex(address);
        }
    }

    std::string Symbols::Describe(std::uint64_t address) const
    {
        {
            std::lock_guard lock(m_registeredLock);
            if (const UserSymbol* symbol = RegisteredAt(address))
                return symbol->Name;
        }
        if (!m_process)
            return Hex(address);
        const auto module = m_process->ModuleAt(address);
        std::string nearExport;
        if (module)
        {
            if (address == module->Base)
                return module->Name;
            const auto table = Exports(*module);
            auto found = std::upper_bound(table->ByAddress.begin(), table->ByAddress.end(), address,
                [](std::uint64_t value, const ExportSymbol& symbol) { return value < symbol.Address; });
            if (found != table->ByAddress.begin())
            {
                --found;
                if (address - found->Address < kExportReach)
                {
                    std::string moduleName = module->Name;
                    if (const std::size_t dot = moduleName.rfind('.'); dot != std::string::npos)
                        moduleName.resize(dot);
                    std::string text = moduleName + "." + found->Name;
                    if (address == found->Address)
                        return text;
                    // Only the nearest export below: the provider may know the
                    // function itself (a PDB name, or an IL2CPP method in
                    // GameAssembly.dll next to the il2cpp_* exports).
                    nearExport = text + "+" + Hex(address - found->Address);
                }
            }
        }
        // No export at the address. Ask the symbol provider: for an in-module
        // address that is the PDB describe or an IL2CPP method; for an address
        // in no module at all it is where JIT-compiled Mono methods get their
        // Namespace.Class:Method name (such code lives outside every module).
        if (m_hasProvider.load())
        {
            SymbolReverse reverse;
            {
                std::lock_guard lock(m_providerLock);
                reverse = m_reverse;
            }
            if (reverse)
            {
                if (const auto name = reverse(address))
                    return *name;
            }
        }
        if (!nearExport.empty())
            return nearExport;
        return module ? module->Name + "+" + Hex(address - module->Base) : Hex(address);
    }

    Evaluation Symbols::Evaluate(std::string_view expression, const ExtraResolver& extra) const
    {
        Parser parser(TrimView(expression), *this, extra, m_process);
        return parser.Run();
    }
}
