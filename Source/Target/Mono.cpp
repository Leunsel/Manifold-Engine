#include "Target/Mono.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Il2CppRuntime.h"
#include "Target/MonoRuntime.h"
#include "Target/Process.h"
#include "Target/UnityExplorer.h"

#include <algorithm>

namespace Manifold::Target
{
    namespace
    {
        const char* const kModuleCandidates[] = { "mono-2.0-bdwgc.dll", "mono.dll", "mono-2.0.dll", "monobdwgc-2.0.dll" };
        constexpr const char* kIl2CppModule = "GameAssembly.dll";

        std::string_view WithoutDll(std::string_view name)
        {
            if (name.size() > 4 && EqualsNoCase(name.substr(name.size() - 4), ".dll"))
                name.remove_suffix(4);
            return name;
        }
    }

    const char* MonoBridge::RuntimeName(ManagedRuntime runtime)
    {
        switch (runtime)
        {
            case ManagedRuntime::Mono: return "Mono";
            case ManagedRuntime::Il2Cpp: return "IL2CPP";
            default: return "no runtime";
        }
    }

    ManagedRuntime MonoBridge::DetectRuntime(const Process& process)
    {
        const ModuleList modules = process.Modules();
        if (!modules)
            return ManagedRuntime::None;
        // GameAssembly.dll wins: an IL2CPP game with a mod loader that brings
        // its own Mono (MelonLoader) keeps the game's classes in IL2CPP.
        ManagedRuntime found = ManagedRuntime::None;
        for (const ModuleInfo& module : *modules)
        {
            if (EqualsNoCase(module.Name, kIl2CppModule))
                return ManagedRuntime::Il2Cpp;
            for (const char* candidate : kModuleCandidates)
            {
                if (EqualsNoCase(module.Name, candidate))
                    found = ManagedRuntime::Mono;
            }
        }
        return found;
    }

    bool MonoBridge::HasRuntime(const Process& process)
    {
        return DetectRuntime(process) != ManagedRuntime::None;
    }

    std::vector<MonoBridge::Candidate> MonoBridge::FindRuntimes(const Process& process, WorkProgress* progress)
    {
        std::vector<Candidate> found;
        if (process.FindModule(kIl2CppModule) && process.FindExport(kIl2CppModule, "il2cpp_domain_get"))
            found.push_back({ kIl2CppModule, ManagedRuntime::Il2Cpp });
        for (const char* candidate : kModuleCandidates)
        {
            if (process.FindModule(candidate) && process.FindExport(candidate, "mono_get_root_domain"))
            {
                found.push_back({ candidate, ManagedRuntime::Mono });
                break;
            }
        }
        if (!found.empty())
            return found;
        // A renamed or embedded runtime: any module with the exports. Mono is
        // tried first within a module, IL2CPP after it.
        if (const auto modules = process.Modules())
        {
            for (const ModuleInfo& info : *modules)
            {
                if (progress && progress->Cancelled())
                    return {};
                if (process.FindExport(info.Name, "mono_get_root_domain"))
                    found.push_back({ info.Name, ManagedRuntime::Mono });
                if (process.FindExport(info.Name, "il2cpp_domain_get"))
                    found.push_back({ info.Name, ManagedRuntime::Il2Cpp });
            }
        }
        return found;
    }

    Status MonoBridge::AttachRuntime(const Candidate& candidate)
    {
        std::unique_ptr<RuntimeBackend> backend;
        if (candidate.Runtime == ManagedRuntime::Il2Cpp)
            backend = std::make_unique<Il2CppRuntime>(*this);
        else
            backend = std::make_unique<MonoRuntime>(*this);
        const Status attached = backend->Attach(candidate.Module);
        if (attached)
            m_backend = std::move(backend);
        return attached;
    }

    std::uint64_t MonoBridge::Fn(const char* name) const
    {
        const auto found = m_functions.find(name);
        return found == m_functions.end() ? 0 : found->second;
    }

    std::optional<std::uint64_t> MonoBridge::CallFn(const char* name, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        const std::uint64_t fn = Fn(name);
        if (!fn)
            return std::nullopt;
        std::uint64_t result = 0;
        if (!m_call.Call(fn, a0, a1, a2, a3, result))
            return std::nullopt;
        return result;
    }

    std::string MonoBridge::CallString(const char* name, std::uint64_t handle)
    {
        const auto pointer = CallFn(name, handle);
        if (!pointer || !*pointer)
            return {};
        return m_call.ReadCString(*pointer);
    }

