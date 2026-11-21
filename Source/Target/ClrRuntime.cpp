#include "Target/ClrRuntime.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>

namespace Manifold::Target
{
    namespace P = ClrProtocol;

    namespace
    {
        constexpr unsigned kListMs = 60000;   // one module's types; a big assembly takes a while
        constexpr unsigned kQueryMs = 5000;   // one type's members
        constexpr unsigned kLookupMs = 1500;  // one address, asked while drawing
        constexpr unsigned kHeapMs = 120000;  // one walk over the whole GC heap
        constexpr std::size_t kMaxMisses = 8192;
        constexpr auto kMissLife = std::chrono::seconds(3);
    }

    std::optional<std::vector<std::uint8_t>> ClrRuntime::Ask(P::Op op, std::uint64_t argument, unsigned timeoutMs, std::string* error, const std::uint32_t* extra)
    {
        P::Writer request;
        request.U8(static_cast<std::uint8_t>(op));
        request.U64(argument);
        if (extra)
            request.U32(*extra);
        std::string reason;
        auto reply = m_link.Call(request, timeoutMs, reason);
        if (!reply && error)
            *error = reason;
        return reply;
    }

    Status ClrRuntime::Attach(const std::string& module)
    {
        if (Status started = m_link.Start(*m_bridge.m_process); !started)
            return started;
        m_bridge.m_module = module;
        m_bridge.m_runtime = ManagedRuntime::Clr;
        const ClrRuntimeInfo& info = m_link.Info();
        LOG_INFO("Mono", "Reading " + info.Flavour + " " + info.Version + " from outside the game with " + info.DacPath);
        return Status::Success();
    }

    void ClrRuntime::Detach()
    {
        m_link.Stop();
        m_parents.clear();
        m_notTypes.clear();
        m_code.clear();
        m_misses.clear();
        m_images.clear();
    }

    std::string ClrRuntime::Description() const
    {
        const ClrRuntimeInfo& info = m_link.Info();
        if (info.Flavour.empty())
            return ".NET";
        return info.Version.empty() ? info.Flavour : info.Flavour + " " + info.Version;
    }

