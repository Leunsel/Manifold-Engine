#include "Target/ClrRuntime.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <iterator>
#include <memory>

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
        constexpr auto kMembersRetry = std::chrono::seconds(2);   // members that could not be read
        constexpr auto kStaticsRetry = std::chrono::seconds(5);   // statics without storage yet
        constexpr auto kRestartFirst = std::chrono::seconds(10);  // at most one restart per this
        constexpr auto kRestartMost = std::chrono::seconds(160);  // the longest pause after failed ones
    }

    const ClrCodeRanges::Entry* ClrCodeRanges::Find(std::uint64_t address) const
    {
        auto it = m_ranges.upper_bound(address);
        if (it == m_ranges.begin())
            return nullptr;
        --it;
        return address < it->second.End ? &it->second : nullptr;
    }

    const ClrCodeRanges::Entry* ClrCodeRanges::Add(std::uint64_t start, std::uint64_t end, std::string name)
    {
        if (end <= start)
            return nullptr;
        // Every older range that overlaps the new one goes: the one starting
        // before it and reaching into it, and all that start inside it.
        auto it = m_ranges.lower_bound(start);
        if (it != m_ranges.begin())
        {
            if (const auto before = std::prev(it); before->second.End > start)
                it = before;
        }
        while (it != m_ranges.end() && it->first < end)
            it = m_ranges.erase(it);
        Entry& entry = m_ranges[start];
        entry.Start = start;
        entry.End = end;
        entry.Name = std::move(name);
        return &entry;
    }

    std::optional<std::vector<std::uint8_t>> ClrRuntime::Ask(P::Op op, std::uint64_t argument, unsigned timeoutMs, std::string* error, const std::uint32_t* extra,
        bool* busy)
    {
        // A helper that ended is started again by the next request that may
        // take a moment; lookups asked while drawing never wait for that.
        if (timeoutMs > kLookupMs && !m_link.Running())
            Restart();
        P::Writer request;
        request.U8(static_cast<std::uint8_t>(op));
        request.U64(argument);
        if (extra)
            request.U32(*extra);
        std::string reason;
        bool wasBusy = false;
        auto reply = m_link.Call(request, timeoutMs, reason, &wasBusy);
        if (busy)
            *busy = wasBusy;
        if (!reply && error)
        {
            *error = reason;
            if (!wasBusy && m_attached.load() && !m_link.Running())
                *error += " (it is started again by a later request, at most every 10 s)";
        }
        return reply;
    }

    bool ClrRuntime::Restart()
    {
        // One thread restarts; the others go on without the helper.
        std::unique_lock guard(m_restartLock, std::try_to_lock);
        if (!guard.owns_lock() || !m_attached.load())
            return false;
        if (m_link.Running())
            return true;
        const auto now = Clock::now();
        if (now < m_nextRestart)
            return false;
        // Detach waits for m_restartLock before the bridge lets go of the
        // process, so it is still there.
        const std::shared_ptr<Process> process = m_bridge.m_process;
        if (!process)
            return false;
        LOG_INFO("Mono", "The .NET helper has ended; starting it again");
        const Status started = m_link.Start(*process);
        if (started.Ok())
        {
            m_restartDelay = kRestartFirst;
            m_nextRestart = now + kRestartFirst;
            LOG_INFO("Mono", "The .NET helper runs again");
            return true;
        }
        m_nextRestart = now + m_restartDelay;
        LOG_WARN("Mono", Format("Could not start the .NET helper again (%s); the next try is in %lld s", started.Text.c_str(),
            static_cast<long long>(m_restartDelay.count())));
        m_restartDelay = std::min<std::chrono::seconds>(m_restartDelay * 2, kRestartMost);
        return false;
    }

    Status ClrRuntime::Attach(const std::string& module)
    {
        if (Status started = m_link.Start(*m_bridge.m_process); !started)
            return started;
        {
            std::lock_guard guard(m_restartLock);
            m_attached.store(true);
            m_nextRestart = {};
            m_restartDelay = kRestartFirst;
        }
        m_bridge.m_module = module;
        m_bridge.m_runtime = ManagedRuntime::Clr;
        const ClrRuntimeInfo& info = m_link.Info();
        LOG_INFO("Mono", "Reading " + info.Flavour + " " + info.Version + " from outside the game with " + info.DacPath);
        return Status::Success();
    }

    void ClrRuntime::Detach()
    {
        // No restart from here on; one under way finishes first.
        {
            std::lock_guard guard(m_restartLock);
            m_attached.store(false);
        }
        m_link.Stop();
        m_parents.clear();
        m_notTypes.clear();
        m_code.Clear();
        m_membersRetry.clear();
        m_staticsRead.clear();
        m_misses.clear();
        m_images.clear();
        m_modules.clear();
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
        std::vector<ManagedModule> modules;
        {
            P::Reader in(*modulesReply);
            const std::uint32_t count = in.Count(30);
            for (std::uint32_t i = 0; i < count && in.Ok(); ++i)
            {
                ManagedModule info;
                info.Address = in.U64();
                info.Name = in.Text();
                info.Path = in.Text();
                info.Base = in.U64();
                info.Size = in.U32();
                info.Dynamic = in.U8() != 0;
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
        for (const ManagedModule& module : modules)
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
                info.Token = in.U32();
                info.Module = module.Address;
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
        const std::size_t moduleCount = modules.size();
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
            m_modules = modules;
            m_notTypes.clear();
            m_misses.clear();
            // Code may have been thrown away and compiled anew since, and the
            // class indexes start over.
            m_code.Clear();
            m_membersRetry.clear();
            m_staticsRead.clear();
            if (starting)
                m_bridge.m_present = true;
            ++m_bridge.m_generation;
        }
        LOG_INFO("Mono", Format("Read %zu classes from %zu .NET assemblies", count, moduleCount));
        return Status::Success();
    }

    void ClrRuntime::EnsureMembers(int classIndex)
    {
        if (m_bridge.m_classes[static_cast<std::size_t>(classIndex)].MembersLoaded)
            return;
        if (const auto retry = m_membersRetry.find(classIndex); retry != m_membersRetry.end())
        {
            if (Clock::now() < retry->second)
                return;
            m_membersRetry.erase(retry);
        }
        ReadMembers(classIndex, false);
    }

    bool ClrRuntime::ReadMembers(int classIndex, bool staticsOnly)
    {
        const std::uint64_t handle = m_bridge.m_classes[static_cast<std::size_t>(classIndex)].Handle;
        std::string error;
        bool busy = false;
        const auto reply = Ask(P::Op::Members, handle, kQueryMs, &error, nullptr, &busy);
        MonoClass& info = m_bridge.m_classes[static_cast<std::size_t>(classIndex)];
        if (!reply)
        {
            // ∑ Never marked as read: a class marked without its members
            // would stay empty after the helper is back (busy, restarted).
            // Asked again after a pause rather than on every frame.
            if (!staticsOnly)
                m_membersRetry[classIndex] = Clock::now() + kMembersRetry;
            if (!busy)
                LOG_WARN("Mono", "Could not read the members of " + info.FullName() + ": " + error);
            return false;
        }
        P::Reader in(*reply);
        const std::uint64_t parent = in.U64();
        std::string parentName = in.Text();
        in.U32();  // type flags
        const std::uint32_t fields = in.Count(40);
        std::vector<MonoField> instance;
        std::vector<MonoField> statics;
        bool waitingStatics = false;
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
            {
                // Thread statics never have one address; other statics get
                // theirs once the class is initialized.
                if (!field.Address && !(flags & P::kFieldThreadStatic))
                    waitingStatics = true;
                statics.push_back(std::move(field));
            }
            else
                instance.push_back(std::move(field));
        }
        if (waitingStatics)
            m_staticsRead[classIndex] = Clock::now();
        else
            m_staticsRead.erase(classIndex);
        if (staticsOnly)
        {
            if (in.Ok())
                info.Statics = std::move(statics);
            return in.Ok();
        }
        info.MembersLoaded = true;
        info.ValueTypesRead = true;
        info.Parent = std::move(parentName);
        if (static_cast<std::size_t>(classIndex) < m_parents.size())
            m_parents[static_cast<std::size_t>(classIndex)] = parent;
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
            method.Token = in.U32();
            method.Static = (in.U32() & P::kMethodStatic) != 0;
            method.ParamCount = in.I32();
            method.Handle = in.U64();
            method.Address = in.U64();
            info.Methods.push_back(std::move(method));
        }
        if (!in.Ok())
            LOG_WARN("Mono", "The members of " + info.FullName() + " were cut short");
        return true;
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
        // 0 with named set: the field is there but has no storage yet.
        const auto find = [&](bool& named) -> std::uint64_t
            {
                named = false;
                for (const MonoField& candidate : m_bridge.m_classes[static_cast<std::size_t>(classIndex)].Statics)
                {
                    if (candidate.Name != field)
                        continue;
                    named = true;
                    if (candidate.Address)
                        return candidate.Address;
                }
                return 0;
            };
        bool named = false;
        std::uint64_t address = find(named);
        // A static without storage gets it when the class is initialized,
        // which may be after its members were read: its class is asked
        // again, every few seconds at most.
        if (!address && named)
        {
            const auto read = m_staticsRead.find(classIndex);
            if (read != m_staticsRead.end() && Clock::now() - read->second >= kStaticsRetry)
            {
                read->second = Clock::now();
                ReadMembers(classIndex, true);
                address = find(named);
            }
        }
        if (!address)
            return std::nullopt;
        return address;
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
        info.Module = in.U64();
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

    const ClrCodeRanges::Entry* ClrRuntime::AskCode(std::uint64_t address)
    {
        if (const ClrCodeRanges::Entry* known = m_code.Find(address))
            return known;
        // Code in a native module (the runtime itself, the system) is never
        // managed; only the JIT's own pages and precompiled assemblies are.
        if (const auto module = m_bridge.m_process->ModuleAt(address); module && !InManagedImage(address))
            return nullptr;
        const auto now = std::chrono::steady_clock::now();
        if (const auto missed = m_misses.find(address); missed != m_misses.end())
        {
            if (now - missed->second < kMissLife)
                return nullptr;
            m_misses.erase(missed);
        }
        bool busy = false;
        const auto reply = Ask(P::Op::MethodAt, address, kLookupMs, nullptr, nullptr, &busy);
        const ClrCodeRanges::Entry* found = nullptr;
        if (reply)
        {
            P::Reader in(*reply);
            if (in.U8() != 0)
            {
                std::string name = in.Text();
                const std::uint64_t start = in.U64();
                const std::uint32_t size = in.U32();
                if (in.Ok() && size && address >= start && address < start + size)
                    found = m_code.Add(start, start + size, std::move(name));
            }
        }
        // A busy helper said nothing about the address.
        if (!found && !busy)
        {
            if (m_misses.size() >= kMaxMisses)
                m_misses.clear();
            m_misses[address] = now;
        }
        return found;
    }

    std::optional<std::string> ClrRuntime::MethodNameAt(std::uint64_t address)
    {
        const ClrCodeRanges::Entry* entry = AskCode(address);
        if (!entry)
            return std::nullopt;
        return entry->Name;
    }

    std::optional<std::string> ClrRuntime::MethodStartNameAt(std::uint64_t address)
    {
        const ClrCodeRanges::Entry* entry = AskCode(address);
        if (!entry || entry->Start != address)
            return std::nullopt;
        return entry->Name;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> ClrRuntime::MethodRange(std::uint64_t address)
    {
        const ClrCodeRanges::Entry* entry = AskCode(address);
        if (!entry)
            return std::nullopt;
        return std::make_pair(entry->Start, entry->End);
    }

    std::size_t ClrRuntime::MethodsSharingCode(std::uint64_t address)
    {
        const ClrCodeRanges::Entry* entry = m_code.Find(address);
        return entry && entry->Start == address ? 1 : 0;
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