    Status MonoBridge::Start(std::shared_ptr<Process> process, WorkProgress* progress)
    {
        Stop();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (!process->Is64Bit())
            return Status::Fail(ErrorCode::MonoNotFound, "only 64-bit Mono and IL2CPP targets are supported");
        if (progress)
            progress->SetText("Looking for the runtime");

        const std::vector<Candidate> candidates = FindRuntimes(*process, progress);
        if (progress && progress->Cancelled())
            return Status::Fail(ErrorCode::MonoNotFound, "cancelled");
        if (candidates.empty())
            return Status::Fail(ErrorCode::MonoNotFound);

        if (progress)
            progress->SetText("Starting the helper in the game");
        std::string module;
        ManagedRuntime runtime = ManagedRuntime::None;
        {
            std::lock_guard lock(m_lock);
            m_process = process;
            if (Status started = m_call.Start(process); !started)
            {
                m_process.reset();
                return started;
            }
            // The first candidate whose runtime answers with a domain wins; a
            // runtime that is not set up yet leaves the next one its turn.
            Status attached = Status::Fail(ErrorCode::MonoNotFound);
            for (const Candidate& candidate : candidates)
            {
                if (progress && progress->Cancelled())
                {
                    attached = Status::Fail(ErrorCode::MonoNotFound, "cancelled");
                    break;
                }
                attached = AttachRuntime(candidate);
                if (attached)
                    break;
            }
            if (!attached)
            {
                m_call.Stop();
                m_functions.clear();
                m_module.clear();
                m_domain = 0;
                m_backend.reset();
                m_runtime = ManagedRuntime::None;
                m_process.reset();
                return attached;
            }
            if (const auto info = process->FindModule(m_module))
            {
                m_moduleBase = info->Base;
                m_moduleEnd = info->Base + info->Size;
            }
            module = m_module;
            runtime = m_runtime.load();
        }
        LOG_SUCCESS("Mono", std::string(RuntimeName(runtime)) + " runtime found in " + module);
        const Status enumerated = Enumerate(progress, true);
        if (!enumerated)
            Stop();
        return enumerated;
    }

    MonoBridge::MonoBridge() = default;

    MonoBridge::~MonoBridge()
    {
        Stop();
    }

    UnityExplorer& MonoBridge::Unity()
    {
        std::lock_guard lock(m_unityLock);
        if (!m_unity)
            m_unity = std::make_unique<UnityExplorer>(*this);
        return *m_unity;
    }

    void MonoBridge::Stop()
    {
        // The explorer first, without holding m_lock: its job thread takes
        // m_lock per call and is joined here, and it takes its callback out of
        // the game through the worker before the worker goes.
        {
            std::lock_guard unityLock(m_unityLock);
            if (m_unity)
                m_unity->Shutdown();
        }
        std::lock_guard lock(m_lock);
        if (m_backend)
            m_backend->Detach();
        m_call.Stop();
        m_backend.reset();
        m_functions.clear();
        m_classes.clear();
        m_classIndex.clear();
        m_handleIndex.clear();
        m_images.clear();
        m_present = false;
        m_runtime = ManagedRuntime::None;
        m_domain = 0;
        m_module.clear();
        m_moduleBase = m_moduleEnd = 0;
        m_process.reset();
        ++m_generation;
    }

    Status MonoBridge::Refresh(WorkProgress* progress)
    {
        if (!m_present.load())
            return Status::Fail(ErrorCode::MonoNotFound);
        return Enumerate(progress, false);
    }

    Status MonoBridge::Enumerate(WorkProgress* progress, bool starting)
    {
        RuntimeBackend* backend = nullptr;
        {
            std::lock_guard lock(m_lock);
            backend = m_backend.get();
        }
        if (!backend)
            return Status::Fail(ErrorCode::MonoNotFound);
        return backend->Enumerate(progress, starting);
    }