    Status ClrRuntime::Enumerate(WorkProgress* progress, bool starting)
    {
        const auto stopped = [progress] { return progress && progress->Cancelled(); };
        const Status cancelled = Status::Fail(ErrorCode::MonoNotFound, "cancelled");
        if (progress)
            progress->SetText("Reading the assemblies");
        std::string error;
        Ask(P::Op::Flush, 0, kQueryMs);
        const auto modulesReply = Ask(P::Op::Modules, 0, kListMs, &error);
        if (!modulesReply)
            return Status::Fail(ErrorCode::ClrHelperFailed, error);
        struct ModuleInfo
        {
            std::uint64_t Address = 0;
            std::string Name;
            std::uint64_t Base = 0;
            std::uint32_t Size = 0;
        };
        std::vector<ModuleInfo> modules;
        {
            P::Reader in(*modulesReply);
            const std::uint32_t count = in.Count(30);
            for (std::uint32_t i = 0; i < count && in.Ok(); ++i)
            {
                ModuleInfo info;
                info.Address = in.U64();
                info.Name = in.Text();
                in.Text();  // path
                info.Base = in.U64();
                info.Size = in.U32();
                in.U8();    // dynamic
                modules.push_back(std::move(info));
            }
            if (!in.Ok())
                return Status::Fail(ErrorCode::ClrHelperFailed, "its module list was cut short");
        }
        if (modules.empty())
            return Status::Fail(ErrorCode::MonoNotFound, "the .NET runtime lists no assemblies yet (try again once the game has loaded)");
        if (progress)
        {
            progress->SetSteps(0, static_cast<std::int64_t>(modules.size()));
            progress->SetText("Reading the classes");
        }

        std::vector<MonoClass> classes;
        std::vector<std::uint64_t> parents;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> images;
        for (const ModuleInfo& module : modules)
        {
            if (stopped())
                return cancelled;
            if (module.Base && module.Size)
                images.emplace_back(module.Base, module.Base + module.Size);
            const auto reply = Ask(P::Op::Types, module.Address, kListMs, &error);
            if (progress)
                progress->Step();
            if (!reply)
            {
                if (!m_link.Running())
                    return Status::Fail(ErrorCode::ClrHelperFailed, error);
                LOG_WARN("Mono", "Could not read the types of " + module.Name + ": " + error);
                continue;
            }
            P::Reader in(*reply);
            const std::uint32_t count = in.Count(32);
            for (std::uint32_t i = 0; i < count && in.Ok(); ++i)
            {
                MonoClass info;
                info.Handle = in.U64();
                in.U32();  // token
                info.Namespace = in.Text();
                info.Name = in.Text();
                const std::uint64_t parent = in.U64();
                const std::uint32_t flags = in.U32();
                in.U32();  // base size
                info.Image = module.Name;
                info.Generic = (flags & P::kTypeGeneric) != 0;
                classes.push_back(std::move(info));
                parents.push_back(parent);
            }
            if (!in.Ok())
                LOG_WARN("Mono", "The type list of " + module.Name + " was cut short");
        }
        if (stopped())
            return cancelled;
        std::sort(images.begin(), images.end());

        std::map<std::string, int> classIndex;
        std::map<std::uint64_t, int> handleIndex;
        std::uint64_t handleMin = 0, handleMax = 0;
        for (int index = 0; index < static_cast<int>(classes.size()); ++index)
        {
            const MonoClass& info = classes[static_cast<std::size_t>(index)];
            classIndex.emplace(info.Name, index);
            if (!info.Namespace.empty())
                classIndex.emplace(info.FullName(), index);
            handleIndex.emplace(info.Handle, index);
            if (!handleMin || info.Handle < handleMin)
                handleMin = info.Handle;
            handleMax = std::max(handleMax, info.Handle);
        }
        const std::size_t count = classes.size();
        {
            std::lock_guard lock(m_bridge.m_lock);
            m_bridge.m_classes = std::move(classes);
            m_bridge.m_classIndex = std::move(classIndex);
            m_bridge.m_handleIndex = std::move(handleIndex);
            m_bridge.m_handleMin = handleMin;
            m_bridge.m_handleMax = handleMax;
            m_bridge.m_images.clear();
            m_parents = std::move(parents);
            m_images = std::move(images);
            m_notTypes.clear();
            m_misses.clear();
            if (starting)
                m_bridge.m_present = true;
            ++m_bridge.m_generation;
        }
        LOG_INFO("Mono", Format("Read %zu classes from %zu .NET assemblies", count, modules.size()));
        return Status::Success();
    }

