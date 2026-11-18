#include "Target/PdbSymbols.h"

#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <algorithm>
#include <chrono>

#include <windows.h>
#include <dbghelp.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kReach = 0x8000;

        std::string LowerAscii(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u >= 'A' && u <= 'Z')
                    c = static_cast<char>(u - 'A' + 'a');
            }
            return out;
        }

        std::wstring SymbolCacheDir()
        {
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            std::wstring dir = length ? std::wstring(temp, length) : std::wstring(L".\\");
            if (!dir.empty() && dir.back() != L'\\')
                dir.push_back(L'\\');
            dir += L"ManifoldSymbols";
            ::CreateDirectoryW(dir.c_str(), nullptr);
            return dir;
        }

        struct EnumContext
        {
            std::vector<std::pair<std::uint64_t, std::string>>* Out;
            const std::atomic<bool>* Stop;
        };

        BOOL CALLBACK EnumSymbol(PSYMBOL_INFO info, ULONG /*size*/, PVOID context)
        {
            auto* ctx = static_cast<EnumContext*>(context);
            if (ctx->Stop->load())
                return FALSE;
            if (info && info->Address && info->NameLen)
                ctx->Out->emplace_back(info->Address, std::string(info->Name, info->NameLen));
            return TRUE;
        }
    }

    PdbSymbols::~PdbSymbols()
    {
        End();
    }

    bool PdbSymbols::Begin(void* processHandle, std::vector<ModuleInfo> modules, bool windowsSymbols)
    {
        End();
        if (!processHandle)
            return false;
        m_handle = processHandle;
        m_stop = false;
        m_loaded = false;
        m_done = 0;
        m_total = 0;
        m_active = true;
        m_thread = std::thread(&PdbSymbols::Worker, this, std::move(modules), windowsSymbols);
        return true;
    }

    void PdbSymbols::End()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        {
            std::lock_guard lock(m_lock);
            m_byName.clear();
            m_byAddress.clear();
        }
        m_handle = nullptr;
        m_active = false;
        m_loaded = false;
        m_any = false;
        m_done = 0;
        m_total = 0;
    }

    void PdbSymbols::Worker(std::vector<ModuleInfo> modules, bool windowsSymbols)
    {
        SetThreadName(L"PDB symbols");
        const HANDLE process = static_cast<HANDLE>(m_handle);

        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
            SYMOPT_NO_PROMPTS | SYMOPT_CASE_INSENSITIVE | SYMOPT_AUTO_PUBLICS);

        std::wstring searchPath;
        if (windowsSymbols)
            searchPath = L"srv*" + SymbolCacheDir() + L"*https://msdl.microsoft.com/download/symbols";

        if (!::SymInitializeW(process, searchPath.empty() ? nullptr : searchPath.c_str(), FALSE))
        {
            LOG_WARN("Symbols", "PDB symbol handler could not start (SymInitialize failed)");
            m_loaded = true;
            m_active = false;
            return;
        }

        std::vector<ModuleInfo> wanted;
        wanted.reserve(modules.size());
        for (const ModuleInfo& module : modules)
            if (module.Base && module.Size && (windowsSymbols || !module.System))
                wanted.push_back(module);
        m_total = static_cast<int>(wanted.size());

        std::vector<std::pair<std::uint64_t, std::string>> collected;
        for (const ModuleInfo& module : wanted)
        {
            if (m_stop)
                break;
            const DWORD64 loaded = ::SymLoadModuleExW(process, nullptr, module.Path.c_str(), nullptr, module.Base,
                static_cast<DWORD>(module.Size), nullptr, 0);
            if (loaded || ::GetLastError() == ERROR_SUCCESS)
            {
                EnumContext ctx{ &collected, &m_stop };
                ::SymEnumSymbols(process, module.Base, "*", &EnumSymbol, &ctx);
            }
            m_done = m_done.load() + 1;
        }

        if (!m_stop)
        {
            std::unordered_map<std::string, std::uint64_t> byName;
            byName.reserve(collected.size());
            std::vector<Named> byAddress;
            byAddress.reserve(collected.size());
            for (auto& [address, name] : collected)
            {
                byName.emplace(LowerAscii(name), address);
                byAddress.push_back({ address, std::move(name) });
            }
            std::sort(byAddress.begin(), byAddress.end(), [](const Named& a, const Named& b) { return a.Address < b.Address; });
            {
                std::lock_guard lock(m_lock);
                m_byName = std::move(byName);
                m_byAddress = std::move(byAddress);
                m_any = !m_byAddress.empty();
            }
            LOG_INFO("Symbols", "PDB symbols loaded (" + std::to_string(m_byAddress.size()) + " from " +
                std::to_string(m_done.load()) + " modules)");
        }

        ::SymCleanup(process);
        m_loaded = true;
    }

    std::optional<std::uint64_t> PdbSymbols::Resolve(std::string_view name) const
    {
        if (name.empty() || !m_any.load())
            return std::nullopt;
        std::lock_guard lock(m_lock);
        const auto found = m_byName.find(LowerAscii(name));
        if (found == m_byName.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<std::string> PdbSymbols::Describe(std::uint64_t address) const
    {
        if (!m_any.load())
            return std::nullopt;
        std::lock_guard lock(m_lock);
        if (m_byAddress.empty())
            return std::nullopt;
        auto found = std::upper_bound(m_byAddress.begin(), m_byAddress.end(), address,
            [](std::uint64_t value, const Named& symbol) { return value < symbol.Address; });
        if (found == m_byAddress.begin())
            return std::nullopt;
        --found;
        const std::uint64_t delta = address - found->Address;
        if (delta >= kReach)
            return std::nullopt;
        if (delta == 0)
            return found->Name;
        return found->Name + "+" + Hex(delta);
    }

    bool PdbSymbols::WaitUntilLoaded(unsigned timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!m_loaded.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }
}
