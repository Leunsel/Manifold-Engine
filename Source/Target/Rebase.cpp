#include "Target/Rebase.h"

#include "Core/Text.h"

#include <windows.h>

namespace Manifold::Target
{
    void Rebases::Set(std::string_view module, std::optional<RebaseRule> rule)
    {
        const std::string key = Lower(Trim(module));
        if (key.empty())
            return;
        std::lock_guard lock(m_lock);
        if (rule)
            m_rules[key] = { Trim(module), *rule };
        else
            m_rules.erase(key);
        m_count = m_rules.size();
        ++m_generation;
    }

    std::optional<RebaseRule> Rebases::Rule(std::string_view module) const
    {
        std::lock_guard lock(m_lock);
        const auto found = m_rules.find(Lower(Trim(module)));
        if (found == m_rules.end())
            return std::nullopt;
        return found->second.Rule;
    }

    std::vector<RebaseEntry> Rebases::Entries() const
    {
        std::lock_guard lock(m_lock);
        std::vector<RebaseEntry> out;
        out.reserve(m_rules.size());
        for (const auto& [key, stored] : m_rules)
            out.push_back({ stored.Module, stored.Rule });
        return out;
    }

    void Rebases::Forget()
    {
        std::lock_guard lock(m_lock);
        m_imageBases.clear();
        ++m_generation;
    }

    void Rebases::Load(const std::map<std::string, std::string>& saved)
    {
        std::lock_guard lock(m_lock);
        m_rules.clear();
        for (const auto& [module, text] : saved)
        {
            const std::string name = Trim(module);
            if (const auto rule = ParseRule(text); rule && !name.empty())
                m_rules[Lower(name)] = { name, *rule };
        }
        m_count = m_rules.size();
        ++m_generation;
    }

    std::map<std::string, std::string> Rebases::Saved() const
    {
        std::lock_guard lock(m_lock);
        std::map<std::string, std::string> out;
        for (const auto& [key, stored] : m_rules)
            out[stored.Module] = RuleText(stored.Rule);
        return out;
    }

    std::string Rebases::RuleText(const RebaseRule& rule)
    {
        return rule.ImageBase ? "image" : "0x" + Hex(rule.Base);
    }

    std::optional<RebaseRule> Rebases::ParseRule(std::string_view text)
    {
        const std::string_view trimmed = TrimView(text);
        if (EqualsNoCase(trimmed, "image"))
            return RebaseRule{ true, 0 };
        if (trimmed.empty() || trimmed.front() == '-' || trimmed.front() == '+')
            return std::nullopt;
        if (const auto value = ParseHex(trimmed))
            return RebaseRule{ false, *value };
        return std::nullopt;
    }

    namespace
    {
        template <class Read>
        std::optional<std::uint64_t> ParseImageBase(Read&& read)
        {
            IMAGE_DOS_HEADER dos{};
            if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x10000)
                return std::nullopt;
            const std::uint64_t nt = static_cast<std::uint64_t>(dos.e_lfanew);
            DWORD signature = 0;
            WORD magic = 0;
            if (!read(nt, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE)
                return std::nullopt;
            const std::uint64_t optional = nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
            if (!read(optional, &magic, sizeof(magic)))
                return std::nullopt;
            if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                std::uint64_t base = 0;
                if (read(optional + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase), &base, sizeof(base)))
                    return base;
            }
            else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                std::uint32_t base = 0;
                if (read(optional + offsetof(IMAGE_OPTIONAL_HEADER32, ImageBase), &base, sizeof(base)))
                    return base;
            }
            return std::nullopt;
        }
    }

    std::optional<std::uint64_t> Rebases::ReadImageBase(const Process& process, const ModuleInfo& module)
    {
        // ∑ The file first: when the loader moves an image it writes the new base into the header in memory, so
        //   memory shows where the module is, not where it asks to be. Memory only when the file cannot be read.
        if (!module.Path.empty())
        {
            const HANDLE file = ::CreateFileW(module.Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                const auto base = ParseImageBase([&](std::uint64_t offset, void* buffer, std::size_t size)
                    {
                        OVERLAPPED at{};
                        at.Offset = static_cast<DWORD>(offset);
                        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
                        DWORD read = 0;
                        return ::ReadFile(file, buffer, static_cast<DWORD>(size), &read, &at) && read == size;
                    });
                ::CloseHandle(file);
                if (base)
                    return base;
            }
        }
        return ParseImageBase([&](std::uint64_t offset, void* buffer, std::size_t size) { return process.ReadExact(module.Base + offset, buffer, size); });
    }

    std::optional<std::uint64_t> Rebases::ImageBase(const Process& process, const ModuleInfo& module) const
    {
        {
            std::lock_guard lock(m_lock);
            if (const auto found = m_imageBases.find(module.Base); found != m_imageBases.end())
                return found->second;
        }
        const auto base = ReadImageBase(process, module);
        std::lock_guard lock(m_lock);
        m_imageBases[module.Base] = base;
        return base;
    }

    std::optional<std::uint64_t> Rebases::ShownBase(const Process& process, const ModuleInfo& module) const
    {
        if (!Any())
            return std::nullopt;
        const auto rule = Rule(module.Name);
        if (!rule)
            return std::nullopt;
        if (!rule->ImageBase)
            return rule->Base;
        return ImageBase(process, module);
    }

    std::optional<std::uint64_t> Rebases::Shown(const Process& process, std::uint64_t address) const
    {
        if (!Any())
            return std::nullopt;
        const auto module = process.ModuleAt(address);
        if (!module)
            return std::nullopt;
        const auto base = ShownBase(process, *module);
        if (!base)
            return std::nullopt;
        return *base + (address - module->Base);
    }

    std::optional<RebaseHit> Rebases::Runtime(const Process& process, std::uint64_t shown) const
    {
        if (!Any())
            return std::nullopt;
        const ModuleList modules = process.Modules();
        if (!modules)
            return std::nullopt;
        for (const ModuleInfo& module : *modules)
        {
            const auto base = ShownBase(process, module);
            if (!base || shown < *base || shown - *base >= module.Size)
                continue;
            return RebaseHit{ module.Base + (shown - *base), module, *base };
        }
        return std::nullopt;
    }
}