    void ClrRuntime::EnsureMembers(int classIndex)
    {
        MonoClass& info = m_bridge.m_classes[static_cast<std::size_t>(classIndex)];
        if (info.MembersLoaded)
            return;
        std::string error;
        const auto reply = Ask(P::Op::Members, info.Handle, kQueryMs, &error);
        if (!reply)
        {
            // Asked again next time, unless the helper is gone.
            if (!m_link.Running())
                info.MembersLoaded = true;
            LOG_WARN("Mono", "Could not read the members of " + info.FullName() + ": " + error);
            return;
        }
        info.MembersLoaded = true;
        info.ValueTypesRead = true;
        P::Reader in(*reply);
        const std::uint64_t parent = in.U64();
        info.Parent = in.Text();
        in.U32();  // type flags
        if (static_cast<std::size_t>(classIndex) < m_parents.size())
            m_parents[static_cast<std::size_t>(classIndex)] = parent;
        const std::uint32_t fields = in.Count(40);
        std::vector<MonoField> instance;
        std::vector<MonoField> statics;
        for (std::uint32_t i = 0; i < fields && in.Ok(); ++i)
        {
            MonoField field;
            field.Name = in.Text();
            in.U32();  // token
            const std::uint32_t flags = in.U32();
            field.Offset = in.I32();
            field.TypeCode = static_cast<int>(in.U32());
            field.EnumCode = static_cast<int>(in.U32());
            const std::uint64_t typeMt = in.U64();
            field.TypeName = in.Text();
            field.Address = in.U64();
            const std::int32_t valueSize = in.I32();
            if (field.TypeCode == 0x11 && field.EnumCode == 0)
            {
                field.TypeHandle = typeMt;
                field.ValueSize = valueSize;
            }
            if (flags & P::kFieldStatic)
                statics.push_back(std::move(field));
            else
                instance.push_back(std::move(field));
        }
        // A struct's size from its method table includes padding up to the
        // pointer size; the next field's offset bounds it exactly.
        std::stable_sort(instance.begin(), instance.end(), [](const MonoField& a, const MonoField& b) { return a.Offset < b.Offset; });
        for (std::size_t i = 0; i + 1 < instance.size(); ++i)
        {
            if (instance[i].ValueSize > 0)
                instance[i].ValueSize = std::min(instance[i].ValueSize, std::max(1, instance[i + 1].Offset - instance[i].Offset));
        }
        info.Fields = std::move(instance);
        info.Statics = std::move(statics);
        const std::uint32_t methods = in.Count(32);
        for (std::uint32_t i = 0; i < methods && in.Ok(); ++i)
        {
            MonoMethod method;
            method.Name = in.Text();
            in.U32();  // token
            in.U32();  // flags
            method.ParamCount = in.I32();
            method.Handle = in.U64();
            method.Address = in.U64();
            info.Methods.push_back(std::move(method));
        }
        if (!in.Ok())
            LOG_WARN("Mono", "The members of " + info.FullName() + " were cut short");
    }

    std::optional<int> ClrRuntime::ParentIndex(int classIndex)
    {
        if (classIndex < 0 || static_cast<std::size_t>(classIndex) >= m_bridge.m_classes.size())
            return -1;
        if (!m_bridge.m_classes[static_cast<std::size_t>(classIndex)].MembersLoaded)
            EnsureMembers(classIndex);
        const std::uint64_t parent = static_cast<std::size_t>(classIndex) < m_parents.size() ? m_parents[static_cast<std::size_t>(classIndex)] : 0;
        if (!parent)
            return -1;
        if (const auto known = m_bridge.m_handleIndex.find(parent); known != m_bridge.m_handleIndex.end())
            return known->second;
        return AddDescribed(parent);
    }

    std::optional<std::uint64_t> ClrRuntime::StaticAddress(int classIndex, std::string_view field)
    {
        if (classIndex < 0 || static_cast<std::size_t>(classIndex) >= m_bridge.m_classes.size())
            return std::nullopt;
        EnsureMembers(classIndex);
        for (const MonoField& candidate : m_bridge.m_classes[static_cast<std::size_t>(classIndex)].Statics)
        {
            if (candidate.Name == field && candidate.Address)
                return candidate.Address;
        }
        return std::nullopt;
    }

    std::uint64_t ClrRuntime::ManagedClass(std::string_view space, std::string_view name)
    {
        const std::string full = space.empty() ? std::string(name) : std::string(space) + "." + std::string(name);
        const int index = m_bridge.FindClass(full);
        return index < 0 ? 0 : m_bridge.m_classes[static_cast<std::size_t>(index)].Handle;
    }

    int ClrRuntime::AddDescribed(std::uint64_t mt)
    {
        if (!mt || m_notTypes.count(mt))
            return -1;
        const auto reply = Ask(P::Op::DescribeType, mt, kLookupMs);
        if (!reply)
            return -1;
        P::Reader in(*reply);
        if (in.U8() == 0)
        {
            if (m_notTypes.size() > 65536)
                m_notTypes.clear();
            m_notTypes.insert(mt);
            return -1;
        }
        MonoClass info;
        info.Handle = mt;
        info.Namespace = in.Text();
        info.Name = in.Text();
        in.U64();  // module
        const std::uint64_t parent = in.U64();
        const std::uint32_t flags = in.U32();
        if (!in.Ok() || info.Name.empty())
            return -1;
        info.Discovered = true;
        info.Generic = (flags & P::kTypeGeneric) != 0;
        const int index = static_cast<int>(m_bridge.m_classes.size());
        m_bridge.m_classIndex.emplace(info.Name, index);
        if (!info.Namespace.empty())
            m_bridge.m_classIndex.emplace(info.FullName(), index);
        m_bridge.m_handleIndex.emplace(mt, index);
        m_bridge.m_classes.push_back(std::move(info));
        if (m_parents.size() < m_bridge.m_classes.size())
            m_parents.resize(m_bridge.m_classes.size(), 0);
        m_parents[static_cast<std::size_t>(index)] = parent;
        return index;
    }