    int MonoBridge::FindClass(std::string_view name) const
    {
        const auto found = m_classIndex.find(std::string(name));
        if (found != m_classIndex.end())
            return found->second;
        // "Image:Class" or "Image:Namespace.Class", the image with or without
        // its .dll, as in Assembly-CSharp:Player:TakeDamage.
        const std::size_t colon = name.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= name.size())
            return -1;
        const std::string_view image = WithoutDll(name.substr(0, colon));
        const std::string_view klass = name.substr(colon + 1);
        int byName = -1;
        for (int index = 0; index < static_cast<int>(m_classes.size()); ++index)
        {
            const MonoClass& info = m_classes[static_cast<std::size_t>(index)];
            if (!EqualsNoCase(WithoutDll(info.Image), image))
                continue;
            if (info.FullName() == klass)
                return index;
            if (byName < 0 && info.Name == klass)
                byName = index;
        }
        return byName;
    }

    void MonoBridge::EnsureMembers(int classIndex)
    {
        if (classIndex < 0 || classIndex >= static_cast<int>(m_classes.size()) || !m_backend)
            return;
        m_backend->EnsureMembers(classIndex);
    }

    std::vector<std::string> MonoBridge::MethodsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return {};
        EnsureMembers(index);
        std::vector<std::string> names;
        for (const MonoMethod& method : m_classes[static_cast<std::size_t>(index)].Methods)
            names.push_back(method.Name);
        return names;
    }

    std::vector<MonoField> MonoBridge::FieldsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return {};
        EnsureMembers(index);
        return m_classes[static_cast<std::size_t>(index)].Fields;
    }

    std::optional<MonoClass> MonoBridge::ClassDetails(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return std::nullopt;
        EnsureMembers(index);
        return m_classes[static_cast<std::size_t>(index)];
    }

    std::vector<MonoClass> MonoBridge::Classes() const
    {
        std::lock_guard lock(m_lock);
        return m_classes;
    }

    std::size_t MonoBridge::ClassCount() const
    {
        std::lock_guard lock(m_lock);
        return m_classes.size();
    }

    std::optional<std::string> MonoBridge::ResolveHead(std::uint64_t header, bool allowRuntimeFallback)
    {
        if (!m_backend)
            return std::nullopt;
        return m_backend->ResolveHead(header, allowRuntimeFallback);
    }

    std::optional<std::string> MonoBridge::ClassNameAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        return ResolveHead(address, true);
    }

    std::optional<MonoInstance> MonoBridge::ClassAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        // The address is often a field inside an object, not its header. Try the
        // address itself first, then step back a little (8-byte aligned) for the
        // enclosing object's header. The runtime fallback is allowed only for the
        // exact address; the back scan keeps to the enumerated classes so a stray
        // field value cannot be mistaken for an object.
        constexpr std::uint64_t kMaxBack = 0x1000;
        const std::uint64_t aligned = address & ~std::uint64_t(7);
        for (std::uint64_t back = 0; back <= kMaxBack; back += 8)
        {
            if (aligned < back + 0x10000)
                break;
            const std::uint64_t header = aligned - back;
            if (const auto name = ResolveHead(header, back == 0))
                return MonoInstance{ header, *name };
        }
        return std::nullopt;
    }

    std::optional<std::string> MonoBridge::MethodNameAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address || !m_backend)
            return std::nullopt;
        return m_backend->MethodNameAt(address);
    }

    std::optional<std::string> MonoBridge::MethodStartNameAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address || !m_backend)
            return std::nullopt;
        return m_backend->MethodStartNameAt(address);
    }

    std::size_t MonoBridge::MethodsSharingCode(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_backend)
            return 0;
        return m_backend->MethodsSharingCode(address);
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> MonoBridge::MethodRange(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address || !m_backend)
            return std::nullopt;
        return m_backend->MethodRange(address);
    }

    std::optional<std::uint64_t> MonoBridge::AddressOf(std::string_view name)
    {
        std::lock_guard lock(m_lock);
        if (!m_present)
            return std::nullopt;
        const std::size_t colon = name.rfind(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= name.size())
            return std::nullopt;
        const int index = FindClass(name.substr(0, colon));
        if (index < 0)
            return std::nullopt;
        EnsureMembers(index);
        const std::string_view methodName = name.substr(colon + 1);
        for (MonoMethod& method : m_classes[static_cast<std::size_t>(index)].Methods)
        {
            if (method.Name != methodName)
                continue;
            if (method.Address)
                return method.Address;
            // IL2CPP has no JIT: the code address was read with the members,
            // and a method without one (abstract, an open generic) has none,
            // so an overload of the same name may still have code.
            if (!m_backend || !m_backend->CompilesOnDemand())
                continue;
            const auto address = m_backend->Compile(method.Handle);
            if (!address)
                return std::nullopt;
            method.Address = *address;
            return method.Address;
        }
        return std::nullopt;
    }

    std::optional<int> MonoBridge::FieldOffset(std::string_view className, std::string_view fieldName)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return std::nullopt;
        EnsureMembers(index);
        for (const MonoField& field : m_classes[static_cast<std::size_t>(index)].Fields)
        {
            if (field.Name == fieldName)
                return field.Offset;
        }
        return std::nullopt;
    }
}
