#include "Target/Mono.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/ClrRuntime.h"
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
        // .NET (CoreCLR), the .NET Framework 4 and 2.
        const char* const kClrModules[] = { "coreclr.dll", "clr.dll", "mscorwks.dll" };

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
            case ManagedRuntime::Clr: return ".NET";
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
        // A .NET runtime counts only without Mono: a Unity tool or mod
        // loader may bring one along.
        ManagedRuntime found = ManagedRuntime::None;
        bool clr = false;
        for (const ModuleInfo& module : *modules)
        {
            if (EqualsNoCase(module.Name, kIl2CppModule))
                return ManagedRuntime::Il2Cpp;
            for (const char* candidate : kModuleCandidates)
            {
                if (EqualsNoCase(module.Name, candidate))
                    found = ManagedRuntime::Mono;
            }
            for (const char* candidate : kClrModules)
                clr = clr || EqualsNoCase(module.Name, candidate);
        }
        return found == ManagedRuntime::None && clr ? ManagedRuntime::Clr : found;
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
        for (const char* candidate : kClrModules)
        {
            if (process.FindModule(candidate))
            {
                found.push_back({ candidate, ManagedRuntime::Clr });
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
        else if (candidate.Runtime == ManagedRuntime::Clr)
            backend = std::make_unique<ClrRuntime>(*this);
        else
            backend = std::make_unique<MonoRuntime>(*this);
        const Status attached = backend->Attach(candidate.Module);
        if (attached)
            m_backend = std::move(backend);
        return attached;
    }

    std::string MonoBridge::RuntimeDescription() const
    {
        std::lock_guard lock(m_lock);
        if (m_backend)
        {
            std::string text = m_backend->Description();
            if (!text.empty())
                return text;
        }
        return RuntimeName(m_runtime.load());
    }

    std::uint64_t MonoBridge::ManagedClass(std::string_view space, std::string_view name)
    {
        std::lock_guard lock(m_lock);
        if (!m_backend || !m_present)
            return 0;
        return m_backend->ManagedClass(space, name);
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
        if (progress)
            progress->SetText("Looking for the runtime");

        const std::vector<Candidate> candidates = FindRuntimes(*process, progress);
        if (progress && progress->Cancelled())
            return Status::Fail(ErrorCode::MonoNotFound, "cancelled");
        if (candidates.empty())
            return Status::Fail(ErrorCode::MonoNotFound);

        // .NET is read from outside the game; the worker in the game is
        // started only for a Mono or IL2CPP candidate.
        const bool needsWorker = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.Runtime != ManagedRuntime::Clr; });
        if (progress)
            progress->SetText(needsWorker ? "Starting the helper in the game" : "Starting the .NET helper");
        std::string module;
        ManagedRuntime runtime = ManagedRuntime::None;
        {
            std::lock_guard lock(m_lock);
            m_process = process;
            m_call.SetGuard(&m_lock);
            if (needsWorker)
            {
                if (Status started = m_call.Start(process); !started)
                {
                    m_process.reset();
                    return started;
                }
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

    std::optional<UnityStatus> MonoBridge::UnityState()
    {
        std::lock_guard lock(m_unityLock);
        if (!m_unity)
            return std::nullopt;
        return m_unity->State();
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
        m_readByDumper = false;
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

    std::vector<MonoField> MonoBridge::TypedFieldsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        return TypedFields(FindClass(className));
    }

    std::vector<MonoField> MonoBridge::TypedFieldsOf(std::uint64_t klass)
    {
        std::lock_guard lock(m_lock);
        const auto found = m_handleIndex.find(klass);
        return TypedFields(found == m_handleIndex.end() ? -1 : found->second);
    }

    std::vector<MonoField> MonoBridge::InstanceFieldsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        return InstanceFields(FindClass(className));
    }

    std::vector<MonoField> MonoBridge::InstanceFieldsAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address || !ResolveHead(address, true))
            return {};
        const auto found = m_handleIndex.find(ClassHandleAt(address));
        return found == m_handleIndex.end() ? std::vector<MonoField>{} : InstanceFields(found->second);
    }

    std::vector<MonoField> MonoBridge::InstanceFields(int classIndex)
    {
        std::vector<MonoField> all;
        for (int depth = 0; classIndex >= 0 && depth < 48; ++depth)
        {
            const std::vector<MonoField> own = TypedFields(classIndex);
            all.insert(all.end(), own.begin(), own.end());
            classIndex = ParentIndex(classIndex);
        }
        return all;
    }

    std::uint64_t MonoBridge::ClassHandleAt(std::uint64_t header) const
    {
        // An Il2CppObject starts with its class; a MonoObject with its vtable,
        // which starts with the class.
        const auto first = m_process->ReadPointer(header);
        if (!first || *first < 0x10000)
            return 0;
        // A .NET object starts with its method table, the class handle.
        if (IsIl2Cpp() || IsClr())
            return *first;
        return m_process->ReadPointer(*first).value_or(0);
    }

    std::string MonoBridge::GenericInstanceName(std::uint64_t klass, const std::string& space, const std::string& name)
    {
        // ∑ The runtime's type name tells generic instances apart (List<Item>
        // and List<Enemy> are both "List`1" to class_get_name, and the nested
        // Entry of every Dictionary is "Entry"). Only names that carry type
        // arguments replace the plain one; the result is "Namespace.Rest" when
        // it starts with the namespace.
        const std::string prefix = IsIl2Cpp() ? "il2cpp_" : "mono_";
        if (!klass || (m_paused && m_paused()) || !Fn((prefix + "class_get_type").c_str()) || !Fn((prefix + "type_get_name").c_str()))
            return {};
        const std::uint64_t type = CallFn((prefix + "class_get_type").c_str(), klass).value_or(0);
        const std::uint64_t text = type ? CallFn((prefix + "type_get_name").c_str(), type).value_or(0) : 0;
        if (!text)
            return {};
        std::string full = m_call.ReadCString(text, 512);
        if (Fn((prefix + "free").c_str()))
            CallFn((prefix + "free").c_str(), text);
        const std::string plain = name.substr(0, name.find('`'));
        if (full.size() > 500 || full.find('<') == std::string::npos || plain.empty() || full.find(plain) == std::string::npos ||
            !std::all_of(full.begin(), full.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7F; }))
            return {};
        if (!space.empty() && full.size() > space.size() + 1 && full.compare(0, space.size(), space) == 0 && full[space.size()] == '.')
            return full.substr(space.size() + 1);
        return full;
    }

    int MonoBridge::AdoptClass(std::uint64_t klass)
    {
        // A class the runtime itself handed out (a parent, an array's element
        // class), so asking the runtime about it is safe.
        if (!klass)
            return -1;
        if (const auto known = m_handleIndex.find(klass); known != m_handleIndex.end())
            return known->second;
        if (m_paused && m_paused())
            return -1;
        const std::string prefix = IsIl2Cpp() ? "il2cpp_" : "mono_";
        MonoClass info;
        info.Handle = klass;
        info.Name = CallString((prefix + "class_get_name").c_str(), klass);
        info.Namespace = CallString((prefix + "class_get_namespace").c_str(), klass);
        if (info.Name.empty())
            return -1;
        if (std::string generic = GenericInstanceName(klass, info.Namespace, info.Name); !generic.empty())
            info.Name = std::move(generic);
        if (const std::uint64_t image = CallFn((prefix + "class_get_image").c_str(), klass).value_or(0))
            info.Image = std::string(WithoutDll(CallString((prefix + "image_get_name").c_str(), image)));
        const int index = static_cast<int>(m_classes.size());
        m_classIndex.emplace(info.Name, index);
        if (!info.Namespace.empty())
            m_classIndex.emplace(info.FullName(), index);
        m_handleIndex.emplace(klass, index);
        m_classes.push_back(std::move(info));
        return index;
    }

    int MonoBridge::ParentIndex(int classIndex)
    {
        if (classIndex < 0 || classIndex >= static_cast<int>(m_classes.size()))
            return -1;
        if (m_backend)
        {
            if (const auto known = m_backend->ParentIndex(classIndex))
                return *known;
        }
        if (m_paused && m_paused())
            return -1;
        const std::string prefix = IsIl2Cpp() ? "il2cpp_" : "mono_";
        const std::uint64_t parent = CallFn((prefix + "class_get_parent").c_str(), m_classes[static_cast<std::size_t>(classIndex)].Handle).value_or(0);
        return AdoptClass(parent);
    }

    std::optional<MonoArrayLayout> MonoBridge::ArrayAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        // ∑ The class is read again below and handed to the runtime; under a moving GC the object can be gone
        //   by then, and a call with a stale or null class crashes the game. Both reads must agree. While the
        //   debugger holds the game, every remote call would only wait for its timeout.
        if (m_paused && m_paused())
            return std::nullopt;
        const std::uint64_t before = ClassHandleAt(address);
        if (!before)
            return std::nullopt;
        const auto name = ResolveHead(address, true);
        if (!name || name->empty() || name->back() != ']')
            return std::nullopt;
        const std::uint64_t klass = ClassHandleAt(address);
        if (klass != before)
            return std::nullopt;
        const std::string prefix = IsIl2Cpp() ? "il2cpp_" : "mono_";
        const auto call = [&](const char* function, std::uint64_t argument) -> std::uint64_t
            {
                const std::string full = prefix + function;
                return Fn(full.c_str()) ? CallFn(full.c_str(), argument).value_or(0) : 0;
            };
        MonoArrayLayout layout;
        layout.Name = *name;
        layout.Rank = static_cast<int>(call("class_get_rank", klass) & 0xFFu);
        if (layout.Rank <= 0)
            return std::nullopt;
        const std::uint64_t element = call("class_get_element_class", klass);
        if (!element)
            return std::nullopt;
        const int pointer = m_process->PointerSize();
        layout.ElementSize = static_cast<int>(call(IsIl2Cpp() ? "class_array_element_size" : "array_element_size", klass) & 0xFFFFu);
        if (layout.ElementSize <= 0)
            layout.ElementSize = pointer;
        const MonoObjectLayout object = MonoLayoutFor(pointer);
        layout.DataOffset = static_cast<int>(object.ArrayData);
        layout.Length = m_process->ReadPointer(address + object.ArrayLength).value_or(0);
        const std::uint64_t type = call("class_get_type", element);
        layout.Element.TypeCode = type ? static_cast<int>(call("type_get_type", type) & 0xFFu) : 0;
        if (MayBeValueType(layout.Element.TypeCode))
        {
            AdoptClass(element);
            DescribeValueType(prefix, element, layout.Element);
        }
        m_call.ResetScratch();
        return layout;
    }

    std::vector<MonoField> MonoBridge::TypedFields(int classIndex)
    {
        if (classIndex < 0 || classIndex >= static_cast<int>(m_classes.size()) || !m_backend)
            return {};
        EnsureMembers(classIndex);
        m_backend->ResolveValueTypes(classIndex);
        return m_classes[static_cast<std::size_t>(classIndex)].Fields;
    }

    void MonoBridge::DescribeValueType(const std::string& prefix, std::uint64_t klass, MonoField& field)
    {
        const auto name = [&](const char* function) { return prefix + function; };
        if (!klass || !Fn(name("class_is_valuetype").c_str()) || !Fn(name("class_value_size").c_str()))
            return;
        if ((CallFn(name("class_is_valuetype").c_str(), klass).value_or(0) & 0xFFu) == 0)
            return;
        const std::uint64_t align = m_call.Scratch(8);
        const int size = static_cast<int>(CallFn(name("class_value_size").c_str(), klass, align).value_or(0) & 0xFFFFFFFFu);
        if (size <= 0 || size > 0x10000)
            return;
        field.TypeHandle = klass;
        field.ValueSize = size;
        if (const auto known = m_handleIndex.find(klass); known != m_handleIndex.end())
            field.TypeName = m_classes[static_cast<std::size_t>(known->second)].FullName();
        else
        {
            const std::string space = CallString(name("class_get_namespace").c_str(), klass);
            const std::string type = CallString(name("class_get_name").c_str(), klass);
            field.TypeName = space.empty() ? type : space + "." + type;
        }
        if (!Fn(name("class_is_enum").c_str()) || (CallFn(name("class_is_enum").c_str(), klass).value_or(0) & 0xFFu) == 0)
            return;
        const std::uint64_t base = CallFn(name("class_enum_basetype").c_str(), klass).value_or(0);
        if (base)
            field.EnumCode = static_cast<int>(CallFn(name("type_get_type").c_str(), base).value_or(0) & 0xFFu);
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
        // address itself first, then step back a little (pointer aligned) for the
        // enclosing object's header. The runtime fallback is allowed only for the
        // exact address; the back scan keeps to the enumerated classes so a stray
        // field value cannot be mistaken for an object.
        constexpr std::uint64_t kMaxBack = 0x1000;
        const std::uint64_t step = static_cast<std::uint64_t>(m_process->PointerSize());
        const std::uint64_t aligned = address & ~(step - 1);
        for (std::uint64_t back = 0; back <= kMaxBack; back += step)
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
            // .NET: this overload has no code yet, another one may. Mono: a
            // failed compile means the worker is in trouble, so stop asking.
            if (!address && m_runtime.load() == ManagedRuntime::Clr)
                continue;
            if (!address)
                return std::nullopt;
            method.Address = *address;
            return method.Address;
        }
        // "Class:Field" names a static field where the runtime reports them.
        if (m_backend)
            return m_backend->StaticAddress(index, methodName);
        return std::nullopt;
    }

    Result<ManagedHeap> MonoBridge::ReadHeap()
    {
        RuntimeBackend* backend = nullptr;
        {
            std::lock_guard lock(m_lock);
            if (m_present)
                backend = m_backend.get();
        }
        if (!backend)
            return Status::Fail(ErrorCode::MonoNotFound);
        return backend->ReadHeap();
    }

    Result<ManagedInstances> MonoBridge::Instances(std::uint64_t klass, std::uint32_t limit)
    {
        RuntimeBackend* backend = nullptr;
        {
            std::lock_guard lock(m_lock);
            if (m_present)
                backend = m_backend.get();
        }
        if (!backend)
            return Status::Fail(ErrorCode::MonoNotFound);
        return backend->Instances(klass, limit);
    }

    std::vector<ManagedModule> MonoBridge::Modules()
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_backend)
            return {};
        return m_backend->Modules();
    }

    std::optional<ManagedObject> MonoBridge::ObjectAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_backend || !address)
            return std::nullopt;
        return m_backend->ObjectAt(address);
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