    std::optional<std::string> ClrRuntime::ResolveHead(std::uint64_t header, bool allowDescribe)
    {
        // A .NET object starts with its MethodTable pointer. Only compared
        // against the known ones, or (for the exact address asked about)
        // checked by the runtime's data access library, which validates it,
        // so a stray value never passes as a type.
        const auto mt = m_bridge.m_process->ReadPointer(header);
        if (!mt || *mt < 0x10000)
            return std::nullopt;
        if (const auto found = m_bridge.m_handleIndex.find(*mt); found != m_bridge.m_handleIndex.end())
            return m_bridge.m_classes[static_cast<std::size_t>(found->second)].FullName();
        if (!allowDescribe)
            return std::nullopt;
        const int index = AddDescribed(*mt);
        if (index < 0)
            return std::nullopt;
        return m_bridge.m_classes[static_cast<std::size_t>(index)].FullName();
    }

    bool ClrRuntime::InManagedImage(std::uint64_t address) const
    {
        auto next = std::upper_bound(m_images.begin(), m_images.end(), std::make_pair(address, ~0ull));
        if (next == m_images.begin())
            return false;
        --next;
        return address >= next->first && address < next->second;
    }

    const std::pair<const std::uint64_t, ClrRuntime::CodeEntry>* ClrRuntime::FindCode(std::uint64_t address)
    {
        auto it = m_code.upper_bound(address);
        if (it == m_code.begin())
            return nullptr;
        --it;
        return address < it->second.End ? &*it : nullptr;
    }

    bool ClrRuntime::AskCode(std::uint64_t address)
    {
        if (FindCode(address))
            return true;
        // Code in a native module (the runtime itself, the system) is never
        // managed; only the JIT's own pages and precompiled assemblies are.
        if (const auto module = m_bridge.m_process->ModuleAt(address); module && !InManagedImage(address))
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (const auto missed = m_misses.find(address); missed != m_misses.end())
        {
            if (now - missed->second < kMissLife)
                return false;
            m_misses.erase(missed);
        }
        const auto reply = Ask(P::Op::MethodAt, address, kLookupMs);
        bool found = false;
        if (reply)
        {
            P::Reader in(*reply);
            if (in.U8() != 0)
            {
                CodeEntry entry;
                entry.Name = in.Text();
                const std::uint64_t start = in.U64();
                const std::uint32_t size = in.U32();
                if (in.Ok() && size && address >= start && address < start + size)
                {
                    entry.End = start + size;
                    m_code[start] = std::move(entry);
                    found = true;
                }
            }
        }
        if (!found)
        {
            if (m_misses.size() >= kMaxMisses)
                m_misses.clear();
            m_misses[address] = now;
        }
        return found;
    }

    std::optional<std::string> ClrRuntime::MethodNameAt(std::uint64_t address)
    {
        if (!AskCode(address))
            return std::nullopt;
        return FindCode(address)->second.Name;
    }

