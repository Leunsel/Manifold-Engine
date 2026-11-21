#include "ClrHelper/DacSession.h"

#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace Manifold::ClrHelper
{
    namespace P = ClrProtocol;

    namespace
    {
        constexpr std::uint32_t kPointer = sizeof(void*);
        constexpr std::uint32_t kMaxFields = 8192;
        constexpr std::uint32_t kMaxMethods = 16384;
        constexpr DWORD kInterfaceAttribute = 0x20;  // tdInterface
        constexpr std::uint16_t kMethodStatic = 0x10, kMethodVirtual = 0x40, kMethodAbstract = 0x400;

        std::string Utf8(const wchar_t* text, std::size_t length)
        {
            if (!length)
                return {};
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
            if (size > 0)
                ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
            return out;
        }

        std::string Utf8(const std::wstring& text) { return Utf8(text.data(), text.size()); }

        std::wstring Lower(std::wstring text)
        {
            for (wchar_t& c : text)
                c = static_cast<wchar_t>(std::towlower(c));
            return text;
        }

        std::wstring FileName(const std::wstring& path)
        {
            const std::size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

        std::string Hex(std::uint64_t value)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
            return text;
        }

        // A name the DAC fills into a caller's buffer: asked once with room for
        // most names, again with the size it reports when that was too small.
        template <typename Call>
        std::string AskName(Call call)
        {
            std::vector<wchar_t> buffer(512);
            unsigned needed = 0;
            if (FAILED(call(static_cast<unsigned>(buffer.size()), buffer.data(), &needed)))
                return {};
            if (needed > buffer.size() && needed < 65536)
            {
                buffer.assign(needed, L'\0');
                if (FAILED(call(static_cast<unsigned>(buffer.size()), buffer.data(), &needed)))
                    return {};
            }
            std::size_t length = 0;
            while (length < buffer.size() && buffer[length])
                ++length;
            return Utf8(buffer.data(), length);
        }

        // "Ns.Outer+Inner[[...]]" -> ("Ns", "Outer+Inner[[...]]"): the
        // namespace ends at the last dot before any nesting or generic part.
        std::pair<std::string, std::string> SplitName(const std::string& full)
        {
            const std::size_t stop = std::min(full.find('+'), full.find('['));
            const std::size_t dot = full.rfind('.', stop == std::string::npos ? std::string::npos : stop);
            if (dot == std::string::npos || dot == 0)
                return { {}, full };
            return { full.substr(0, dot), full.substr(dot + 1) };
        }

        bool Primitive(DWORD element)
        {
            return (element >= kElementBoolean && element <= kElementR8) || element == kElementI || element == kElementU || element == kElementPtr
                || element == kElementFnPtr;
        }

        void __stdcall CollectType(UINT index, ClrAddress mt, LPVOID token)
        {
            static_cast<std::vector<std::pair<UINT, ClrAddress>>*>(token)->emplace_back(index, mt);
        }

        // Progress lines inside long requests, with MANIFOLD_CLR_TRACE set.
        void Trace(const char* format, ...)
        {
            static std::FILE* file = []() -> std::FILE*
                {
                    wchar_t path[MAX_PATH] = L"";
                    std::FILE* opened = nullptr;
                    if (::GetEnvironmentVariableW(L"MANIFOLD_CLR_TRACE", path, MAX_PATH))
                        _wfopen_s(&opened, (std::wstring(path) + L".steps").c_str(), L"a");
                    return opened;
                }();
            if (!file)
                return;
            va_list arguments;
            va_start(arguments, format);
            std::vfprintf(file, format, arguments);
            va_end(arguments);
            std::fputc(0x0A, file);
            std::fflush(file);
        }

        std::vector<std::uint8_t> Failure(const std::string& text)
        {
            P::Writer out;
            out.U8(1);
            out.Text(text);
            return out.Done();
        }
    }

    void DataTarget::Attach(HANDLE process)
    {
        m_process = process;
        m_pages.clear();
        RefreshModules();
    }

    void DataTarget::Forget()
    {
        m_pages.clear();
    }

    void DataTarget::RefreshModules()
    {
        m_modules.clear();
        if (!m_process)
            return;
        std::vector<HMODULE> handles(1024);
        DWORD needed = 0;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (!::K32EnumProcessModulesEx(m_process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed, LIST_MODULES_DEFAULT))
                return;
            if (needed <= handles.size() * sizeof(HMODULE))
                break;
            handles.resize(needed / sizeof(HMODULE) + 64);
        }
        handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));
        for (HMODULE handle : handles)
        {
            MODULEINFO info{};
            wchar_t path[MAX_PATH * 2] = L"";
            if (!::K32GetModuleInformation(m_process, handle, &info, sizeof(info)))
                continue;
            const DWORD length = ::K32GetModuleFileNameExW(m_process, handle, path, static_cast<DWORD>(std::size(path)));
            Module module;
            module.Path.assign(path, length);
            module.Name = FileName(module.Path);
            module.Base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
            module.Size = info.SizeOfImage;
            m_modules.push_back(std::move(module));
        }
    }

    const std::uint8_t* DataTarget::Page(ClrAddress page)
    {
        if (const auto found = m_pages.find(page); found != m_pages.end())
            return found->second ? found->second->data() : nullptr;
        if (m_pages.size() >= kMaxPages)
            m_pages.clear();
        auto bytes = std::make_unique<std::array<std::uint8_t, kPage>>();
        SIZE_T got = 0;
        const bool ok = page <= static_cast<ClrAddress>(~static_cast<std::uintptr_t>(0))
            && ::ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(page)), bytes->data(), kPage, &got) && got == kPage;
        auto& slot = m_pages[page];
        if (ok)
            slot = std::move(bytes);
        return slot ? slot->data() : nullptr;
    }

    bool DataTarget::Read(ClrAddress address, void* buffer, std::size_t size)
    {
        ULONG32 done = 0;
        return SUCCEEDED(ReadVirtual(address, static_cast<BYTE*>(buffer), static_cast<ULONG32>(size), &done)) && done == size;
    }

    HRESULT DataTarget::QueryInterface(REFIID iid, void** out)
    {
        if (!out)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == kIidClrDataTarget)
            *out = static_cast<ClrDataTarget*>(this);
        else if (iid == kIidClrMetadataLocator)
            *out = static_cast<ClrMetadataLocator*>(this);
        else if (iid == kIidClrRuntimeLocator && m_runtimeBase)
            *out = static_cast<ClrRuntimeLocator*>(this);
        else
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        return S_OK;
    }

    HRESULT DataTarget::GetMachineType(ULONG32* machine)
    {
        *machine = kPointer == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
        return S_OK;
    }

    HRESULT DataTarget::GetPointerSize(ULONG32* size)
    {
        *size = kPointer;
        return S_OK;
    }

    HRESULT DataTarget::GetImageBase(LPCWSTR imagePath, ClrAddress* base)
    {
        if (!imagePath || !base)
            return E_INVALIDARG;
        const std::wstring wanted = Lower(FileName(imagePath));
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const Module& module : m_modules)
            {
                if (Lower(module.Name) == wanted)
                {
                    *base = module.Base;
                    return S_OK;
                }
            }
            RefreshModules();
        }
        return E_FAIL;
    }

    HRESULT DataTarget::ReadVirtual(ClrAddress address, BYTE* buffer, ULONG32 size, ULONG32* done)
    {
        if (done)
            *done = 0;
        if (!buffer)
            return E_INVALIDARG;
        // A large read (metadata, a whole table) goes straight through; the
        // cache is for the DAC's many small ones.
        if (size >= 64 * 1024 && address <= static_cast<ClrAddress>(~static_cast<std::uintptr_t>(0)))
        {
            SIZE_T got = 0;
            if (::ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), buffer, size, &got) && got == size)
            {
                if (done)
                    *done = size;
                return S_OK;
            }
        }
        ULONG32 copied = 0;
        while (copied < size)
        {
            const ClrAddress at = address + copied;
            const ClrAddress page = at & ~static_cast<ClrAddress>(kPage - 1);
            const std::uint8_t* bytes = Page(page);
            if (!bytes)
                break;
            const std::size_t offset = static_cast<std::size_t>(at - page);
            const ULONG32 chunk = static_cast<ULONG32>(std::min<std::size_t>(kPage - offset, size - copied));
            std::memcpy(buffer + copied, bytes + offset, chunk);
            copied += chunk;
        }
        if (done)
            *done = copied;
        return copied ? S_OK : HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
    }

    std::size_t DataTarget::ReadDirect(ClrAddress address, std::uint8_t* buffer, std::size_t size)
    {
        if (!size || address > static_cast<ClrAddress>(~static_cast<std::uintptr_t>(0)))
            return 0;
        SIZE_T got = 0;
        if (::ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), buffer, size, &got) && got == size)
            return size;
        // Part of the range is not readable: the readable pages from the start.
        std::size_t done = 0;
        while (done < size)
        {
            const ClrAddress at = address + done;
            const std::size_t chunk = std::min<std::size_t>(size - done, kPage - static_cast<std::size_t>(at & (kPage - 1)));
            got = 0;
            if (!::ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), buffer + done, chunk, &got) || got != chunk)
                break;
            done += chunk;
        }
        return done;
    }

    HRESULT DataTarget::GetMetadata(LPCWSTR imagePath, ULONG32, ULONG32, GUID*, ULONG32, ULONG32, ULONG32 bufferSize, BYTE* buffer, ULONG32* dataSize)
    {
        if (!imagePath)
            return E_INVALIDARG;
        std::filesystem::path path(imagePath);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            const std::wstring wanted = Lower(FileName(imagePath));
            path.clear();
            for (const Module& module : m_modules)
            {
                if (Lower(module.Name) == wanted)
                {
                    path = module.Path;
                    break;
                }
            }
            if (path.empty())
                return E_FAIL;
        }
        // ∑ The DAC asks again after every flush (an NGEN'd mscorlib has no
        // metadata in memory, and naming a type can ask for it); the file
        // does not change, so it is read once.
        const std::wstring key = Lower(path.wstring());
        auto cached = m_metadataFiles.find(key);
        if (cached == m_metadataFiles.end())
        {
            std::ifstream file(path, std::ios::binary);
            std::optional<std::vector<std::uint8_t>> block;
            if (file)
            {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                block = Target::ClrMetadata::FromPeFile(bytes);
            }
            cached = m_metadataFiles.emplace(key, std::move(block)).first;
        }
        const std::optional<std::vector<std::uint8_t>>& metadata = cached->second;
        if (!metadata)
            return E_FAIL;
        if (dataSize)
            *dataSize = static_cast<ULONG32>(metadata->size());
        if (!buffer || bufferSize < metadata->size())
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::memcpy(buffer, metadata->data(), metadata->size());
        return S_OK;
    }

    HRESULT DataTarget::GetRuntimeBase(ClrAddress* base)
    {
        if (!base || !m_runtimeBase)
            return E_FAIL;
        *base = m_runtimeBase;
        return S_OK;
    }

    DacSession::~DacSession()
    {
        if (m_sos14)
            m_sos14->Release();
        if (m_sos)
            m_sos->Release();
        if (m_data)
            m_data->Release();
        // The DAC stays loaded: unloading it while it may still hold threads
        // or callbacks buys nothing in a process that is about to end.
        if (m_process)
            ::CloseHandle(m_process);
    }

    std::vector<std::uint8_t> DacSession::Handle(std::span<const std::uint8_t> request)
    {
        P::Reader in(request);
        const auto op = static_cast<P::Op>(in.U8());
        P::Writer out;
        out.U8(0);
        if (op == P::Op::Open)
        {
            const std::uint32_t version = in.U32();
            const std::uint32_t pid = in.U32();
            const std::uint64_t handle = in.U64();
            if (!in.Ok())
                return Failure("the request was cut short");
            if (version != P::kVersion)
                return Failure("the helper speaks protocol " + std::to_string(P::kVersion) + ", the program " + std::to_string(version));
            const std::string error = Open(pid, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)), out);
            return error.empty() ? out.Done() : Failure(error);
        }
        if (!m_sos)
            return Failure("no runtime is open");
        // The game keeps running between questions; what was cached may be
        // stale after a pause. Questions that come back to back (reading the
        // class list) share the cache.
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastRequest > std::chrono::milliseconds(250))
            Flush();
        m_lastRequest = now;
        const std::uint64_t argument = in.U64();
        switch (op)
        {
            case P::Op::Modules: Modules(out); break;
            case P::Op::Types: Types(argument, out); break;
            case P::Op::Members: Members(argument, out); break;
            case P::Op::DescribeType: DescribeType(argument, out); break;
            case P::Op::MethodAt: MethodAt(argument, out); break;
            case P::Op::MethodCode: MethodCode(argument, out); break;
            case P::Op::Flush: Flush(); m_modulesLoaded = false; break;
            case P::Op::HeapStats:
                if (std::string error = HeapStats(out); !error.empty())
                    return Failure(error);
                break;
            case P::Op::Instances:
            {
                const std::uint32_t limit = in.U32();
                if (std::string error = Instances(argument, in.Ok() ? std::min<std::uint32_t>(limit, 1000000) : 1000, out); !error.empty())
                    return Failure(error);
                break;
            }
            case P::Op::ObjectInfo: ObjectInfo(argument, out); break;
            default: return Failure("unknown request " + std::to_string(static_cast<int>(op)));
        }
        return out.Done();
    }

    std::string DacSession::Open(std::uint32_t pid, HANDLE inherited, P::Writer& out)
    {
        if (m_sos)
            return "a runtime is already open";
        m_process = inherited;
        if (!m_process || ::GetProcessId(m_process) != pid)
        {
            m_process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!m_process)
                return "could not open process " + std::to_string(pid) + " (error " + std::to_string(::GetLastError()) + ")";
        }
        m_target.Attach(m_process);
        if (m_target.Modules().empty())
            return "could not list the game's modules (error " + std::to_string(::GetLastError()) + ")";

        // The runtime module names the DAC that matches it, next to it.
        struct Known
        {
            const wchar_t* Runtime;
            const wchar_t* Dac;
            const char* Flavour;
        };
        static const Known kKnown[] = {
            { L"coreclr.dll", L"mscordaccore.dll", ".NET" },
            { L"clr.dll", L"mscordacwks.dll", ".NET Framework" },
            { L"mscorwks.dll", L"mscordacwks.dll", ".NET Framework" },
        };
        const DataTarget::Module* runtime = nullptr;
        const Known* kind = nullptr;
        for (const Known& known : kKnown)
        {
            for (const DataTarget::Module& module : m_target.Modules())
            {
                if (Lower(module.Name) == known.Runtime)
                {
                    runtime = &module;
                    kind = &known;
                    break;
                }
            }
            if (runtime)
                break;
        }
        if (!runtime)
            return "the game has no .NET runtime loaded (no coreclr.dll or clr.dll)";
        const std::filesystem::path dac = std::filesystem::path(runtime->Path).parent_path() / kind->Dac;
        std::error_code error;
        if (!std::filesystem::exists(dac, error))
            return "the runtime's data access library is missing: " + Utf8(dac.wstring());
        m_target.SetRuntimeBase(runtime->Base);
        m_dac = ::LoadLibraryExW(dac.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_dac)
            return "could not load " + Utf8(dac.wstring()) + " (error " + std::to_string(::GetLastError()) + ")";
        const auto create = reinterpret_cast<ClrDataCreateInstanceFn>(reinterpret_cast<void*>(::GetProcAddress(m_dac, "CLRDataCreateInstance")));
        if (!create)
            return Utf8(dac.wstring()) + " has no CLRDataCreateInstance";
        void* created = nullptr;
        HRESULT result = create(kIidXClrDataProcess, &m_target, &created);
        if (FAILED(result) || !created)
            return "the data access library did not accept the game (HRESULT 0x" + Hex(static_cast<std::uint32_t>(result)) + ")";
        m_data = static_cast<XClrDataProcess*>(created);
        result = m_data->QueryInterface(kIidSosDac, reinterpret_cast<void**>(&m_sos));
        if (FAILED(result) || !m_sos)
            return "the data access library has no SOS interface (HRESULT 0x" + Hex(static_cast<std::uint32_t>(result)) + ")";
        if (FAILED(m_data->QueryInterface(kIidSosDac14, reinterpret_cast<void**>(&m_sos14))))
            m_sos14 = nullptr;
        Padded<DacpUsefulGlobalsData> globals;
        if (SUCCEEDED(m_sos->GetUsefulGlobals(globals.Ptr())))
            m_globals = globals.Value;
        m_lastRequest = std::chrono::steady_clock::now();

        std::string version;
        DWORD ignored = 0;
        const DWORD infoSize = ::GetFileVersionInfoSizeW(runtime->Path.c_str(), &ignored);
        if (infoSize)
        {
            std::vector<std::uint8_t> info(infoSize);
            VS_FIXEDFILEINFO* fixed = nullptr;
            UINT fixedSize = 0;
            if (::GetFileVersionInfoW(runtime->Path.c_str(), 0, infoSize, info.data())
                && ::VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) && fixed)
            {
                version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "."
                    + std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
            }
        }
        out.Text(kind->Flavour);
        out.Text(version);
        out.Text(Utf8(runtime->Path));
        out.Text(Utf8(dac.wstring()));
        out.U32(kPointer);
        out.U8(m_sos14 ? 1 : 0);
        return {};
    }

    void DacSession::Flush()
    {
        if (m_data)
            m_data->Flush();
        m_target.Forget();
    }

    void DacSession::LoadModules()
    {
        if (m_modulesLoaded)
            return;
        m_modulesLoaded = true;
        m_target.RefreshModules();
        Padded<DacpAppDomainStoreData> store;
        if (FAILED(m_sos->GetAppDomainStoreData(store.Ptr())))
            return;
        std::vector<ClrAddress> domains;
        if (store->DomainCount > 0 && store->DomainCount < 4096)
        {
            std::vector<ClrAddress> list(static_cast<std::size_t>(store->DomainCount));
            unsigned needed = 0;
            if (SUCCEEDED(m_sos->GetAppDomainList(static_cast<unsigned>(list.size()), list.data(), &needed)))
            {
                list.resize(std::min<std::size_t>(list.size(), needed));
                domains = list;
            }
        }
        m_defaultDomain = domains.empty() ? 0 : domains.front();
        std::vector<ClrAddress> all = domains;
        for (ClrAddress domain : { store->SharedDomain, store->SystemDomain })
        {
            if (domain)
                all.push_back(domain);
        }

        std::set<ClrAddress> seen;
        for (const ModuleRecord& known : m_modules)
            seen.insert(known.Address);
        for (ClrAddress domain : all)
        {
            int needed = 0;
            if (FAILED(m_sos->GetAssemblyList(domain, 0, nullptr, &needed)) || needed <= 0 || needed > 65536)
                continue;
            std::vector<ClrAddress> assemblies(static_cast<std::size_t>(needed));
            if (FAILED(m_sos->GetAssemblyList(domain, needed, assemblies.data(), &needed)))
                continue;
            assemblies.resize(std::min<std::size_t>(assemblies.size(), static_cast<std::size_t>(std::max(needed, 0))));
            for (ClrAddress assembly : assemblies)
            {
                unsigned count = 0;
                if (FAILED(m_sos->GetAssemblyModuleList(assembly, 0, nullptr, &count)) || count == 0 || count > 4096)
                    continue;
                std::vector<ClrAddress> modules(count);
                if (FAILED(m_sos->GetAssemblyModuleList(assembly, count, modules.data(), &count)))
                    continue;
                modules.resize(std::min<std::size_t>(modules.size(), count));
                for (ClrAddress address : modules)
                {
                    if (!address || !seen.insert(address).second)
                        continue;
                    Padded<DacpModuleData> data;
                    if (FAILED(m_sos->GetModuleData(address, data.Ptr())))
                        continue;
                    ModuleRecord record;
                    record.Address = address;
                    record.Assembly = data->Assembly ? data->Assembly : assembly;
                    record.ModuleId = data->ModuleId;
                    record.Dynamic = data->IsReflection != 0;
                    std::string path;
                    if (data->PeFile)
                        path = AskName([&](unsigned n, WCHAR* text, unsigned* got) { return m_sos->GetPEFileName(data->PeFile, n, text, got); });
                    if (path.empty())
                        path = AskName([&](unsigned n, WCHAR* text, unsigned* got) { return m_sos->GetAssemblyName(record.Assembly, n, text, got); });
                    record.Path = path;
                    std::string name = path;
                    if (const std::size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
                        name = name.substr(slash + 1);
                    if (name.size() > 4)
                    {
                        std::string tail = name.substr(name.size() - 4);
                        std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                        if (tail == ".dll" || tail == ".exe")
                            name.resize(name.size() - 4);
                    }
                    record.Name = name.empty() ? "dynamic" : name;
                    ClrAddress base = 0;
                    if (data->PeFile && SUCCEEDED(m_sos->GetPEFileBase(data->PeFile, &base)) && base)
                        record.Base = base;
                    else
                        record.Base = data->IlBase;
                    if (record.Base)
                    {
                        std::uint32_t lfanew = 0;
                        std::uint32_t imageSize = 0;
                        if (m_target.Read(record.Base + 0x3C, &lfanew, sizeof(lfanew)) && lfanew < 0x1000)
                            m_target.Read(record.Base + lfanew + 24 + 56, &imageSize, sizeof(imageSize));
                        record.Size = imageSize;
                    }
                    m_modules.push_back(std::move(record));
                }
            }
        }
    }

    DacSession::ModuleRecord* DacSession::FindModule(ClrAddress module)
    {
        LoadModules();
        for (ModuleRecord& record : m_modules)
        {
            if (record.Address == module)
                return &record;
        }
        // A module loaded after the list was made.
        m_modulesLoaded = false;
        LoadModules();
        for (ModuleRecord& record : m_modules)
        {
            if (record.Address == module)
                return &record;
        }
        return nullptr;
    }

    const Target::ClrMetadata* DacSession::MetadataOf(ClrAddress module)
    {
        ModuleRecord* record = FindModule(module);
        if (!record)
            return nullptr;
        if (record->MetadataTried)
            return record->Metadata.get();
        record->MetadataTried = true;
        auto metadata = std::make_shared<Target::ClrMetadata>();
        Padded<DacpModuleData> data;
        if (SUCCEEDED(m_sos->GetModuleData(module, data.Ptr())) && data->MetadataStart && data->MetadataSize && data->MetadataSize < 512ull * 1024 * 1024)
        {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(data->MetadataSize));
            if (m_target.Read(data->MetadataStart, bytes.data(), bytes.size()) && metadata->Parse(bytes))
                record->Metadata = metadata;
        }
        // Not mapped in the game (an NGEN image of the .NET Framework): the
        // assembly's file has the same tables.
        if (!record->Metadata && !record->Dynamic && !record->Path.empty())
        {
            const int length = ::MultiByteToWideChar(CP_UTF8, 0, record->Path.data(), static_cast<int>(record->Path.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, record->Path.data(), static_cast<int>(record->Path.size()), wide.data(), length);
            std::ifstream file(std::filesystem::path(wide), std::ios::binary);
            if (file)
            {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (const auto block = Target::ClrMetadata::FromPeFile(bytes); block && metadata->Parse(*block))
                    record->Metadata = metadata;
            }
        }
        return record->Metadata.get();
    }

    const DacSession::TypeInfo& DacSession::Type(ClrAddress mt)
    {
        static const TypeInfo kInvalid;
        if (!mt)
            return kInvalid;
        if (const auto found = m_types.find(mt); found != m_types.end())
            return found->second;
        Padded<DacpMethodTableData> data;
        if (FAILED(m_sos->GetMethodTableData(mt, data.Ptr())))
            return kInvalid;
        TypeInfo info;
        info.Valid = true;
        info.Data = data.Value;
        return m_types.emplace(mt, std::move(info)).first->second;
    }

    const std::string& DacSession::NameOf(ClrAddress mt)
    {
        static const std::string kNone;
        const TypeInfo& known = Type(mt);
        if (!known.Valid)
            return kNone;
        TypeInfo& info = m_types[mt];
        if (!info.Named)
        {
            info.Named = true;
            info.Name = AskName([&](unsigned n, WCHAR* text, unsigned* got) { return m_sos->GetMethodTableName(mt, n, text, got); });
        }
        return info.Name;
    }

    std::uint32_t DacSession::TypeFlags(ClrAddress mt, bool withGeneric)
    {
        const TypeInfo& info = Type(mt);
        if (!info.Valid)
            return 0;
        std::uint32_t flags = 0;
        if (info.Data.AttrClass & kInterfaceAttribute)
            flags |= P::kTypeInterface;
        if (mt == m_globals.StringMethodTable)
            flags |= P::kTypeString;
        // Only arrays and strings have elements.
        if (info.Data.ComponentSize && mt != m_globals.StringMethodTable)
            flags |= P::kTypeArray;
        if (withGeneric)
        {
            const std::string& name = NameOf(mt);
            if (name.find('`') != std::string::npos && name.find('[') == std::string::npos)
                flags |= P::kTypeGeneric;
        }
        const ClrAddress parent = info.Data.ParentMethodTable;
        if (parent)
        {
            if (!m_valueTypeMt || !m_enumMt)
            {
                const std::string& parentName = NameOf(parent);
                if (parentName == "System.ValueType")
                    m_valueTypeMt = parent;
                else if (parentName == "System.Enum")
                    m_enumMt = parent;
            }
            if (parent == m_enumMt)
                flags |= P::kTypeEnum | P::kTypeValue;
            else if (parent == m_valueTypeMt && mt != m_enumMt)
                flags |= P::kTypeValue;
        }
        return flags;
    }

    void DacSession::Modules(P::Writer& out)
    {
        m_modulesLoaded = false;
        LoadModules();
        out.U32(static_cast<std::uint32_t>(m_modules.size()));
        for (const ModuleRecord& record : m_modules)
        {
            out.U64(record.Address);
            out.Text(record.Name);
            out.Text(record.Path);
            out.U64(record.Base);
            out.U32(record.Size);
            out.U8(record.Dynamic ? 1 : 0);
        }
    }

    void DacSession::Types(ClrAddress module, P::Writer& out)
    {
        std::vector<std::pair<UINT, ClrAddress>> found;
        Trace("types %llX: traverse", static_cast<unsigned long long>(module));
        m_sos->TraverseModuleMap(kTypeDefToMethodTable, module, &CollectType, &found);
        Trace("types: %zu method tables, metadata", found.size());
        const Target::ClrMetadata* metadata = MetadataOf(module);
        Trace("types: metadata %s", metadata ? "parsed" : "none");
        struct Row
        {
            ClrAddress Mt;
            std::uint32_t Token;
            std::string Namespace;
            std::string Name;
            ClrAddress Parent;
            std::uint32_t Flags;
            std::uint32_t BaseSize;
        };
        std::vector<Row> rows;
        rows.reserve(found.size());
        for (const auto& [index, mt] : found)
        {
            if ((rows.size() & 255) == 0)
                Trace("types: at %zu", rows.size());
            if (!mt)
                continue;
            const TypeInfo& info = Type(mt);
            if (!info.Valid)
                continue;
            Row row{ mt, info.Data.Token, {}, {}, info.Data.ParentMethodTable, TypeFlags(mt, metadata == nullptr), info.Data.BaseSize };
            const std::uint32_t rid = info.Data.Token & 0x00FFFFFF;
            if (metadata && (info.Data.Token >> 24) == 0x02 && rid)
            {
                std::uint32_t outer = rid;
                for (int depth = 0; depth < 16 && metadata->EnclosingOf(outer); ++depth)
                    outer = metadata->EnclosingOf(outer);
                const std::string full = metadata->TypeDefName(rid);
                const auto top = metadata->TypeDefAt(outer);
                row.Namespace = top ? top->Namespace : std::string();
                row.Name = !row.Namespace.empty() && full.size() > row.Namespace.size() + 1 ? full.substr(row.Namespace.size() + 1) : full;
                if (metadata->HasGenericParams(rid))
                    row.Flags |= P::kTypeGeneric;
            }
            if (row.Name.empty())
            {
                auto [space, name] = SplitName(NameOf(mt));
                row.Namespace = std::move(space);
                row.Name = std::move(name);
            }
            if (row.Name.empty() || row.Name == "<Module>")
                continue;
            rows.push_back(std::move(row));
        }
        out.U32(static_cast<std::uint32_t>(rows.size()));
        for (const Row& row : rows)
        {
            out.U64(row.Mt);
            out.U32(row.Token);
            out.Text(row.Namespace);
            out.Text(row.Name);
            out.U64(row.Parent);
            out.U32(row.Flags);
            out.U32(row.BaseSize);
        }
    }

    ClrAddress DacSession::StaticBase(ClrAddress mt, ClrAddress module, bool primitive)
    {
        if (m_sos14)
        {
            ClrAddress nonGc = 0, gc = 0;
            if (FAILED(m_sos14->GetStaticBaseAddress(mt, &nonGc, &gc)))
                return 0;
            return primitive ? nonGc : gc;
        }
        Padded<DacpDomainLocalModuleData> local;
        HRESULT result = m_sos->GetDomainLocalModuleDataFromModule(module, local.Ptr());
        // A domain-neutral module (mscorlib on the .NET Framework) keeps its
        // statics per domain: the game's own domain is the first one.
        if (FAILED(result) && m_defaultDomain)
        {
            if (const ModuleRecord* record = FindModule(module))
                result = m_sos->GetDomainLocalModuleDataFromAppDomain(m_defaultDomain, static_cast<int>(record->ModuleId), local.Ptr());
        }
        if (FAILED(result))
            return 0;
        return primitive ? local->NonGcStaticDataStart : local->GcStaticDataStart;
    }

    void DacSession::Members(ClrAddress mt, P::Writer& out)
    {
        const TypeInfo info = Type(mt);
        if (!info.Valid)
        {
            out.U64(0);
            out.Text({});
            out.U32(0);
            out.U32(0);
            out.U32(0);
            return;
        }
        const std::uint32_t flags = TypeFlags(mt);
        const bool valueType = (flags & P::kTypeValue) != 0;
        const bool generic = (flags & P::kTypeGeneric) != 0;
        const Target::ClrMetadata* metadata = MetadataOf(info.Data.Module);
        const ClrAddress parent = info.Data.ParentMethodTable;
        out.U64(parent);
        out.Text(parent ? NameOf(parent) : std::string());
        out.U32(flags);

        struct Field
        {
            std::string Name;
            std::uint32_t Token = 0;
            std::uint32_t Flags = 0;
            std::int32_t Offset = 0;
            std::uint32_t Element = 0;
            std::uint32_t EnumCode = 0;
            ClrAddress TypeMt = 0;
            std::string TypeName;
            ClrAddress Address = 0;
            std::int32_t ValueSize = 0;
        };
        std::vector<Field> fields;
        Padded<DacpMethodTableFieldData> fieldData;
        if (SUCCEEDED(m_sos->GetMethodTableFieldData(mt, fieldData.Ptr())))
        {
            std::uint32_t inherited = 0;
            Padded<DacpMethodTableFieldData> parentData;
            if (parent && SUCCEEDED(m_sos->GetMethodTableFieldData(parent, parentData.Ptr())))
                inherited = parentData->NumInstanceFields;
            const std::uint32_t own = fieldData->NumInstanceFields >= inherited ? fieldData->NumInstanceFields - inherited : fieldData->NumInstanceFields;
            const std::uint32_t total = std::min<std::uint32_t>(own + fieldData->NumStaticFields + fieldData->NumThreadStaticFields, kMaxFields);
            ClrAddress at = fieldData->FirstField;
            for (std::uint32_t i = 0; i < total && at; ++i)
            {
                Padded<DacpFieldDescData> data;
                if (FAILED(m_sos->GetFieldDescData(at, data.Ptr())))
                    break;
                at = data->NextField;
                Field field;
                field.Token = data->Token;
                field.Element = data->Type;
                field.TypeMt = data->MtOfType;
                const std::uint32_t rid = data->Token & 0x00FFFFFF;
                if (metadata && (data->Token >> 24) == 0x04)
                {
                    if (const auto row = metadata->FieldAt(rid))
                        field.Name = row->Name;
                    field.TypeName = metadata->FieldTypeName(rid);
                }
                if (field.Name.empty())
                    field.Name = "field_" + Hex(data->Offset);
                if (field.TypeName.empty() && data->MtOfType)
                    field.TypeName = NameOf(data->MtOfType);
                if (data->Type == kElementValueType && data->MtOfType)
                {
                    const TypeInfo& inner = Type(data->MtOfType);
                    if (inner.Valid && inner.Data.BaseSize > 2 * kPointer)
                        field.ValueSize = static_cast<std::int32_t>(inner.Data.BaseSize - 2 * kPointer);
                    if (TypeFlags(data->MtOfType) & P::kTypeEnum)
                    {
                        Padded<DacpMethodTableFieldData> enumData;
                        Padded<DacpFieldDescData> value;
                        if (SUCCEEDED(m_sos->GetMethodTableFieldData(data->MtOfType, enumData.Ptr())) && enumData->FirstField
                            && SUCCEEDED(m_sos->GetFieldDescData(enumData->FirstField, value.Ptr())))
                            field.EnumCode = value->Type;
                    }
                }
                if (data->IsThreadLocal || data->IsContextLocal)
                {
                    field.Flags = P::kFieldStatic | P::kFieldThreadStatic | P::kFieldUnallocated;
                    field.Offset = static_cast<std::int32_t>(data->Offset);
                }
                else if (data->IsStatic)
                {
                    field.Flags = P::kFieldStatic;
                    field.Offset = static_cast<std::int32_t>(data->Offset);
                    const bool primitive = Primitive(data->Type);
                    if (!primitive && data->Type == kElementValueType)
                        field.Flags |= P::kFieldBoxed;
                    // A generic type's statics live per instantiation, not here.
                    const ClrAddress base = generic ? 0 : StaticBase(mt, info.Data.Module, primitive);
                    if (base)
                        field.Address = base + data->Offset;
                    else
                        field.Flags |= P::kFieldUnallocated;
                }
                else
                {
                    // ∑ The DAC's offset starts after the object's method
                    // table pointer. Fields are reported from the object's
                    // start; a struct's from where a boxed copy's would be, two
                    // pointers in, the same convention Mono's offsets have, so
                    // Structure Dissect embeds both alike.
                    field.Offset = static_cast<std::int32_t>(data->Offset + (valueType ? 2 * kPointer : kPointer));
                }
                fields.push_back(std::move(field));
            }
        }
        out.U32(static_cast<std::uint32_t>(fields.size()));
        for (const Field& field : fields)
        {
            out.Text(field.Name);
            out.U32(field.Token);
            out.U32(field.Flags);
            out.I32(field.Offset);
            out.U32(field.Element);
            out.U32(field.EnumCode);
            out.U64(field.TypeMt);
            out.Text(field.TypeName);
            out.U64(field.Address);
            out.I32(field.ValueSize);
        }

        struct Method
        {
            std::string Name;
            std::uint32_t Token = 0;
            std::uint32_t Flags = 0;
            std::int32_t Params = -1;
            ClrAddress Md = 0;
            ClrAddress Code = 0;
        };
        std::vector<Method> methods;
        const std::uint32_t typeRid = info.Data.Token & 0x00FFFFFF;
        const auto row = metadata && (info.Data.Token >> 24) == 0x02 ? metadata->TypeDefAt(typeRid) : std::nullopt;
        if (row)
        {
            for (std::uint32_t rid = row->MethodFirst; rid < row->MethodEnd && methods.size() < kMaxMethods; ++rid)
            {
                const auto methodRow = metadata->MethodAt(rid);
                if (!methodRow)
                    break;
                Method method;
                method.Name = methodRow->Name;
                method.Token = 0x06000000u | rid;
                method.Params = metadata->ParamCount(rid);
                if (methodRow->Flags & kMethodStatic)
                    method.Flags |= P::kMethodStatic;
                if (methodRow->Flags & kMethodVirtual)
                    method.Flags |= P::kMethodVirtual;
                if (methodRow->Flags & kMethodAbstract)
                    method.Flags |= P::kMethodAbstract;
                ClrAddress md = 0;
                if (SUCCEEDED(m_sos->GetMethodDescFromToken(info.Data.Module, method.Token, &md)) && md)
                {
                    method.Md = md;
                    Padded<DacpMethodDescData> data;
                    if (SUCCEEDED(m_sos->GetMethodDescData(md, 0, data.Ptr(), 0, nullptr, nullptr)) && data->HasNativeCode)
                        method.Code = data->NativeCodeAddr;
                }
                methods.push_back(std::move(method));
            }
        }
        else
        {
            // No metadata (a dynamic module): the method table's slots, named
            // by the runtime.
            for (unsigned slot = 0; slot < info.Data.NumMethods && methods.size() < kMaxMethods; ++slot)
            {
                ClrAddress entry = 0;
                if (FAILED(m_sos->GetMethodTableSlot(mt, slot, &entry)) || !entry)
                    continue;
                ClrAddress md = 0;
                Padded<DacpCodeHeaderData> header;
                if (SUCCEEDED(m_sos->GetCodeHeaderData(entry, header.Ptr())) && header->MethodDescPtr)
                    md = header->MethodDescPtr;
                else if (FAILED(m_sos->GetMethodDescPtrFromIP(entry, &md)))
                    md = 0;
                if (!md)
                    continue;
                Padded<DacpMethodDescData> data;
                if (FAILED(m_sos->GetMethodDescData(md, 0, data.Ptr(), 0, nullptr, nullptr)) || data->MethodTablePtr != mt)
                    continue;
                Method method;
                method.Md = md;
                method.Token = data->Token;
                method.Code = data->HasNativeCode ? data->NativeCodeAddr : 0;
                const std::string full = MethodName(md, nullptr);
                const std::size_t colon = full.rfind(':');
                method.Name = colon == std::string::npos ? full : full.substr(colon + 1);
                methods.push_back(std::move(method));
            }
        }
        out.U32(static_cast<std::uint32_t>(methods.size()));
        for (const Method& method : methods)
        {
            out.Text(method.Name);
            out.U32(method.Token);
            out.U32(method.Flags);
            out.I32(method.Params);
            out.U64(method.Md);
            out.U64(method.Code);
        }
    }

    void DacSession::DescribeType(ClrAddress mt, P::Writer& out)
    {
        const TypeInfo& info = Type(mt);
        if (!info.Valid || info.Data.IsFree || NameOf(mt).empty() || mt == m_globals.FreeMethodTable)
        {
            out.U8(0);
            return;
        }
        const auto [space, name] = SplitName(NameOf(mt));
        out.U8(1);
        out.Text(space);
        out.Text(name);
        out.U64(info.Data.Module);
        out.U64(info.Data.ParentMethodTable);
        out.U32(TypeFlags(mt));
        out.U32(info.Data.BaseSize);
        out.U32(info.Data.ComponentSize);
    }

    std::string DacSession::MethodName(ClrAddress md, ClrAddress* typeOut)
    {
        Padded<DacpMethodDescData> data;
        if (FAILED(m_sos->GetMethodDescData(md, 0, data.Ptr(), 0, nullptr, nullptr)))
            return {};
        if (typeOut)
            *typeOut = data->MethodTablePtr;
        const std::string type = NameOf(data->MethodTablePtr);
        std::string method;
        if ((data->Token >> 24) == 0x06)
        {
            if (const Target::ClrMetadata* metadata = MetadataOf(data->ModulePtr))
            {
                if (const auto row = metadata->MethodAt(data->Token & 0x00FFFFFF))
                    method = row->Name;
            }
        }
        if (method.empty())
        {
            // "Ns.Type.Method(Args)": the name is between the last dot ahead
            // of the arguments and the parenthesis.
            const std::string full = AskName([&](unsigned n, WCHAR* text, unsigned* got) { return m_sos->GetMethodDescName(md, n, text, got); });
            const std::size_t open = full.find('(');
            const std::size_t dot = full.rfind('.', open == std::string::npos ? std::string::npos : open);
            method = full.substr(dot == std::string::npos ? 0 : dot + 1, open == std::string::npos ? std::string::npos : open - (dot == std::string::npos ? 0 : dot + 1));
        }
        if (type.empty() || method.empty())
            return {};
        return type + ":" + method;
    }

    void DacSession::MethodAt(ClrAddress ip, P::Writer& out)
    {
        Padded<DacpCodeHeaderData> header;
        if (FAILED(m_sos->GetCodeHeaderData(ip, header.Ptr())) || !header->MethodDescPtr)
        {
            out.U8(0);
            return;
        }
        ClrAddress start = header->MethodStart;
        std::uint32_t size = header->MethodSize;
        const bool hot = ip >= start && ip < start + size;
        const bool cold = header->ColdRegionStart && ip >= header->ColdRegionStart && ip < header->ColdRegionStart + header->ColdRegionSize;
        if (cold)
        {
            start = header->ColdRegionStart;
            size = header->ColdRegionSize;
        }
        else if (!hot)
        {
            out.U8(0);
            return;
        }
        const std::string name = MethodName(header->MethodDescPtr, nullptr);
        if (name.empty())
        {
            out.U8(0);
            return;
        }
        out.U8(1);
        out.Text(name);
        out.U64(start);
        out.U32(size);
        out.U64(header->MethodDescPtr);
    }

    void DacSession::MethodCode(ClrAddress md, P::Writer& out)
    {
        Padded<DacpMethodDescData> data;
        if (FAILED(m_sos->GetMethodDescData(md, 0, data.Ptr(), 0, nullptr, nullptr)) || !data->HasNativeCode)
        {
            out.U64(0);
            return;
        }
        out.U64(data->NativeCodeAddr);
    }

    std::string DacSession::HeapSegments(std::vector<HeapSegment>& out)
    {
        Padded<DacpGcHeapData> heap;
        if (FAILED(m_sos->GetGCHeapData(heap.Ptr())))
            return "the runtime did not describe its GC heap";
        if (!heap->GcStructuresValid)
            return "the game is in the middle of a garbage collection; try again";
        std::vector<DacpGcHeapDetails> heaps;
        if (heap->ServerMode)
        {
            const unsigned count = std::min<unsigned>(heap->HeapCount, 1024);
            std::vector<ClrAddress> list(count);
            unsigned needed = 0;
            if (!count || FAILED(m_sos->GetGCHeapList(count, list.data(), &needed)))
                return "the runtime did not list its GC heaps";
            for (unsigned i = 0; i < std::min(count, needed); ++i)
            {
                Padded<DacpGcHeapDetails> details;
                if (SUCCEEDED(m_sos->GetGCHeapDetails(list[i], details.Ptr())))
                    heaps.push_back(details.Value);
            }
        }
        else
        {
            Padded<DacpGcHeapDetails> details;
            if (FAILED(m_sos->GetGCHeapStaticData(details.Ptr())))
                return "the runtime did not describe its GC heap";
            heaps.push_back(details.Value);
        }
        // Every generation's list of segments (regions on .NET 7 and later),
        // the large object heap's among them; a segment is in several lists
        // under the older layout, so each one is walked once.
        std::set<ClrAddress> seen;
        for (const DacpGcHeapDetails& details : heaps)
        {
            for (int generation = 0; generation < kDacGenerations; ++generation)
            {
                ClrAddress segment = details.GenerationTable[generation].StartSegment;
                for (int guard = 0; segment && guard < 1000000; ++guard)
                {
                    if (!seen.insert(segment).second)
                        break;
                    Padded<DacpHeapSegmentData> data;
                    if (FAILED(m_sos->GetHeapSegmentData(segment, data.Ptr())))
                        break;
                    HeapSegment part;
                    part.Start = data->Mem;
                    // The segment the GC allocates in is filled up to the heap's
                    // allocation pointer; its own mark lags behind.
                    part.End = segment == details.EphemeralHeapSegment && details.AllocAllocated ? details.AllocAllocated : data->Allocated;
                    part.Large = generation == kDacGenerations - 1;
                    if (part.Start && part.End > part.Start)
                        out.push_back(part);
                    segment = data->Next;
                }
            }
        }
        if (out.empty())
            return "the runtime's GC heap has no segments";
        return {};
    }

    const DacSession::ObjectSize& DacSession::SizeOf(ClrAddress mt)
    {
        if (const auto found = m_sizes.find(mt); found != m_sizes.end())
            return found->second;
        ObjectSize size;
        // A method table is pointer aligned and never in the first 64 KB; a
        // word that fails this is no object header, and asking the DAC about
        // every such word would be slow.
        if (mt >= 0x10000 && (mt & (kPointer - 1)) == 0)
        {
            const TypeInfo& info = Type(mt);
            if (info.Valid && info.Data.BaseSize >= 2 * kPointer && info.Data.BaseSize < 0x1000000)
            {
                size.Valid = true;
                size.Base = info.Data.BaseSize;
                size.Component = info.Data.ComponentSize;
            }
        }
        if (m_sizes.size() > 2000000)
            m_sizes.clear();
        return m_sizes.emplace(mt, size).first->second;
    }

    template <typename Visit>
    std::string DacSession::WalkHeap(Visit&& visit, HeapTotals& totals)
    {
        std::vector<HeapSegment> segments;
        if (std::string error = HeapSegments(segments); !error.empty())
            return error;
        constexpr std::size_t kChunk = 4u << 20;
        std::vector<std::uint8_t> buffer(kChunk);
        for (const HeapSegment& segment : segments)
        {
            ++totals.Segments;
            ClrAddress bufferStart = 0;
            std::size_t bufferLength = 0;
            // The bytes at [at, at + need) from a window over the segment,
            // read again whenever the walk leaves it.
            const auto fetch = [&](ClrAddress at, std::size_t need) -> const std::uint8_t*
                {
                    if (bufferLength && at >= bufferStart && at + need <= bufferStart + bufferLength)
                        return buffer.data() + (at - bufferStart);
                    const std::size_t wanted = static_cast<std::size_t>(std::min<ClrAddress>(kChunk, segment.End - at));
                    bufferStart = at;
                    bufferLength = m_target.ReadDirect(at, buffer.data(), wanted);
                    return bufferLength >= need ? buffer.data() : nullptr;
                };
            const ClrAddress align = segment.Large ? 8 : kPointer;
            ClrAddress at = segment.Start;
            while (at + 2 * kPointer <= segment.End)
            {
                const std::uint8_t* header = fetch(at, 2 * kPointer);
                if (!header)
                {
                    // An unreadable page: go on after it.
                    at = (at + 0x1000) & ~static_cast<ClrAddress>(0xFFF);
                    ++totals.Skipped;
                    continue;
                }
                ClrAddress mt = 0;
                std::memcpy(&mt, header, kPointer);
                mt &= ~static_cast<ClrAddress>(3);  // the GC's mark bits
                const ObjectSize& size = mt ? SizeOf(mt) : ObjectSize{};
                if (!size.Valid)
                {
                    // Zeroed space (an allocation context not handed out yet)
                    // or a word inside something else: step on to the next one.
                    at += kPointer;
                    ++totals.Skipped;
                    continue;
                }
                std::uint64_t bytes = size.Base;
                if (size.Component)
                {
                    std::uint32_t count = 0;
                    std::memcpy(&count, header + kPointer, sizeof(count));
                    bytes += static_cast<std::uint64_t>(count) * size.Component;
                }
                bytes = (bytes + align - 1) & ~(align - 1);
                if (at + bytes > segment.End)
                    bytes = segment.End - at;
                ++totals.Objects;
                totals.Bytes += bytes;
                if (!visit(at, mt, bytes))
                    return {};
                at += bytes;
            }
        }
        return {};
    }

    std::string DacSession::HeapStats(P::Writer& out)
    {
        struct Count
        {
            std::uint64_t Objects = 0;
            std::uint64_t Bytes = 0;
        };
        std::unordered_map<ClrAddress, Count> counts;
        HeapTotals totals;
        const std::string error = WalkHeap([&](ClrAddress, ClrAddress mt, std::uint64_t bytes)
            {
                Count& count = counts[mt];
                ++count.Objects;
                count.Bytes += bytes;
                return true;
            }, totals);
        if (!error.empty())
            return error;
        out.U64(totals.Objects);
        out.U64(totals.Bytes);
        out.U32(totals.Segments);
        out.U64(totals.Skipped);
        out.U32(static_cast<std::uint32_t>(counts.size()));
        for (const auto& [mt, count] : counts)
        {
            out.U64(mt);
            out.Text(mt == m_globals.FreeMethodTable ? std::string("Free") : NameOf(mt));
            out.U64(count.Objects);
            out.U64(count.Bytes);
        }
        return {};
    }

    std::string DacSession::Instances(ClrAddress mt, std::uint32_t limit, P::Writer& out)
    {
        std::vector<ClrAddress> found;
        std::uint64_t total = 0;
        HeapTotals totals;
        const std::string error = WalkHeap([&](ClrAddress at, ClrAddress type, std::uint64_t)
            {
                if (type == mt)
                {
                    ++total;
                    if (found.size() < limit)
                        found.push_back(at);
                }
                return true;
            }, totals);
        if (!error.empty())
            return error;
        out.U64(total);
        out.U32(static_cast<std::uint32_t>(found.size()));
        for (ClrAddress address : found)
            out.U64(address);
        return {};
    }

    void DacSession::ObjectInfo(ClrAddress address, P::Writer& out)
    {
        Padded<DacpObjectData> data;
        if (!address || FAILED(m_sos->GetObjectData(address, data.Ptr())) || !data->MethodTable)
        {
            out.U8(0);
            return;
        }
        out.U8(1);
        out.U64(data->MethodTable);
        out.Text(data->ObjectType == kObjectFree ? std::string("Free") : NameOf(data->MethodTable));
        out.U32(static_cast<std::uint32_t>(data->ObjectType));
        out.U64(data->Size);
        // A type handle with the low bit set is a type descriptor, not a
        // method table (pointers, function pointers); no class to name.
        const ClrAddress element = (data->ElementTypeHandle & 1) ? 0 : data->ElementTypeHandle;
        out.U64(element);
        out.Text(element ? NameOf(element) : std::string());
        out.U32(data->ElementType);
        out.U32(data->Rank);
        // The DAC counts only an array's elements; a string's length is its
        // first field, right after the method table.
        std::uint64_t length = data->NumComponents;
        if (data->ObjectType == kObjectString)
        {
            std::uint32_t characters = 0;
            length = m_target.Read(address + kPointer, &characters, sizeof(characters)) ? characters : 0;
        }
        out.U64(length);
        out.U64(data->ComponentSize);
        out.U64(data->ArrayDataPtr);
    }
}