    std::optional<std::string> ClrRuntime::MethodStartNameAt(std::uint64_t address)
    {
        if (!AskCode(address))
            return std::nullopt;
        const auto* entry = FindCode(address);
        if (entry->first != address)
            return std::nullopt;
        return entry->second.Name;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> ClrRuntime::MethodRange(std::uint64_t address)
    {
        if (!AskCode(address))
            return std::nullopt;
        const auto* entry = FindCode(address);
        return std::make_pair(entry->first, entry->second.End);
    }

    std::size_t ClrRuntime::MethodsSharingCode(std::uint64_t address)
    {
        const auto* entry = FindCode(address);
        return entry && entry->first == address ? 1 : 0;
    }

    Result<ManagedHeap> ClrRuntime::ReadHeap()
    {
        const auto began = std::chrono::steady_clock::now();
        std::string error;
        const auto reply = Ask(P::Op::HeapStats, 0, kHeapMs, &error);
        if (!reply)
            return Status::Fail(ErrorCode::ClrHelperFailed, error);
        P::Reader in(*reply);
        ManagedHeap heap;
        heap.Objects = in.U64();
        heap.Bytes = in.U64();
        heap.Segments = in.U32();
        heap.Skipped = in.U64();
        const std::uint32_t count = in.Count(28);
        heap.Types.reserve(count);
        for (std::uint32_t i = 0; i < count && in.Ok(); ++i)
        {
            ManagedHeapType type;
            type.Handle = in.U64();
            type.Name = in.Text();
            type.Count = in.U64();
            type.Bytes = in.U64();
            heap.Types.push_back(std::move(type));
        }
        if (!in.Ok())
            return Status::Fail(ErrorCode::ClrHelperFailed, "its heap summary was cut short");
        std::sort(heap.Types.begin(), heap.Types.end(), [](const ManagedHeapType& a, const ManagedHeapType& b)
            { return a.Bytes != b.Bytes ? a.Bytes > b.Bytes : a.Name < b.Name; });
        heap.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        return heap;
    }

    Result<ManagedInstances> ClrRuntime::Instances(std::uint64_t klass, std::uint32_t limit)
    {
        std::string error;
        const auto reply = Ask(P::Op::Instances, klass, kHeapMs, &error, &limit);
        if (!reply)
            return Status::Fail(ErrorCode::ClrHelperFailed, error);
        P::Reader in(*reply);
        ManagedInstances found;
        found.Total = in.U64();
        const std::uint32_t count = in.Count(8);
        found.Addresses.reserve(count);
        for (std::uint32_t i = 0; i < count && in.Ok(); ++i)
            found.Addresses.push_back(in.U64());
        if (!in.Ok())
            return Status::Fail(ErrorCode::ClrHelperFailed, "its list of objects was cut short");
        return found;
    }

    std::optional<ManagedObject> ClrRuntime::ObjectAt(std::uint64_t address)
    {
        const auto reply = Ask(P::Op::ObjectInfo, address, kLookupMs);
        if (!reply)
            return std::nullopt;
        P::Reader in(*reply);
        if (in.U8() == 0)
            return std::nullopt;
        ManagedObject object;
        object.Address = address;
        object.Handle = in.U64();
        object.Name = in.Text();
        const std::uint32_t kind = in.U32();
        // ∑ The runtime reports an ordinary object as OBJ_OTHER; OBJ_OBJECT is
        // only System.Object itself. Both are plain objects here.
        object.Kind = kind == static_cast<std::uint32_t>(ManagedObjectKind::Other) ? ManagedObjectKind::Object
            : kind < static_cast<std::uint32_t>(ManagedObjectKind::Other)      ? static_cast<ManagedObjectKind>(kind)
                                                                               : ManagedObjectKind::Object;
        object.Size = in.U64();
        object.ElementHandle = in.U64();
        object.ElementName = in.Text();
        object.ElementType = static_cast<int>(in.U32());
        object.Rank = static_cast<int>(in.U32());
        object.Length = in.U64();
        object.ElementSize = in.U64();
        object.Data = in.U64();
        if (!in.Ok())
            return std::nullopt;
        return object;
    }

    std::optional<std::uint64_t> ClrRuntime::Compile(std::uint64_t method)
    {
        if (!method)
            return std::nullopt;
        const auto reply = Ask(P::Op::MethodCode, method, kLookupMs);
        if (!reply)
            return std::nullopt;
        P::Reader in(*reply);
        const std::uint64_t code = in.U64();
        if (!in.Ok() || !code)
            return std::nullopt;
        return code;
    }
}
