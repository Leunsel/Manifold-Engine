#include "Target/UnityExplorer.h"

#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/ManagedAgent.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/UnityExplorerIl2Cpp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <windows.h>

namespace Manifold::Target
{
    using namespace ExplorerLimits;

    namespace
    {
        constexpr std::size_t kArgsSize = 0x10000;
        constexpr const char* kGoneText = "the object is gone (the game destroyed it, e.g. on a scene change), so nothing was called on it";
        constexpr const char* kUnheldText = "Manifold does not hold this object; open it from its parent (the scene tree, a field or a property) first";
        constexpr auto kHeartbeatEvery = std::chrono::seconds(2);
        constexpr int kMaxMembers = 4096;
        constexpr std::uint32_t kFieldStatic = 0x10;
        constexpr std::uint32_t kFieldLiteral = 0x40;
        constexpr std::uint32_t kMethodStatic = 0x10;

        // Set on the explorer's job thread, the only thread that may make
        // remote calls for names and layouts; drawing code only reads caches.
        thread_local bool t_jobThread = false;

        int PrimitiveSize(int code, int pointer)
        {
            switch (code)
            {
            case MonoTypeCode::Boolean: case MonoTypeCode::I1: case MonoTypeCode::U1: return 1;
            case MonoTypeCode::Char: case MonoTypeCode::I2: case MonoTypeCode::U2: return 2;
            case MonoTypeCode::I4: case MonoTypeCode::U4: case MonoTypeCode::R4: return 4;
            case MonoTypeCode::I8: case MonoTypeCode::U8: case MonoTypeCode::R8: return 8;
            case MonoTypeCode::I: case MonoTypeCode::U: case MonoTypeCode::Ptr: case MonoTypeCode::FnPtr: return pointer;
            default: return 0;
            }
        }

        bool IsPrimitive(int code)
        {
            return PrimitiveSize(code, 8) > 0;
        }

        std::uint64_t LoadPointer(const std::uint8_t* bytes, std::size_t pointer)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes, std::min<std::size_t>(pointer, sizeof(value)));
            return value;
        }

        template <typename T>
        T Load(const std::uint8_t* bytes)
        {
            T value{};
            std::memcpy(&value, bytes, sizeof(T));
            return value;
        }

        std::string FormatFloat(double value, int digits)
        {
            if (std::isnan(value))
                return "NaN";
            if (std::isinf(value))
                return value < 0 ? "-Infinity" : "Infinity";
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*g", digits, value);
            std::string text = buffer;
            if (text.find_first_of(".eEn") == std::string::npos)
                text += ".0";
            return text;
        }

        std::optional<std::int64_t> ParseInteger(std::string_view text)
        {
            const std::string trimmed = Trim(text);
            if (trimmed.empty())
                return std::nullopt;
            const bool negative = trimmed[0] == '-';
            const std::string_view body = std::string_view(trimmed).substr(negative ? 1 : 0);
            if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
            {
                const auto hex = ParseHex(body.substr(2));
                if (!hex)
                    return std::nullopt;
                const std::int64_t value = static_cast<std::int64_t>(*hex);
                return negative ? -value : value;
            }
            return ParseDecimal(trimmed);
        }

        void Put(std::vector<std::uint8_t>& out, std::size_t at, const void* value, std::size_t size)
        {
            if (at + size <= out.size())
                std::memcpy(out.data() + at, value, size);
        }

        bool ParsePrimitive(int code, int pointer, const std::string& text, std::uint8_t* out, std::string* error)
        {
            const auto fail = [&](const char* why) { if (error) *error = why; return false; };
            const std::string value = Trim(text);
            switch (code)
            {
            case MonoTypeCode::Boolean:
            {
                const std::string lower = Lower(value);
                if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
                    out[0] = 1;
                else if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
                    out[0] = 0;
                else
                    return fail("expected true or false");
                return true;
            }
            case MonoTypeCode::Char:
            {
                const std::wstring wide = Utf8ToWide(value);
                if (wide.size() != 1)
                    return fail("expected one character");
                const std::uint16_t unit = static_cast<std::uint16_t>(wide[0]);
                std::memcpy(out, &unit, 2);
                return true;
            }
            case MonoTypeCode::R4:
            case MonoTypeCode::R8:
            {
                const auto number = ParseDouble(value);
                if (!number)
                    return fail("expected a number");
                if (code == MonoTypeCode::R4)
                {
                    const float f = static_cast<float>(*number);
                    std::memcpy(out, &f, 4);
                }
                else
                    std::memcpy(out, &*number, 8);
                return true;
            }
            default:
                break;
            }
            const int size = PrimitiveSize(code, pointer);
            if (!size)
                return fail("not a simple value");
            const auto number = ParseInteger(value);
            if (!number)
                return fail("expected a whole number (or 0x hex)");
            const bool isSigned = code == MonoTypeCode::I1 || code == MonoTypeCode::I2 || code == MonoTypeCode::I4 || code == MonoTypeCode::I8 || code == MonoTypeCode::I;
            if (size < 8)
            {
                const std::int64_t bits = size * 8;
                const std::int64_t low = isSigned ? -(std::int64_t(1) << (bits - 1)) : 0;
                const std::int64_t high = isSigned ? (std::int64_t(1) << (bits - 1)) - 1 : (std::int64_t(1) << bits) - 1;
                if (*number < low || *number > high)
                    return fail("out of range for this type");
            }
            std::memcpy(out, &*number, static_cast<std::size_t>(size));
            return true;
        }

        std::string ShortName(std::string_view name)
        {
            static const std::pair<const char*, const char*> kAliases[] = {
                { "System.Boolean", "bool" }, { "System.Char", "char" }, { "System.SByte", "sbyte" }, { "System.Byte", "byte" },
                { "System.Int16", "short" }, { "System.UInt16", "ushort" }, { "System.Int32", "int" }, { "System.UInt32", "uint" },
                { "System.Int64", "long" }, { "System.UInt64", "ulong" }, { "System.Single", "float" }, { "System.Double", "double" },
                { "System.String", "string" }, { "System.Object", "object" }, { "System.Void", "void" }, { "System.IntPtr", "IntPtr" },
                { "System.UIntPtr", "UIntPtr" },
            };
            std::string text(name);
            for (const auto& alias : kAliases)
                text = ReplaceAll(text, alias.first, alias.second);
            static const char* const kNamespaces[] = { "System.Collections.Generic.", "UnityEngine.SceneManagement.", "UnityEngine.", "System." };
            for (const char* space : kNamespaces)
                text = ReplaceAll(text, space, "");
            return text;
        }
    }

    UnityExplorer::UnityExplorer(MonoBridge& mono) : m_mono(mono) {}

    UnityExplorer::~UnityExplorer()
    {
        Shutdown();
    }

    std::shared_ptr<Process> UnityExplorer::Proc() const
    {
        std::lock_guard lock(m_stateLock);
        return m_process;
    }

    std::size_t UnityExplorer::PointerBytes() const
    {
        const auto process = Proc();
        return process ? static_cast<std::size_t>(process->PointerSize()) : 8;
    }

    // ---------------------------------------------------------------------
    // Jobs

    bool UnityExplorer::Allowed()
    {
        if (m_enabled.load())
            return true;
        {
            std::lock_guard state(m_stateLock);
            m_status.Error = "The scene explorer and inspector are switched off for this session.";
        }
        Publish();
        return false;
    }

    void UnityExplorer::Post(std::string task, std::function<void()> job)
    {
        if (!m_enabled.load())
            return;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_present || !m_mono.m_process || m_mono.m_runtime.load() == ManagedRuntime::None)
                return;
            std::lock_guard state(m_stateLock);
            m_process = m_mono.m_process;
        }
        std::lock_guard lock(m_jobLock);
        if (m_stopping)
            return;
        m_jobs.emplace_back(std::move(task), std::move(job));
        if (!m_thread.joinable())
            m_thread = std::thread([this]
                {
                    SetThreadName(L"Unity explorer");
                    Loop();
                });
        m_jobWake.notify_one();
    }

    void UnityExplorer::Loop()
    {
        t_jobThread = true;
        for (;;)
        {
            std::pair<std::string, std::function<void()>> job;
            {
                std::unique_lock lock(m_jobLock);
                while (!m_stopping && m_jobs.empty())
                {
                    if (m_jobWake.wait_for(lock, kHeartbeatEvery) == std::cv_status::timeout && m_agent && m_agent->Connected())
                    {
                        lock.unlock();
                        m_agent->Heartbeat();
                        lock.lock();
                    }
                }
                if (m_stopping)
                    return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                m_jobAbort = m_abort.load();
            }
            {
                std::lock_guard state(m_stateLock);
                m_status.Busy = true;
                m_status.Task = job.first;
                m_status.LastTask = job.first;
            }
            Publish();
            try
            {
                LearnClassShape();
                job.second();
            }
            catch (const std::exception& error)
            {
                Fail(std::string("internal error: ") + error.what());
            }
            bool idle = false;
            {
                std::lock_guard lock(m_jobLock);
                idle = m_jobs.empty();
            }
            {
                std::lock_guard state(m_stateLock);
                m_status.Busy = !idle;
                if (idle)
                    m_status.Task.clear();
            }
            Publish();
        }
    }

    void UnityExplorer::Publish()
    {
        std::lock_guard state(m_stateLock);
        const std::uint64_t generation = ++m_generation;
        m_status.Generation = generation;
    }

    void UnityExplorer::Fail(const std::string& text)
    {
        LOG_WARN("Unity", text);
        {
            std::lock_guard state(m_stateLock);
            m_status.Error = text;
        }
        Publish();
    }

    bool UnityExplorer::Aborted() const
    {
        std::lock_guard lock(m_jobLock);
        return m_stopping || m_abort.load() != m_jobAbort;
    }

    void UnityExplorer::Cancel()
    {
        {
            std::lock_guard lock(m_jobLock);
            m_jobs.clear();
        }
        ++m_abort;
    }

    void UnityExplorer::Shutdown()
    {
        {
            std::lock_guard lock(m_jobLock);
            m_stopping = true;
            m_jobs.clear();
        }
        ++m_abort;
        m_jobWake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_thread = std::thread();

        // The worker still runs here (the bridge stops it afterwards), so the
        // agent can be taken out of the frame callback; it sets
        // Application.runInBackground back and drops its slots on the game's
        // main thread.
        bool present = false;
        {
            std::lock_guard bridge(m_mono.m_lock);
            present = m_mono.m_present && m_mono.m_process && m_mono.m_process->Alive();
        }
        if (m_agent)
        {
            if (present)
                m_agent->Stop();
            else
                m_agent->Forget();
            m_agent.reset();
        }
        m_agentReady = false;
        Il2Shutdown(present);
        m_args = 0;
        m_argsUsed = 0;
        m_dontDestroyHandle = 0;
        m_classTokens.clear();
        {
            std::lock_guard state(m_stateLock);
            m_classShape = {};
            m_validClasses.clear();
            m_slotAddress.clear();
            m_addressSlot.clear();
        }
        m_types.clear();
        m_chains.clear();
        {
            std::lock_guard state(m_stateLock);
            m_process.reset();
            m_classNames.clear();
            m_status = {};
            m_tree = {};
            m_search = {};
            m_inspections.clear();
            m_layouts.clear();
            m_layoutQueued.clear();
        }
        {
            std::lock_guard lock(m_jobLock);
            m_stopping = false;
        }
        Publish();
    }

    UnityStatus UnityExplorer::State() const
    {
        std::lock_guard state(m_stateLock);
        return m_status;
    }

    UnityTree UnityExplorer::Tree() const
    {
        std::lock_guard state(m_stateLock);
        return m_tree;
    }

    UnitySearchResult UnityExplorer::SearchResult() const
    {
        std::lock_guard state(m_stateLock);
        return m_search;
    }

    std::optional<UnityInspection> UnityExplorer::Inspection(int id) const
    {
        std::lock_guard state(m_stateLock);
        const auto found = m_inspections.find(id);
        if (found == m_inspections.end())
            return std::nullopt;
        return found->second;
    }

    // ---------------------------------------------------------------------
    // Remote calls

    bool UnityExplorer::Il2() const
    {
        return m_mono.IsIl2Cpp();
    }

    std::optional<std::uint64_t> UnityExplorer::Worker(const char* function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        std::string translated;
        if (const std::string_view name(function); name.starts_with("mono_") && Il2())
        {
            translated = name == "mono_class_from_mono_type" ? "il2cpp_class_from_type"
                       : name == "mono_array_element_size" ? "il2cpp_class_array_element_size"
                       : "il2cpp_" + std::string(name.substr(5));
            function = translated.c_str();
        }
        std::lock_guard bridge(m_mono.m_lock);
        if (!m_mono.m_present)
            return std::nullopt;
        return m_mono.CallFn(function, a0, a1, a2, a3);
    }

    std::optional<std::uint64_t> UnityExplorer::Exec(const char* function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        if (!m_agent || !m_agent->Connected())
            return Worker(function, a0, a1, a2, a3);
        std::uint64_t fn = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_present)
                return std::nullopt;
            fn = m_mono.Fn(function);
        }
        if (!fn)
            return std::nullopt;
        AgentRequest request;
        request.Op(AgentProtocol::Native)
            .I64(static_cast<std::int64_t>(fn))
            .I64(static_cast<std::int64_t>(a0))
            .I64(static_cast<std::int64_t>(a1))
            .I64(static_cast<std::int64_t>(a2))
            .I64(static_cast<std::int64_t>(a3));
        const Result<AgentLink::Exchanged> answer = Ask(request);
        if (!answer || !answer->Replies.front().Ok())
            return std::nullopt;
        AgentCursor in(answer->Replies.front().Payload);
        const std::int64_t value = in.I64();
        if (!in.Ok())
            return std::nullopt;
        return static_cast<std::uint64_t>(value);
    }
    std::uint64_t UnityExplorer::ArgSpace(std::size_t bytes)
    {
        if (!m_args)
        {
            const auto process = Proc();
            if (!process)
                return 0;
            m_args = process->Allocate(kArgsSize, PAGE_READWRITE).ValueOr(0);
            if (!m_args)
                return 0;
        }
        const std::size_t aligned = (bytes + 15) & ~std::size_t(15);
        if (m_argsUsed + aligned > kArgsSize)
            return 0;
        const std::uint64_t at = m_args + m_argsUsed;
        m_argsUsed += aligned;
        return at;
    }

    std::string UnityExplorer::ReadString(std::uint64_t monoString, std::size_t limit) const
    {
        const auto process = Proc();
        if (!process || !monoString)
            return {};
        const MonoObjectLayout layout = MonoLayoutFor(process->PointerSize());
        std::int32_t length = 0;
        if (!process->ReadExact(monoString + layout.StringLength, &length, sizeof(length)) || length <= 0)
            return {};
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), limit);
        std::wstring wide(count, L'\0');
        const std::size_t got = process->Read(monoString + layout.StringChars, wide.data(), count * 2);
        wide.resize(got / 2);
        std::string text = WideToUtf8(wide);
        if (static_cast<std::size_t>(length) > limit)
            text += "...";
        return text;
    }

    std::vector<std::uint64_t> UnityExplorer::ReadArray(std::uint64_t monoArray, std::size_t limit, std::size_t* total) const
    {
        std::vector<std::uint64_t> items;
        const auto process = Proc();
        if (total)
            *total = 0;
        if (!process || !monoArray)
            return items;
        const MonoObjectLayout layout = MonoLayoutFor(process->PointerSize());
        const std::uint64_t length = process->ReadPointer(monoArray + layout.ArrayLength).value_or(~std::uint64_t(0));
        if (length > 0x10000000ull)
            return items;
        if (total)
            *total = static_cast<std::size_t>(length);
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(length, limit));
        std::vector<std::uint8_t> bytes(count * layout.Pointer);
        if (count && process->ReadExact(monoArray + layout.ArrayData, bytes.data(), bytes.size()))
        {
            items.resize(count);
            for (std::size_t i = 0; i < count; ++i)
                items[i] = LoadPointer(bytes.data() + i * layout.Pointer, layout.Pointer);
        }
        return items;
    }

    template <typename T>
    std::optional<T> UnityExplorer::Unbox(std::uint64_t boxed) const
    {
        const auto process = Proc();
        if (!process || !boxed)
            return std::nullopt;
        T value{};
        if (!process->ReadExact(boxed + MonoLayoutFor(process->PointerSize()).ObjectHeader, &value, sizeof(T)))
            return std::nullopt;
        return value;
    }

    // ---------------------------------------------------------------------
    // Classes, methods and types

    std::uint64_t UnityExplorer::FindClass(std::string_view name)
    {
        std::lock_guard bridge(m_mono.m_lock);
        const int index = m_mono.FindClass(name);
        return index < 0 ? 0 : m_mono.m_classes[static_cast<std::size_t>(index)].Handle;
    }

    std::string UnityExplorer::WorkerString(const char* function, std::uint64_t handle)
    {
        const auto pointer = Worker(function, handle);
        if (!pointer || !*pointer)
            return {};
        return m_mono.m_call.ReadCString(*pointer);
    }

    std::string UnityExplorer::TypeName(std::uint64_t type)
    {
        const auto text = Worker("mono_type_get_name", type);
        if (!text || !*text)
            return {};
        std::string name = m_mono.m_call.ReadCString(*text);
        Worker("mono_free", *text);
        return name;
    }

    MonoTypeInfo UnityExplorer::TypeInfo(std::uint64_t type)
    {
        if (!type)
            return {};
        if (const auto found = m_types.find(type); found != m_types.end())
            return found->second;
        const auto process = Proc();
        const int pointer = process ? process->PointerSize() : 8;
        MonoTypeInfo info;
        info.Code = static_cast<int>(Worker("mono_type_get_type", type).value_or(0) & 0xFF);
        info.ValueCode = info.Code;
        info.Class = Worker("mono_class_from_mono_type", type).value_or(0);
        info.Name = TypeName(type);
        if (IsPrimitive(info.Code))
        {
            info.ValueType = true;
            info.Size = PrimitiveSize(info.Code, pointer);
        }
        else if ((info.Code == MonoTypeCode::ValueType || info.Code == MonoTypeCode::GenericInst) && info.Class &&
                 (Worker("mono_class_is_valuetype", info.Class).value_or(0) & 0xFF))
        {
            info.ValueType = true;
            if (Worker("mono_class_is_enum", info.Class).value_or(0) & 0xFF)
            {
                info.Enum = true;
                if (const auto base = Worker("mono_class_enum_basetype", info.Class); base && *base)
                    info.ValueCode = static_cast<int>(Worker("mono_type_get_type", *base).value_or(MonoTypeCode::I4) & 0xFF);
                else
                    info.ValueCode = MonoTypeCode::I4;
                info.Size = std::max(1, PrimitiveSize(info.ValueCode, pointer));
            }
            else
            {
                const std::int64_t size = static_cast<std::int32_t>(Worker("mono_class_value_size", info.Class, 0).value_or(0) & 0xFFFFFFFFu);
                info.Size = static_cast<int>(std::clamp<std::int64_t>(size, 1, 4096));
            }
        }
        else if (info.Code == MonoTypeCode::Void)
            info.Size = 0;
        m_types[type] = info;
        return info;
    }

    std::string UnityExplorer::ClassName(std::uint64_t klass)
    {
        if (!klass)
            return {};
        {
            std::lock_guard state(m_stateLock);
            if (const auto found = m_classNames.find(klass); found != m_classNames.end() && (!found->second.empty() || !t_jobThread))
                return found->second;
        }
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (const auto found = m_mono.m_handleIndex.find(klass); found != m_mono.m_handleIndex.end())
                return m_mono.m_classes[static_cast<std::size_t>(found->second)].FullName();
        }
        // Not enumerated (a generic instance, an array): ask the runtime, on the
        // job thread only, so drawing never waits for a remote call.
        if (!t_jobThread)
        {
            bool queue = false;
            {
                std::lock_guard state(m_stateLock);
                queue = m_classNames.emplace(klass, std::string()).second;
            }
            if (queue)
                Post("Reading a class name", [this, klass] { ClassName(klass); Publish(); });
            return {};
        }
        std::string name;
        if (const auto type = Worker("mono_class_get_type", klass); type && *type)
            name = TypeName(*type);
        if (name.empty())
        {
            const std::string space = WorkerString("mono_class_get_namespace", klass);
            name = WorkerString("mono_class_get_name", klass);
            if (!space.empty())
                name = space + "." + name;
        }
        std::lock_guard state(m_stateLock);
        m_classNames[klass] = name;
        return name;
    }

    std::uint64_t UnityExplorer::ClassOf(std::uint64_t object) const
    {
        const auto process = Proc();
        if (!process || !object)
            return 0;
        const auto vtable = process->ReadPointer(object);
        if (!vtable || *vtable < 0x10000)
            return 0;
        if (Il2())
            return IsClass(*vtable) ? *vtable : 0;
        // ∑ Whatever comes back here can end up in mono_class_get_type /
        // mono_type_get_name on the worker, and Mono dereferences it blindly.
        // A field of an object the game had destroyed and collected (Content
        // Warning, after the player died and the level reloaded) produced a
        // "class" of 0x01000001A9DE141D and killed the game inside
        // mono_type_get_name. So nothing leaves here unvetted.
        const std::uint64_t klass = process->ReadPointer(*vtable).value_or(0);
        return IsClass(klass) ? klass : 0;
    }

    bool UnityExplorer::IsClass(std::uint64_t klass) const
    {
        const auto process = Proc();
        const std::uint64_t pointer = process ? static_cast<std::uint64_t>(process->PointerSize()) : 8;
        if (klass < 0x10000 || klass > (pointer == 4 ? 0xFFFFFFFFull : 0x7FFFFFFFFFFFull) || (klass & (pointer - 1)) != 0)
            return false;
        ClassShape shape;
        {
            std::lock_guard state(m_stateLock);
            if (m_validClasses.count(klass))
                return true;
            shape = m_classShape;
        }
        bool enumerated = false;
        {
            std::lock_guard bridge(m_mono.m_lock);
            enumerated = m_mono.m_handleIndex.count(klass) != 0;
        }
        if (enumerated)
        {
            std::lock_guard state(m_stateLock);
            m_validClasses.insert(klass);
            return true;
        }
        // Not enumerated (a generic instance, an array): its image field must
        // name a loaded image, and its name field a readable name.
        if (!shape.Ready || !process)
            return false;
        const auto image = process->ReadPointer(klass + shape.Image);
        if (!image || !*image)
            return false;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_images.count(*image))
                return false;
        }
        const auto name = process->ReadPointer(klass + shape.Name);
        if (!name || *name < 0x10000)
            return false;
        char text[128] = {};
        const std::size_t got = process->Read(*name, text, sizeof(text));
        std::size_t length = 0;
        while (length < got && text[length] != '\0')
        {
            if (static_cast<unsigned char>(text[length]) < 0x20)
                return false;
            ++length;
        }
        if (length == 0 || length == got)
            return false;
        std::lock_guard state(m_stateLock);
        m_validClasses.insert(klass);
        return true;
    }

    void UnityExplorer::LearnClassShape()
    {
        {
            std::lock_guard state(m_stateLock);
            if (m_classShape.Ready || m_classShape.Failed)
                return;
        }
        std::vector<std::uint64_t> samples;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_present || m_mono.m_classes.empty())
                return;
            const std::size_t count = m_mono.m_classes.size();
            for (std::size_t i = 0; i < 6 && i < count; ++i)
                samples.push_back(m_mono.m_classes[i * count / 6].Handle);
        }
        // The offsets at which the class holds what mono_class_get_image and
        // mono_class_get_name return, the same in every sample.
        const auto process = Proc();
        std::optional<std::uint32_t> imageAt;
        std::optional<std::uint32_t> nameAt;
        int agreed = 0;
        bool consistent = process != nullptr;
        for (const std::uint64_t klass : samples)
        {
            if (!consistent)
                break;
            const std::uint64_t image = Worker("mono_class_get_image", klass).value_or(0);
            const std::uint64_t name = Worker("mono_class_get_name", klass).value_or(0);
            const std::uint32_t pointer = static_cast<std::uint32_t>(process->PointerSize());
            std::uint8_t words[64 * 8] = {};
            if (!image || !name || !process->ReadExact(klass, words, 64 * pointer))
                continue;
            std::optional<std::uint32_t> foundImage;
            std::optional<std::uint32_t> foundName;
            for (std::uint32_t i = 0; i < 64; ++i)
            {
                const std::uint64_t word = LoadPointer(words + i * pointer, pointer);
                if (!foundImage && word == image)
                    foundImage = i * pointer;
                if (!foundName && word == name)
                    foundName = i * pointer;
            }
            if (!foundImage || !foundName || (imageAt && *imageAt != *foundImage) || (nameAt && *nameAt != *foundName))
                consistent = false;
            imageAt = foundImage;
            nameAt = foundName;
            ++agreed;
        }
        std::lock_guard state(m_stateLock);
        if (consistent && agreed >= 2 && imageAt && nameAt)
        {
            m_classShape.Ready = true;
            m_classShape.Image = *imageAt;
            m_classShape.Name = *nameAt;
        }
        else
        {
            m_classShape.Failed = true;
            LOG_WARN("Unity", "Could not learn where this runtime keeps a class's image and name; objects of generic and array classes stay unnamed");
        }
    }

    std::string UnityExplorer::ClassNameOf(std::uint64_t object)
    {
        return ClassName(ClassOf(object));
    }

    std::vector<std::string> UnityExplorer::ChainOf(std::uint64_t klass)
    {
        if (const auto found = m_chains.find(klass); found != m_chains.end())
            return found->second;
        std::vector<std::string> chain;
        std::uint64_t current = klass;
        for (int depth = 0; current && depth < 48; ++depth)
        {
            chain.push_back(ClassName(current));
            current = Worker("mono_class_get_parent", current).value_or(0);
        }
        m_chains[klass] = chain;
        return chain;
    }

    bool UnityExplorer::IsUnityObjectClass(std::uint64_t klass)
    {
        // Walk the parent chain by class handle and match UnityEngine.Object's
        // handle, rather than by name: metadata-only (no managed code) and it
        // does not depend on every parent's name resolving.
        const std::uint64_t unityObject = FindClass("UnityEngine.Object");
        if (!unityObject)
            return false;
        std::uint64_t current = klass;
        for (int depth = 0; current && depth < 64; ++depth)
        {
            if (current == unityObject)
                return true;
            current = Worker("mono_class_get_parent", current).value_or(0);
        }
        return false;
    }

    std::shared_ptr<const MonoClassLayout> UnityExplorer::LoadLayout(std::uint64_t klass)
    {
        {
            std::lock_guard state(m_stateLock);
            if (const auto found = m_layouts.find(klass); found != m_layouts.end())
                return found->second;
        }
        auto layout = std::make_shared<MonoClassLayout>();
        const auto process = Proc();
        if (!klass || !process)
            return layout;
        layout->Klass = klass;
        layout->Name = ClassName(klass);
        if (const auto image = Worker("mono_class_get_image", klass); image && *image)
            layout->Image = WorkerString("mono_image_get_name", *image);
        layout->TypeToken = static_cast<std::uint32_t>(Worker("mono_class_get_type_token", klass).value_or(0) & 0xFFFFFFFFu);
        layout->Chain = ChainOf(klass);
        const auto has = [&](const char* name) { return std::find(layout->Chain.begin(), layout->Chain.end(), name) != layout->Chain.end(); };
        layout->UnityObject = has("UnityEngine.Object");
        layout->GameObject = has("UnityEngine.GameObject");
        layout->Component = has("UnityEngine.Component");
        layout->Transform = has("UnityEngine.Transform");
        layout->Behaviour = has("UnityEngine.Behaviour");
        layout->ValueType = (Worker("mono_class_is_valuetype", klass).value_or(0) & 0xFF) != 0;
        layout->Enum = layout->ValueType && (Worker("mono_class_is_enum", klass).value_or(0) & 0xFF) != 0;
        layout->Rank = static_cast<int>(Worker("mono_class_get_rank", klass).value_or(0) & 0xFF);
        if (layout->Rank > 0)
        {
            if (const auto element = Worker("mono_class_get_element_class", klass); element && *element)
                if (const auto type = Worker("mono_class_get_type", *element); type && *type)
                    layout->Element = TypeInfo(*type);
            layout->ElementSize = static_cast<int>(Worker("mono_array_element_size", klass).value_or(static_cast<std::uint64_t>(process->PointerSize())) & 0xFFFF);
        }

        if (Il2())
        {
            Il2LoadMembers(*layout, klass);
            std::lock_guard state(m_stateLock);
            m_layouts[klass] = layout;
            m_layoutQueued.erase(klass);
            return layout;
        }

        std::uint64_t domain = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            domain = m_mono.m_domain;
        }
        const std::size_t mark = m_argsUsed;
        const std::uint64_t iter = ArgSpace(8);
        const std::uint64_t paramIter = ArgSpace(8);
        const std::size_t pointer = static_cast<std::size_t>(process->PointerSize());
        const std::uint64_t names = ArgSpace(8 * 64);
        const std::uint64_t literal = ArgSpace(64);
        const std::uint64_t zero = 0;
        const auto reset = [&](std::uint64_t at) { process->Write(at, &zero, sizeof(zero)); };
        std::uint64_t current = klass;
        for (int depth = 0; current && depth < 48 && iter; ++depth, current = Worker("mono_class_get_parent", current).value_or(0))
        {
            const std::string declaring = ClassName(current);
            if (declaring == "System.Object" || declaring == "System.ValueType" || declaring == "System.Enum")
                break;
            std::string image;
            if (const auto owner = Worker("mono_class_get_image", current); owner && *owner)
                image = WorkerString("mono_image_get_name", *owner);
            const auto token = [&](const char* function, std::uint64_t handle)
                {
                    return handle ? static_cast<std::uint32_t>(Worker(function, handle).value_or(0) & 0xFFFFFFFFu) : 0u;
                };
            std::uint64_t vtable = 0;
            std::uint64_t staticData = 0;
            bool vtableRead = false;
            const auto statics = [&]
                {
                    if (!vtableRead)
                    {
                        vtableRead = true;
                        vtable = Exec("mono_class_vtable", domain, current).value_or(0);
                        if (vtable)
                            staticData = Exec("mono_vtable_get_static_field_data", vtable).value_or(0);
                    }
                };

            reset(iter);
            for (int n = 0; n < kMaxMembers; ++n)
            {
                const auto field = Worker("mono_class_get_fields", current, iter);
                if (!field || !*field)
                    break;
                MonoMember member;
                member.Kind = MonoMemberKind::Field;
                member.Handle = *field;
                member.Declaring = declaring;
                member.Image = image;
                member.Token = token("mono_class_get_field_token", *field);
                member.Name = WorkerString("mono_field_get_name", *field);
                const std::uint32_t flags = static_cast<std::uint32_t>(Worker("mono_field_get_flags", *field).value_or(0) & 0xFFFFFFFFu);
                member.Static = (flags & kFieldStatic) != 0;
                member.Literal = (flags & kFieldLiteral) != 0;
                member.Offset = static_cast<int>(Worker("mono_field_get_offset", *field).value_or(0) & 0xFFFFFFFFu);
                if (const auto type = Worker("mono_field_get_type", *field); type && *type)
                    member.Type = TypeInfo(*type);
                if (member.Static)
                {
                    statics();
                    member.VTable = vtable;
                    member.StaticData = staticData;
                    if (member.Literal && vtable && literal && member.Type.Size <= 64)
                    {
                        std::uint8_t clear[64] = {};
                        process->Write(literal, clear, sizeof(clear));
                        Exec("mono_field_static_get_value", vtable, *field, literal);
                        member.LiteralBytes.resize(member.Type.ValueType ? static_cast<std::size_t>(member.Type.Size) : pointer);
                        process->ReadExact(literal, member.LiteralBytes.data(), member.LiteralBytes.size());
                    }
                }
                if (layout->Enum && depth == 0)
                {
                    if (member.Literal && !member.LiteralBytes.empty())
                    {
                        std::int64_t value = 0;
                        std::memcpy(&value, member.LiteralBytes.data(), std::min<std::size_t>(8, member.LiteralBytes.size()));
                        const int size = static_cast<int>(member.LiteralBytes.size());
                        if (size < 8 && (member.Type.ValueCode == MonoTypeCode::I1 || member.Type.ValueCode == MonoTypeCode::I2 || member.Type.ValueCode == MonoTypeCode::I4))
                            value = (value << (64 - size * 8)) >> (64 - size * 8);
                        layout->EnumValues.push_back({ value, member.Name });
                    }
                    if (!member.Static && member.Name == "value__")
                        continue;
                }
                layout->Members.push_back(std::move(member));
            }

            std::vector<std::string> propertyNames;
            reset(iter);
            for (int n = 0; n < kMaxMembers; ++n)
            {
                const auto property = Worker("mono_class_get_properties", current, iter);
                if (!property || !*property)
                    break;
                MonoMember member;
                member.Kind = MonoMemberKind::Property;
                member.Handle = *property;
                member.Declaring = declaring;
                member.Name = WorkerString("mono_property_get_name", *property);
                member.Getter = Worker("mono_property_get_get_method", *property).value_or(0);
                member.Setter = Worker("mono_property_get_set_method", *property).value_or(0);
                member.Image = image;
                member.GetterToken = token("mono_method_get_token", member.Getter);
                member.SetterToken = token("mono_method_get_token", member.Setter);
                propertyNames.push_back(member.Name);
                const std::uint64_t accessor = member.Getter ? member.Getter : member.Setter;
                if (!accessor)
                    continue;
                const auto signature = Worker("mono_method_signature", accessor);
                if (!signature || !*signature)
                    continue;
                const int count = static_cast<int>(Worker("mono_signature_get_param_count", *signature).value_or(0) & 0xFFFFFFFFu);
                if (member.Getter)
                {
                    if (count != 0)
                        continue;  // an indexer
                    if (const auto type = Worker("mono_signature_get_return_type", *signature); type && *type)
                        member.Type = TypeInfo(*type);
                }
                else
                {
                    if (count != 1)
                        continue;
                    reset(paramIter);
                    if (const auto type = Worker("mono_signature_get_params", *signature, paramIter); type && *type)
                        member.Type = TypeInfo(*type);
                }
                const std::uint32_t flags = static_cast<std::uint32_t>(Worker("mono_method_get_flags", accessor, 0).value_or(0) & 0xFFFFFFFFu);
                member.Static = (flags & kMethodStatic) != 0;
                layout->Members.push_back(std::move(member));
            }

            reset(iter);
            for (int n = 0; n < kMaxMembers; ++n)
            {
                const auto method = Worker("mono_class_get_methods", current, iter);
                if (!method || !*method)
                    break;
                MonoMember member;
                member.Kind = MonoMemberKind::Method;
                member.Handle = *method;
                member.Declaring = declaring;
                member.Image = image;
                member.Token = token("mono_method_get_token", *method);
                member.Name = WorkerString("mono_method_get_name", *method);
                if (member.Name == ".ctor" || member.Name == ".cctor")
                    continue;
                // Property accessors show as the property, not as methods.
                if (member.Name.size() > 4 && (member.Name.rfind("get_", 0) == 0 || member.Name.rfind("set_", 0) == 0) &&
                    std::find(propertyNames.begin(), propertyNames.end(), member.Name.substr(4)) != propertyNames.end())
                    continue;
                const std::uint32_t flags = static_cast<std::uint32_t>(Worker("mono_method_get_flags", *method, 0).value_or(0) & 0xFFFFFFFFu);
                member.Static = (flags & kMethodStatic) != 0;
                const auto signature = Worker("mono_method_signature", *method);
                if (signature && *signature)
                {
                    if (const auto type = Worker("mono_signature_get_return_type", *signature); type && *type)
                        member.Type = TypeInfo(*type);
                    const int count = static_cast<int>(Worker("mono_signature_get_param_count", *signature).value_or(0) & 0xFFFFFFFFu);
                    reset(paramIter);
                    for (int p = 0; p < count && p < 64; ++p)
                    {
                        const auto type = Worker("mono_signature_get_params", *signature, paramIter);
                        if (!type || !*type)
                            break;
                        member.Params.push_back(TypeInfo(*type));
                    }
                    if (names && count > 0 && count <= 64)
                    {
                        std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * pointer, 0);
                        process->Write(names, raw.data(), raw.size());
                        Worker("mono_method_get_param_names", *method, names);
                        process->ReadExact(names, raw.data(), raw.size());
                        for (std::size_t p = 0; p < static_cast<std::size_t>(count); ++p)
                        {
                            const std::uint64_t text = LoadPointer(raw.data() + p * pointer, pointer);
                            member.ParamNames.push_back(text ? m_mono.m_call.ReadCString(text, 128) : std::string());
                        }
                    }
                }
                layout->Members.push_back(std::move(member));
            }
        }
        m_argsUsed = mark;
        std::lock_guard state(m_stateLock);
        m_layouts[klass] = layout;
        m_layoutQueued.erase(klass);
        return layout;
    }

    std::shared_ptr<const MonoClassLayout> UnityExplorer::CachedLayout(std::uint64_t klass)
    {
        if (!klass)
            return nullptr;
        {
            std::lock_guard state(m_stateLock);
            if (const auto found = m_layouts.find(klass); found != m_layouts.end())
                return found->second;
            if (!m_layoutQueued.emplace(klass, true).second)
                return nullptr;
        }
        Post("Reading a class", [this, klass] { LoadLayout(klass); Publish(); });
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // Values

    std::string UnityExplorer::ShortTypeName(const MonoTypeInfo& type)
    {
        return type.Name.empty() ? std::string("?") : ShortName(type.Name);
    }

    ExplorerExitNote UnityExplorer::ExitNote(const UnityStatus& status, std::string_view process, std::optional<std::uint32_t> exitCode, double seconds)
    {
        ExplorerExitNote note;
        note.Crashed = exitCode && *exitCode >= 0xC0000000u;
        std::string text(process);
        if (note.Crashed)
            text += " crashed with exit code 0x" + Hex(*exitCode, 8);
        else if (exitCode)
            text += " ended with exit code " + std::to_string(*exitCode);
        else
            text += " ended";
        text += Manifold::Format(" while the scene explorer's agent was in it: %.0f s on %s, %llu frames", std::max(0.0, seconds),
            status.Hook.empty() ? "the frame callback" : status.Hook.c_str(), static_cast<unsigned long long>(status.CallbackRuns));
        if (!status.LastTask.empty())
            text += ", last request \"" + status.LastTask + "\"" + (status.Busy ? " (still running)" : "");
        if (!status.UnityVersion.empty())
            text += ", Unity " + status.UnityVersion;
        if (!status.Error.empty())
            text += ", last error: " + status.Error;
        note.Text = std::move(text);
        return note;
    }

    std::string UnityExplorer::Signature(const MonoMember& method)
    {
        std::string text = "(";
        for (std::size_t i = 0; i < method.Params.size(); ++i)
        {
            if (i)
                text += ", ";
            text += ShortTypeName(method.Params[i]);
            if (i < method.ParamNames.size() && !method.ParamNames[i].empty())
                text += " " + method.ParamNames[i];
        }
        return text + ")";
    }

    MonoShown UnityExplorer::Format(const MonoTypeInfo& type, const std::uint8_t* bytes, std::size_t size, int depth)
    {
        MonoShown shown;
        const auto process = Proc();
        const MonoObjectLayout objects = MonoLayoutFor(process ? process->PointerSize() : 8);
        const int code = type.ValueCode ? type.ValueCode : type.Code;
        const auto need = [&](std::size_t n) { return size >= n; };
        if (type.Enum)
        {
            const int width = std::max(1, PrimitiveSize(code, static_cast<int>(objects.Pointer)));
            if (!need(static_cast<std::size_t>(width)))
                return { "??" };
            std::int64_t value = 0;
            std::memcpy(&value, bytes, static_cast<std::size_t>(width));
            if (width < 8 && (code == MonoTypeCode::I1 || code == MonoTypeCode::I2 || code == MonoTypeCode::I4))
                value = (value << (64 - width * 8)) >> (64 - width * 8);
            shown.Editable = true;
            shown.Text = std::to_string(value);
            if (const auto layout = CachedLayout(type.Class))
            {
                std::string names;
                for (const MonoEnumValue& entry : layout->EnumValues)
                {
                    if (entry.Value == value)
                    {
                        names = entry.Name;
                        break;
                    }
                }
                if (names.empty() && value != 0)
                {
                    std::int64_t rest = value;
                    for (const MonoEnumValue& entry : layout->EnumValues)
                    {
                        if (entry.Value != 0 && (rest & entry.Value) == entry.Value)
                        {
                            names += (names.empty() ? "" : " | ") + entry.Name;
                            rest &= ~entry.Value;
                        }
                    }
                    if (rest != 0)
                        names.clear();
                }
                if (!names.empty())
                    shown.Text = names + " (" + shown.Text + ")";
            }
            else
                shown.Pending = true;
            return shown;
        }
        switch (code)
        {
        case MonoTypeCode::Boolean: if (!need(1)) break; shown.Text = bytes[0] ? "true" : "false"; shown.Editable = true; return shown;
        case MonoTypeCode::Char:
        {
            if (!need(2)) break;
            const std::uint16_t unit = Load<std::uint16_t>(bytes);
            const wchar_t wide[2] = { static_cast<wchar_t>(unit), 0 };
            shown.Text = unit >= 0x20 ? "'" + WideToUtf8(wide) + "'" : Manifold::Format("\\u%04X", unit);
            shown.Editable = true;
            return shown;
        }
        case MonoTypeCode::I1: if (!need(1)) break; shown.Text = std::to_string(Load<std::int8_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::U1: if (!need(1)) break; shown.Text = std::to_string(Load<std::uint8_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::I2: if (!need(2)) break; shown.Text = std::to_string(Load<std::int16_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::U2: if (!need(2)) break; shown.Text = std::to_string(Load<std::uint16_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::I4: if (!need(4)) break; shown.Text = std::to_string(Load<std::int32_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::U4: if (!need(4)) break; shown.Text = std::to_string(Load<std::uint32_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::I8: if (!need(8)) break; shown.Text = std::to_string(Load<std::int64_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::U8: if (!need(8)) break; shown.Text = std::to_string(Load<std::uint64_t>(bytes)); shown.Editable = true; return shown;
        case MonoTypeCode::R4: if (!need(4)) break; shown.Text = FormatFloat(Load<float>(bytes), 7); shown.Editable = true; return shown;
        case MonoTypeCode::R8: if (!need(8)) break; shown.Text = FormatFloat(Load<double>(bytes), 15); shown.Editable = true; return shown;
        case MonoTypeCode::I: case MonoTypeCode::U: case MonoTypeCode::Ptr: case MonoTypeCode::FnPtr:
            if (!need(objects.Pointer)) break;
            shown.Text = "0x" + Hex(LoadPointer(bytes, objects.Pointer));
            shown.Editable = true;
            return shown;
        default:
            break;
        }
        if (type.ValueType && !IsPrimitive(code))
        {
            const auto layout = CachedLayout(type.Class);
            if (!layout)
            {
                shown.Text = "...";
                shown.Pending = true;
                return shown;
            }
            std::vector<std::string> parts;
            bool editable = true;
            for (const MonoMember& field : layout->Members)
            {
                if (field.Kind != MonoMemberKind::Field || field.Static)
                    continue;
                const std::int64_t at = static_cast<std::int64_t>(field.Offset) - static_cast<std::int64_t>(objects.ObjectHeader);
                if (at < 0 || static_cast<std::size_t>(at) >= size || parts.size() >= 8)
                    break;
                if (!field.Type.ValueType)
                    editable = false;
                if (depth >= 2)
                {
                    parts.push_back("...");
                    editable = false;
                    break;
                }
                const MonoShown inner = Format(field.Type, bytes + at, size - static_cast<std::size_t>(at), depth + 1);
                if (inner.Pending)
                    shown.Pending = true;
                if (!inner.Editable || (field.Type.ValueType && !IsPrimitive(field.Type.ValueCode) && !field.Type.Enum))
                    editable = false;
                parts.push_back(inner.Text);
            }
            shown.Text = "(" + Join(parts, ", ") + ")";
            shown.Editable = editable && !parts.empty();
            return shown;
        }
        if (!need(objects.Pointer))
            return { "??" };
        const std::uint64_t pointer = LoadPointer(bytes, objects.Pointer);
        if (!pointer)
        {
            shown.Text = "null";
            shown.Null = true;
            shown.Editable = code == MonoTypeCode::String;
            return shown;
        }
        shown.Reference = pointer;
        const std::uint64_t klass = ClassOf(pointer);
        std::string className = ClassName(klass);
        if (code == MonoTypeCode::String || className == "System.String")
        {
            shown.Text = "\"" + ReadString(pointer, 256) + "\"";
            shown.Editable = code == MonoTypeCode::String;
            shown.Reference = 0;
            return shown;
        }
        if (className.empty())
            className = ShortTypeName(type);
        const std::optional<std::uint64_t> length = process && !className.empty() && className.back() == ']' ? process->ReadPointer(pointer + objects.ArrayLength) : std::nullopt;
        if (length)
            shown.Text = ShortName(className) + "  (" + std::to_string(*length) + ")";
        else
            shown.Text = ShortName(className);
        return shown;
    }

    MonoShown UnityExplorer::ReadField(const MonoMember& field, std::uint64_t object)
    {
        if (field.Literal)
        {
            if (field.LiteralBytes.empty())
                return { "const" };
            return Format(field.Type, field.LiteralBytes.data(), field.LiteralBytes.size());
        }
        const auto process = Proc();
        if (!process)
            return { "??" };
        std::uint64_t address = 0;
        if (field.Static)
        {
            if (!field.StaticData && field.Offset >= 0 && Il2())
                return { "(not set up yet)" };
            if (!field.StaticData || field.Offset < 0 || field.Offset > 0x1000000)
                return { "(thread static)" };
            address = field.StaticData + static_cast<std::uint64_t>(field.Offset);
        }
        else
        {
            if (!object)
                return { "" };
            address = object + static_cast<std::uint64_t>(field.Offset);
        }
        const std::size_t size = static_cast<std::size_t>(field.Type.ValueType ? std::max(1, field.Type.Size) : process->PointerSize());
        std::vector<std::uint8_t> bytes(size, 0);
        if (!process->ReadExact(address, bytes.data(), bytes.size()))
            return { "??" };
        return Format(field.Type, bytes.data(), bytes.size());
    }

    MonoShown UnityExplorer::Shown(const MonoMember& member, const MonoReadValue& value)
    {
        if (!value.Read)
            return { value.Error.empty() ? std::string() : value.Error };
        if (member.Type.Code == MonoTypeCode::Void)
            return { "done" };
        if (!value.Text.empty() || member.Type.Code == MonoTypeCode::String)
        {
            MonoShown shown;
            const std::uint64_t pointer = value.Raw.empty() ? 0 : LoadPointer(value.Raw.data(), value.Raw.size());
            shown.Text = pointer ? "\"" + value.Text + "\"" : "null";
            shown.Null = !pointer;
            shown.Editable = true;
            return shown;
        }
        return Format(member.Type, value.Raw.data(), value.Raw.size());
    }

    std::optional<std::vector<std::uint8_t>> UnityExplorer::Parse(const MonoTypeInfo& type, const std::string& text, std::string* error)
    {
        const auto fail = [&](const std::string& why) -> std::optional<std::vector<std::uint8_t>> { if (error) *error = why; return std::nullopt; };
        if (!type.ValueType)
            return fail("not an inline value");
        std::vector<std::uint8_t> out(static_cast<std::size_t>(std::max(1, type.Size)), 0);
        const int code = type.ValueCode ? type.ValueCode : type.Code;
        if (type.Enum)
        {
            if (const auto number = ParseInteger(text))
            {
                std::memcpy(out.data(), &*number, std::min<std::size_t>(out.size(), 8));
                return out;
            }
            const auto layout = CachedLayout(type.Class);
            if (!layout)
                return fail("the enum names are still loading");
            std::int64_t value = 0;
            for (const std::string& part : Split(text, '|'))
            {
                const std::string name = Trim(part);
                const auto found = std::find_if(layout->EnumValues.begin(), layout->EnumValues.end(), [&](const MonoEnumValue& v) { return EqualsNoCase(v.Name, name); });
                if (found == layout->EnumValues.end())
                    return fail("no value named " + name);
                value |= found->Value;
            }
            std::memcpy(out.data(), &value, std::min<std::size_t>(out.size(), 8));
            return out;
        }
        const auto process = Proc();
        const MonoObjectLayout objects = MonoLayoutFor(process ? process->PointerSize() : 8);
        if (IsPrimitive(code))
        {
            if (!ParsePrimitive(code, static_cast<int>(objects.Pointer), text, out.data(), error))
                return std::nullopt;
            return out;
        }
        const auto layout = CachedLayout(type.Class);
        if (!layout)
            return fail("the struct layout is still loading");
        std::string body = Trim(text);
        if (body.size() >= 2 && body.front() == '(' && body.back() == ')')
            body = body.substr(1, body.size() - 2);
        const std::vector<std::string> parts = Split(body, ',', true);
        std::vector<const MonoMember*> fields;
        for (const MonoMember& field : layout->Members)
        {
            if (field.Kind == MonoMemberKind::Field && !field.Static)
                fields.push_back(&field);
        }
        if (fields.size() != parts.size())
            return fail("expected " + std::to_string(fields.size()) + " values separated by commas");
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const MonoMember& field = *fields[i];
            const std::int64_t at = static_cast<std::int64_t>(field.Offset) - static_cast<std::int64_t>(objects.ObjectHeader);
            if (!field.Type.ValueType || at < 0 || static_cast<std::size_t>(at + std::max(1, field.Type.Size)) > out.size())
                return fail("this struct holds references and cannot be typed in");
            const auto inner = Parse(field.Type, parts[i], error);
            if (!inner)
                return std::nullopt;
            Put(out, static_cast<std::size_t>(at), inner->data(), inner->size());
        }
        return out;
    }

    // ---------------------------------------------------------------------
    // The per-frame callback

    Status UnityExplorer::EnsureAgent()
    {
        // Last line of defence: nothing goes into the game's frame callback
        // unless the user turned the explorer on for this session.
        if (!m_enabled.load())
            return Status::Fail(ErrorCode::MonoCallFailed, "the scene explorer is switched off");
        if (m_mono.Runtime() != ManagedRuntime::Mono)
            return Status::Fail(ErrorCode::MonoCallFailed, "the scene explorer needs a Mono runtime");
        if (!m_agent)
            m_agent = std::make_unique<AgentLink>(m_mono);
        if (m_agent->Connected() && m_agentReady)
            return Status::Success();
        {
            static const char* const kNeeded[] = { "mono_method_get_token", "mono_class_get_field_token", "mono_class_get_type_token" };
            std::vector<std::string> missing;
            {
                std::lock_guard bridge(m_mono.m_lock);
                for (const char* name : kNeeded)
                {
                    if (!m_mono.Fn(name))
                        missing.emplace_back(name);
                }
            }
            if (!missing.empty())
                LOG_WARN("Unity", "This Mono runtime is missing: " + Join(missing, ", "));
            std::lock_guard state(m_stateLock);
            m_status.Missing = std::move(missing);
        }
        if (Status connected = m_agent->Connect(); !connected)
            return connected;
        ForgetSlots();
        AgentRequest request;
        request.Op(AgentProtocol::Info);
        const Result<AgentLink::Exchanged> answer = Ask(request);
        if (!answer)
            return answer.Error();
        const AgentReply& reply = answer->Replies.front();
        if (!reply.Ok())
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent could not read the game's Unity version: " + reply.Error());
        AgentCursor in(reply.Payload);
        const std::string version = in.Text();
        const std::int32_t background = in.I32();
        const std::string hook = in.Text();
        m_agentReady = true;
        {
            std::lock_guard state(m_stateLock);
            if (!m_status.Hooked)
                m_status.HookedSince = std::chrono::steady_clock::now();
            m_status.Hooked = true;
            m_status.Hook = hook;
            m_status.UnityVersion = version;
            if (background >= 0)
                m_status.RunInBackground = background != 0;
            m_status.CallbackRuns = m_agent->Frames();
            m_status.Error.clear();
        }
        LOG_INFO("Unity", "The agent answers from " + hook + " (Unity " + version + ")");
        Publish();
        return Status::Success();
    }

    Result<AgentLink::Exchanged> UnityExplorer::Ask(AgentRequest& request, std::chrono::milliseconds timeout)
    {
        if (!m_agent || !m_agent->Connected())
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent is not in the game");
        const std::size_t requests = request.Count();
        const int stateBefore = m_agent->RemoteState();
        Result<AgentLink::Exchanged> answer = m_agent->Exchange(request.Finish(), timeout);
        ++m_statTrips;
        m_statCalls += static_cast<int>(requests);
        if (stateBefore != AgentProtocol::kStateHooked)
            ForgetSlots();
        {
            std::lock_guard state(m_stateLock);
            m_status.CallbackRuns = m_agent->Frames();
            m_status.Hooked = m_agent->RemoteState() == AgentProtocol::kStateHooked;
        }
        if (!answer)
            return answer;
        m_statFrames += static_cast<int>(answer->Frames);
        if (answer->Replies.size() != requests)
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent answered " + std::to_string(answer->Replies.size()) + " of " + std::to_string(requests) + " requests");
        return answer;
    }

    Status UnityExplorer::Command(AgentRequest& request, const std::string& what)
    {
        const Result<AgentLink::Exchanged> answer = Ask(request);
        if (!answer)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": " + answer.Error().Text);
        for (const AgentReply& reply : answer->Replies)
        {
            if (!reply.Ok())
                return Status::Fail(ErrorCode::MonoCallFailed, what + ": " + reply.Error());
        }
        return Status::Success();
    }

    void UnityExplorer::LearnSlots(const std::vector<int>& slots)
    {
        if (!m_agent)
            return;
        std::vector<int> unknown;
        {
            std::lock_guard state(m_stateLock);
            for (const int slot : slots)
            {
                if (slot > 0 && !m_slotAddress.count(slot))
                    unknown.push_back(slot);
            }
        }
        if (unknown.empty())
            return;
        const std::map<int, std::uint64_t> found = m_agent->Addresses(unknown);
        std::lock_guard state(m_stateLock);
        for (const auto& [slot, address] : found)
        {
            if (!address)
                continue;
            m_slotAddress[slot] = address;
            m_addressSlot[address] = slot;
        }
    }

    int UnityExplorer::SlotOf(std::uint64_t object) const
    {
        if (!object)
            return 0;
        std::lock_guard state(m_stateLock);
        const auto found = m_addressSlot.find(object);
        return found == m_addressSlot.end() ? 0 : found->second;
    }

    std::uint64_t UnityExplorer::AddressOf(int slot) const
    {
        if (slot <= 0)
            return 0;
        std::lock_guard state(m_stateLock);
        const auto found = m_slotAddress.find(slot);
        return found == m_slotAddress.end() ? 0 : found->second;
    }

    void UnityExplorer::HoldSlots(int group, const std::vector<int>& slots)
    {
        if (Il2())
            return Il2Hold(group, slots);
        if (slots.empty() || !m_agent || !m_agent->Connected())
            return;
        AgentRequest request;
        request.Op(AgentProtocol::Hold).I32(group).I32(static_cast<std::int32_t>(slots.size()));
        for (const int slot : slots)
            request.I32(slot);
        if (const Status held = Command(request, "Keeping the object"); !held)
            LOG_WARN("Unity", held.Text);
    }

    void UnityExplorer::ReleaseGroup(int group)
    {
        if (Il2())
            return Il2Release(group);
        if (!m_agent || !m_agent->Connected() || m_agent->RemoteState() != AgentProtocol::kStateHooked)
            return;
        AgentRequest request;
        request.Op(AgentProtocol::Release).I32(group);
        const Result<AgentLink::Exchanged> answer = Ask(request);
        if (!answer || !answer->Replies.front().Ok())
            return;
        AgentCursor in(answer->Replies.front().Payload);
        const std::int32_t count = in.I32();
        std::lock_guard state(m_stateLock);
        for (std::int32_t i = 0; i < count && in.Ok(); ++i)
        {
            const int slot = in.I32();
            const auto found = m_slotAddress.find(slot);
            if (found == m_slotAddress.end())
                continue;
            const auto back = m_addressSlot.find(found->second);
            if (back != m_addressSlot.end() && back->second == slot)
                m_addressSlot.erase(back);
            m_slotAddress.erase(found);
        }
    }

    void UnityExplorer::ForgetSlots()
    {
        std::lock_guard state(m_stateLock);
        m_slotAddress.clear();
        m_addressSlot.clear();
    }

    std::pair<std::string, std::uint32_t> UnityExplorer::ClassToken(std::uint64_t klass)
    {
        if (const auto found = m_classTokens.find(klass); found != m_classTokens.end())
            return found->second;
        std::pair<std::string, std::uint32_t> token;
        if (const auto image = Worker("mono_class_get_image", klass); image && *image)
            token.first = WorkerString("mono_image_get_name", *image);
        token.second = static_cast<std::uint32_t>(Worker("mono_class_get_type_token", klass).value_or(0) & 0xFFFFFFFFu);
        m_classTokens[klass] = token;
        return token;
    }

    void UnityExplorer::WriteArg(AgentRequest& request, const AgentArg& arg)
    {
        request.U8(arg.Kind);
        if (arg.Kind == AgentProtocol::ArgSlot)
            request.I32(arg.Slot);
        else if (arg.Kind == AgentProtocol::ArgBytes)
            request.Bytes(arg.Bytes);
        else if (arg.Kind == AgentProtocol::ArgText)
            request.Text(arg.Text);
    }

    UnityExplorer::PendingValue UnityExplorer::ReadValue(AgentCursor& in, const MonoTypeInfo& type)
    {
        PendingValue pending;
        pending.Value.Read = true;
        switch (in.U8())
        {
        case AgentProtocol::ValueBytes:
            pending.Value.Raw = in.Bytes();
            break;
        case AgentProtocol::ValueText:
            pending.Slot = in.I32();
            pending.Value.Text = in.Text();
            pending.Reference = true;
            break;
        case AgentProtocol::ValueObject:
            pending.Slot = in.I32();
            pending.Reference = true;
            break;
        default:
            if (type.Code != MonoTypeCode::Void)
                pending.Value.Raw.assign(type.ValueType ? static_cast<std::size_t>(std::max(1, type.Size)) : PointerBytes(), 0);
            break;
        }
        if (!in.Ok())
        {
            pending.Value = {};
            pending.Value.Error = "the agent's answer is damaged";
        }
        return pending;
    }

    void UnityExplorer::Resolve(PendingValue& pending)
    {
        if (!pending.Reference)
            return;
        const std::uint64_t address = AddressOf(pending.Slot);
        pending.Value.Raw.resize(PointerBytes());
        std::memcpy(pending.Value.Raw.data(), &address, pending.Value.Raw.size());
    }

    // ---------------------------------------------------------------------
    // Scenes

    void UnityExplorer::ReadScenes()
    {
        if (!Allowed())
            return;
        Post("Reading the scenes", [this] { DoReadScenes(); });
    }

    void UnityExplorer::ExpandScene(int scene)
    {
        if (!Allowed())
            return;
        {
            std::lock_guard state(m_stateLock);
            if (scene < 0 || scene >= static_cast<int>(m_tree.Scenes.size()) || m_tree.Scenes[static_cast<std::size_t>(scene)].RootsLoaded ||
                m_tree.Scenes[static_cast<std::size_t>(scene)].Loading)
                return;
            m_tree.Scenes[static_cast<std::size_t>(scene)].Loading = true;
        }
        Post("Reading the scene's objects", [this, scene] { DoExpandScene(scene); });
    }

    void UnityExplorer::ExpandNode(int node)
    {
        if (!Allowed())
            return;
        {
            std::lock_guard state(m_stateLock);
            if (node < 0 || node >= static_cast<int>(m_tree.Nodes.size()) || m_tree.Nodes[static_cast<std::size_t>(node)].ChildrenLoaded ||
                m_tree.Nodes[static_cast<std::size_t>(node)].Loading)
                return;
            m_tree.Nodes[static_cast<std::size_t>(node)].Loading = true;
        }
        Post("Reading child objects", [this, node] { DoExpandNode(node); });
    }

    namespace
    {
        struct NodeReply
        {
            int Object = 0;
            int Transform = 0;
            std::string Name;
            bool Active = false;
            int ChildCount = 0;
        };

        NodeReply ReadNodeReply(AgentCursor& in)
        {
            NodeReply node;
            node.Object = in.I32();
            node.Transform = in.I32();
            node.Name = in.Text();
            node.Active = in.U8() != 0;
            node.ChildCount = in.I32();
            return node;
        }
    }

    void UnityExplorer::DoReadScenes()
    {
        if (Il2())
            return Il2ReadScenes();
        {
            std::lock_guard state(m_stateLock);
            m_status.Error.clear();
        }
        if (Status ready = EnsureAgent(); !ready)
        {
            Fail(ready.Text);
            return;
        }
        ReleaseGroup(kTreeGroup);
        AgentRequest request;
        request.Op(AgentProtocol::Scenes).I32(kTreeGroup);
        const Result<AgentLink::Exchanged> answer = Ask(request);
        if (!answer)
        {
            Fail("Could not read the scenes: " + answer.Error().Text);
            return;
        }
        const AgentReply& reply = answer->Replies.front();
        if (!reply.Ok())
        {
            Fail("Could not read the scenes (this Unity version may be older than 5.3): " + reply.Error());
            return;
        }
        AgentCursor in(reply.Payload);
        const std::int32_t active = in.I32();
        const std::int32_t count = in.I32();
        UnityTree tree;
        tree.Read = true;
        for (std::int32_t i = 0; i < count && in.Ok(); ++i)
        {
            UnityScene scene;
            scene.Slot = in.I32();
            scene.Handle = in.I32();
            scene.Name = in.Text();
            scene.BuildIndex = in.I32();
            scene.RootCount = in.I32();
            scene.Loaded = in.U8() != 0;
            scene.Active = scene.Handle == active;
            if (scene.Name.empty())
                scene.Name = "?";
            tree.Scenes.push_back(std::move(scene));
        }
        if (!in.Ok())
        {
            Fail("The agent's scene list is damaged");
            return;
        }
        UnityScene dontDestroy;
        dontDestroy.Name = "DontDestroyOnLoad";
        dontDestroy.DontDestroy = true;
        dontDestroy.Handle = m_dontDestroyHandle;
        dontDestroy.Loaded = true;
        tree.Scenes.push_back(std::move(dontDestroy));
        {
            std::lock_guard state(m_stateLock);
            tree.Generation = m_tree.Generation + 1;
            m_tree = std::move(tree);
        }
        Publish();
    }

    void UnityExplorer::DoExpandScene(int sceneIndex)
    {
        if (Il2())
            return Il2ExpandScene(sceneIndex);
        UnityScene scene;
        std::uint64_t generation = 0;
        {
            std::lock_guard state(m_stateLock);
            if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_tree.Scenes.size()))
                return;
            scene = m_tree.Scenes[static_cast<std::size_t>(sceneIndex)];
            generation = m_tree.Generation;
        }
        const auto finish = [&](const std::string& error)
            {
                {
                    std::lock_guard state(m_stateLock);
                    if (m_tree.Generation == generation && sceneIndex < static_cast<int>(m_tree.Scenes.size()))
                    {
                        m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].Loading = false;
                        m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].RootsLoaded = error.empty();
                    }
                }
                if (!error.empty())
                    Fail(error);
                else
                    Publish();
            };
        if (Status ready = EnsureAgent(); !ready)
            return finish(ready.Text);
        AgentRequest request;
        request.Op(AgentProtocol::Roots).I32(kTreeGroup).I32(scene.DontDestroy ? 0 : scene.Slot).I32(static_cast<std::int32_t>(kMaxChildren));
        const Result<AgentLink::Exchanged> answer = Ask(request, std::chrono::seconds(30));
        if (!answer)
            return finish("Could not read the scene's objects: " + answer.Error().Text);
        const AgentReply& reply = answer->Replies.front();
        if (!reply.Ok())
            return finish("Could not read the scene's objects: " + reply.Error());
        AgentCursor in(reply.Payload);
        const int sceneSlot = in.I32();
        const int handle = in.I32();
        const std::int32_t total = in.I32();
        const std::int32_t count = in.I32();
        std::vector<NodeReply> replies;
        std::vector<int> slots;
        for (std::int32_t i = 0; i < count && in.Ok(); ++i)
        {
            replies.push_back(ReadNodeReply(in));
            slots.push_back(replies.back().Object);
            slots.push_back(replies.back().Transform);
        }
        if (!in.Ok())
            return finish("The agent's list of the scene's objects is damaged");
        LearnSlots(slots);
        if (scene.DontDestroy)
            m_dontDestroyHandle = handle;
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation != generation)
                return;
            UnityScene& target = m_tree.Scenes[static_cast<std::size_t>(sceneIndex)];
            target.Slot = sceneSlot;
            target.Handle = handle;
            target.RootCount = total;
            for (const NodeReply& reply : replies)
            {
                if (m_tree.Nodes.size() >= kMaxNodes)
                    break;
                UnityNode node;
                node.Object = m_slotAddress.count(reply.Object) ? m_slotAddress[reply.Object] : 0;
                node.Transform = m_slotAddress.count(reply.Transform) ? m_slotAddress[reply.Transform] : 0;
                node.Name = reply.Name.empty() ? std::string("?") : reply.Name;
                node.Active = reply.Active;
                node.ChildCount = reply.ChildCount;
                node.Scene = sceneIndex;
                if (!node.Object)
                    continue;
                target.Roots.push_back(static_cast<int>(m_tree.Nodes.size()));
                m_tree.Nodes.push_back(std::move(node));
            }
        }
        finish({});
    }

    void UnityExplorer::DoExpandNode(int nodeIndex)
    {
        if (Il2())
            return Il2ExpandNode(nodeIndex);
        UnityNode parent;
        std::uint64_t generation = 0;
        {
            std::lock_guard state(m_stateLock);
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_tree.Nodes.size()))
                return;
            parent = m_tree.Nodes[static_cast<std::size_t>(nodeIndex)];
            generation = m_tree.Generation;
        }
        std::string error;
        std::vector<NodeReply> replies;
        if (Status ready = EnsureAgent(); !ready)
            error = ready.Text;
        else if (const int transform = SlotOf(parent.Transform); !transform)
            error = "Manifold no longer holds this object; read the scenes again";
        else
        {
            AgentRequest request;
            request.Op(AgentProtocol::Children).I32(kTreeGroup).I32(transform).I32(static_cast<std::int32_t>(kMaxChildren));
            const Result<AgentLink::Exchanged> answer = Ask(request, std::chrono::seconds(30));
            if (!answer)
                error = "Could not read the child objects: " + answer.Error().Text;
            else if (!answer->Replies.front().Ok())
                error = "Could not read the child objects: " + answer->Replies.front().Error();
            else
            {
                AgentCursor in(answer->Replies.front().Payload);
                in.I32();
                const std::int32_t count = in.I32();
                std::vector<int> slots;
                for (std::int32_t i = 0; i < count && in.Ok(); ++i)
                {
                    replies.push_back(ReadNodeReply(in));
                    slots.push_back(replies.back().Object);
                    slots.push_back(replies.back().Transform);
                }
                if (!in.Ok())
                    error = "The agent's list of child objects is damaged";
                else
                    LearnSlots(slots);
            }
        }
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation == generation && nodeIndex < static_cast<int>(m_tree.Nodes.size()))
            {
                m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Loading = false;
                m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].ChildrenLoaded = error.empty();
                for (const NodeReply& reply : replies)
                {
                    if (m_tree.Nodes.size() >= kMaxNodes)
                        break;
                    UnityNode node;
                    node.Object = m_slotAddress.count(reply.Object) ? m_slotAddress[reply.Object] : 0;
                    node.Transform = m_slotAddress.count(reply.Transform) ? m_slotAddress[reply.Transform] : 0;
                    node.Name = reply.Name.empty() ? std::string("?") : reply.Name;
                    node.Active = reply.Active;
                    node.ChildCount = reply.ChildCount;
                    node.Parent = nodeIndex;
                    node.Scene = parent.Scene;
                    if (!node.Object)
                        continue;
                    m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Children.push_back(static_cast<int>(m_tree.Nodes.size()));
                    m_tree.Nodes.push_back(std::move(node));
                }
                m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].ChildCount = static_cast<int>(m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Children.size());
            }
        }
        if (!error.empty())
            Fail(error);
        else
            Publish();
    }

    // ---------------------------------------------------------------------
    // Search

    void UnityExplorer::Search(UnitySearch request)
    {
        if (!Allowed())
            return;
        Cancel();
        {
            std::lock_guard state(m_stateLock);
            m_search.Request = request;
            m_search.Hits.clear();
            m_search.Total = 0;
            m_search.Running = true;
            m_search.Done = false;
            m_search.HeapScan = false;
            m_search.Error.clear();
            ++m_search.Generation;
        }
        Publish();
        Post("Searching for " + request.ClassName, [this, request] { DoSearch(request); });
    }

    void UnityExplorer::DoSearch(UnitySearch request)
    {
        UnitySearchResult result;
        result.Request = request;
        const auto publish = [&](bool done)
            {
                std::lock_guard state(m_stateLock);
                result.Generation = m_search.Generation;
                result.Running = !done;
                result.Done = done;
                m_search = result;
            };
        std::uint64_t klass = FindClass(Trim(request.ClassName));
        if (!klass)
        {
            // A case-insensitive fallback over the enumerated classes.
            std::lock_guard bridge(m_mono.m_lock);
            for (const MonoClass& info : m_mono.m_classes)
            {
                if (EqualsNoCase(info.FullName(), Trim(request.ClassName)) || EqualsNoCase(info.Name, Trim(request.ClassName)))
                {
                    klass = info.Handle;
                    break;
                }
            }
        }
        if (!klass)
        {
            result.Error = "No class named " + request.ClassName;
            publish(true);
            Publish();
            return;
        }
        const std::string className = ClassName(klass);
        if (Il2() && IsUnityObjectClass(klass))
        {
            Il2Search(request, klass, result);
            publish(true);
            Publish();
            return;
        }
        if (IsUnityObjectClass(klass))
        {
            if (Status ready = EnsureAgent(); !ready)
            {
                result.Error = ready.Text;
                publish(true);
                Publish();
                return;
            }
            const auto [image, token] = ClassToken(klass);
            if (image.empty() || !token)
            {
                result.Error = "Could not name " + className + " to the agent";
                publish(true);
                Publish();
                return;
            }
            ReleaseGroup(kSearchGroup);
            AgentRequest find;
            find.Op(AgentProtocol::Find)
                .I32(kSearchGroup)
                .Text(image)
                .U32(token)
                .U8(request.IncludeHidden ? 1 : 0)
                .Text(Trim(request.NameFilter))
                .I32(static_cast<std::int32_t>(kMaxHits));
            const Result<AgentLink::Exchanged> answer = Ask(find, std::chrono::seconds(30));
            if (!answer || !answer->Replies.front().Ok())
            {
                result.Error = "The search failed: " + (answer ? answer->Replies.front().Error() : answer.Error().Text);
                publish(true);
                Publish();
                return;
            }
            AgentCursor in(answer->Replies.front().Payload);
            result.Total = static_cast<std::size_t>(std::max(0, in.I32()));
            const std::int32_t count = in.I32();
            std::vector<std::pair<int, std::string>> found;
            std::vector<int> slots;
            for (std::int32_t i = 0; i < count && in.Ok(); ++i)
            {
                const int slot = in.I32();
                found.emplace_back(slot, in.Text());
                slots.push_back(slot);
            }
            LearnSlots(slots);
            for (const auto& [slot, name] : found)
            {
                UnityHit hit;
                hit.Object = AddressOf(slot);
                if (!hit.Object)
                    continue;
                hit.Name = name.empty() ? std::string("?") : name;
                hit.Class = ClassNameOf(hit.Object);
                result.Hits.push_back(std::move(hit));
            }
            if (!in.Ok())
                result.Error = "The agent's search result is damaged";
            publish(true);
            Publish();
            return;
        }

        // Not a Unity object: find instances in the game's writable private
        // memory by walking each candidate's own header instead of asking the
        // runtime for the class's vtable -- mono_class_vtable would build it and
        // run the class's static constructor, and running foreign static
        // initialisers inside a frame is not worth a search. Every read here is
        // a plain memory read that simply fails on unmapped pages.
        result.HeapScan = true;
        const auto process = Proc();
        if (!process)
        {
            result.Error = "No process";
            publish(true);
            Publish();
            return;
        }
        constexpr std::uint64_t kChunk = 0x100000;
        constexpr std::size_t kMaxVTables = 64;
        const std::size_t pointer = static_cast<std::size_t>(process->PointerSize());
        std::vector<std::uint8_t> chunk;
        // Sweep once over a set of pointer-sized values and hand every hit's ADDRESS
        // to the visitor; an object header holds its vtable, a vtable holds its
        // class, both at offset 0, so one sweep shape serves both passes.
        const auto sweep = [&](const std::vector<std::uint64_t>& wanted, bool writableOnly, const std::function<bool(std::uint64_t)>& hit)
            {
                for (const RegionInfo& region : process->Regions(process->LowestAddress(), process->HighestAddress()))
                {
                    if (Aborted())
                        return;
                    if (!region.Committed() || !region.Readable() || region.Guarded())
                        continue;
                    if (writableOnly && (!region.Writable() || region.Type != MEM_PRIVATE))
                        continue;
                    for (std::uint64_t at = region.Base; at < region.End(); at += kChunk)
                    {
                        if (Aborted())
                            return;
                        const std::uint64_t size = std::min(kChunk, region.End() - at);
                        chunk.assign(static_cast<std::size_t>(size) / pointer * pointer, 0);
                        const std::size_t got = process->Read(at, chunk.data(), chunk.size()) / pointer;
                        for (std::size_t i = 0; i < got; ++i)
                        {
                            if (std::find(wanted.begin(), wanted.end(), LoadPointer(chunk.data() + i * pointer, pointer)) == wanted.end())
                                continue;
                            if (!hit(at + i * pointer))
                                return;
                        }
                    }
                }
            };

        std::vector<std::uint64_t> vtables;
        if (Il2())
            vtables.push_back(klass);
        else
            sweep({ klass }, false, [&](std::uint64_t address)
                {
                    vtables.push_back(address);
                    return vtables.size() < kMaxVTables;
                });
        if (vtables.empty())
        {
            result.Error = "Found no vtable for " + className + ", so its instances cannot be located in memory";
            publish(true);
            Publish();
            return;
        }
        sweep(vtables, true, [&](std::uint64_t object)
            {
                // Confirm through the object's own header; a stray qword that
                // merely held the same value is dropped here.
                if (ClassOf(object) != klass)
                    return true;
                if (Il2())
                {
                    const std::uint64_t monitor = process->ReadPointer(object + pointer).value_or(1);
                    if (monitor != 0 && (monitor < 0x10000 || (monitor & (pointer - 1)) != 0))
                        return true;
                }
                ++result.Total;
                if (result.Hits.size() < kMaxHits)
                    result.Hits.push_back({ object, std::string(), className });
                return result.Hits.size() < kMaxHits;
            });
        publish(true);
        Publish();
    }

    // ---------------------------------------------------------------------
    // Inspector

    int UnityExplorer::Inspect(std::uint64_t object, bool fromScan)
    {
        if (!Allowed())
            return 0;
        const std::uint64_t klass = ClassOf(object);
        const int slot = SlotOf(object);
        int id = 0;
        {
            std::lock_guard state(m_stateLock);
            id = m_nextInspection++;
            UnityInspection& inspection = m_inspections[id];
            inspection.Id = id;
            inspection.Object = object;
            inspection.Klass = klass;
            inspection.Unheld = !fromScan && object && !slot;
            inspection.FromScan = fromScan || inspection.Unheld;
            inspection.Loading = true;
            inspection.Title = Hex(object);
        }
        Publish();
        Post("Inspecting an object", [this, id, slot]
            {
                if (slot)
                    HoldSlots(InspectionGroup(id), { slot });
                DoInspect(id, true);
            });
        return id;
    }

    int UnityExplorer::InspectClass(std::uint64_t klass)
    {
        if (!Allowed())
            return 0;
        int id = 0;
        {
            std::lock_guard state(m_stateLock);
            id = m_nextInspection++;
            UnityInspection& inspection = m_inspections[id];
            inspection.Id = id;
            inspection.Klass = klass;
            inspection.Loading = true;
        }
        Publish();
        Post("Inspecting a class", [this, id] { DoInspect(id, true); });
        return id;
    }

    void UnityExplorer::Refresh(int id, bool properties)
    {
        {
            std::lock_guard state(m_stateLock);
            const auto found = m_inspections.find(id);
            if (found == m_inspections.end() || found->second.Loading)
                return;
            found->second.Loading = true;
        }
        Post("Refreshing the inspector", [this, id, properties] { DoInspect(id, properties); });
    }

    void UnityExplorer::Close(int id)
    {
        {
            std::lock_guard state(m_stateLock);
            m_inspections.erase(id);
        }
        Post("Closing an inspector", [this, id] { ReleaseGroup(InspectionGroup(id)); });
        Publish();
    }

    namespace
    {
        void ReadTransformReply(AgentCursor& in, UnityTransform& transform)
        {
            transform.Read = in.U8() != 0;
            float* const targets[] = { transform.LocalPosition, transform.LocalEuler, transform.LocalScale, transform.Position };
            for (float* target : targets)
            {
                for (int i = 0; i < 3; ++i)
                    target[i] = in.F32();
            }
        }
    }

    void UnityExplorer::DoInspect(int id, bool properties)
    {
        std::optional<UnityInspection> copy = Inspection(id);
        if (!copy)
            return;
        UnityInspection inspection = *copy;
        inspection.Error.clear();
        const auto started = std::chrono::steady_clock::now();
        m_statTrips = 0;
        m_statCalls = 0;
        m_statFrames = 0;
        if (!inspection.Klass && inspection.Object)
            inspection.Klass = ClassOf(inspection.Object);
        // Once inspected, the memory must still hold an object of that class.
        // After a scene change the game may have destroyed and collected it
        // and reused the memory; from then on nothing is called on it (and it
        // stays gone even if another object of the class lands there).
        if (!inspection.Gone && inspection.Object && inspection.Klass && ClassOf(inspection.Object) != inspection.Klass)
            inspection.Gone = true;
        if (inspection.Gone)
        {
            inspection.Values.clear();
            inspection.GameObject.reset();
            inspection.Transform.reset();
            inspection.Error = "This object is gone: the game destroyed it (a scene change does that), and its memory may already hold "
                               "something else, so nothing is read from it or called on it any more.";
        }
        if (!inspection.Klass)
            inspection.Error = "This address does not hold a managed object.";
        else
        {
            if (Il2())
                Il2RefreshStatics(inspection.Klass);
            inspection.Layout = LoadLayout(inspection.Klass);
            inspection.Title = inspection.Layout->Name;
        }

        const int group = InspectionGroup(id);
        const int self = inspection.Object ? SlotOf(inspection.Object) : 0;
        const bool callable = inspection.Layout && !inspection.FromScan && !inspection.Gone && (!inspection.Object || self);
        if (callable && Il2())
            Il2Inspect(inspection, properties, group);
        else if (callable)
        {
            const MonoClassLayout& layout = *inspection.Layout;
            AgentRequest request;
            int unityReply = -1;
            std::vector<std::size_t> getters;
            int getterFirst = -1;
            int fieldsReply = -1;
            std::vector<std::size_t> referenceFields;
            int elementsReply = -1;
            int next = 0;
            if (layout.UnityObject && inspection.Object)
            {
                if (layout.GameObject)
                    request.Op(AgentProtocol::GameObject).I32(group).I32(self).I32(static_cast<std::int32_t>(kMaxShownChildren));
                else
                    request.Op(AgentProtocol::UnityObject).I32(group).I32(self);
                unityReply = next++;
            }
            if (properties)
            {
                for (std::size_t i = 0; i < layout.Members.size(); ++i)
                {
                    const MonoMember& member = layout.Members[i];
                    if (member.Kind != MonoMemberKind::Property || !member.Getter || !member.GetterToken || (!member.Static && !inspection.Object))
                        continue;
                    if (getterFirst < 0)
                        getterFirst = next;
                    request.Op(AgentProtocol::Invoke)
                        .I32(group)
                        .Text(member.Image)
                        .U32(member.GetterToken)
                        .I32(member.Static ? 0 : self)
                        .I32(member.Type.ValueType ? std::max(1, member.Type.Size) : static_cast<int>(PointerBytes()))
                        .I32(0);
                    getters.push_back(i);
                    ++next;
                }
            }
            for (std::size_t i = 0; i < layout.Members.size(); ++i)
            {
                const MonoMember& member = layout.Members[i];
                if (member.Kind == MonoMemberKind::Field && !member.Literal && !member.Type.ValueType && member.Token && (member.Static || inspection.Object))
                    referenceFields.push_back(i);
            }
            if (!referenceFields.empty())
            {
                request.Op(AgentProtocol::Fields).I32(group).I32(self).I32(static_cast<std::int32_t>(referenceFields.size()));
                for (const std::size_t i : referenceFields)
                    request.Text(layout.Members[i].Image).U32(layout.Members[i].Token).I32(static_cast<int>(PointerBytes()));
                fieldsReply = next++;
            }
            if (layout.Rank > 0 && inspection.Object && !layout.Element.ValueType)
            {
                request.Op(AgentProtocol::Elements).I32(group).I32(self).I32(0).I32(static_cast<std::int32_t>(kMaxShownElements));
                elementsReply = next++;
            }
            if (request.Count() > 0)
            {
                if (Status ready = EnsureAgent(); !ready)
                    inspection.Error = ready.Text;
                else
                {
                    const Result<AgentLink::Exchanged> answer = Ask(request);
                    if (!answer)
                        inspection.Error = answer.Error().Text;
                    else
                    {
                        const std::vector<AgentReply>& replies = answer->Replies;
                        std::vector<int> slots;
                        struct GameObjectReply
                        {
                            UnityGameObject Info;
                            int Transform = 0;
                            int Parent = 0;
                            std::vector<int> Children;
                            std::vector<int> Components;
                        };
                        std::optional<GameObjectReply> gameObject;
                        int owner = 0;
                        std::optional<UnityTransform> transform;
                        if (unityReply >= 0)
                        {
                            const AgentReply& reply = replies[static_cast<std::size_t>(unityReply)];
                            AgentCursor in(reply.Payload);
                            if (!reply.Ok())
                                inspection.Error = reply.Error();
                            else if (layout.GameObject)
                            {
                                GameObjectReply got;
                                got.Info.Name = in.Text();
                                got.Info.Tag = in.Text();
                                got.Info.Layer = in.I32();
                                got.Info.ActiveSelf = in.U8() != 0;
                                got.Info.ActiveInHierarchy = in.U8() != 0;
                                got.Info.Scene = in.Text();
                                got.Transform = in.I32();
                                ReadTransformReply(in, got.Info.Transform);
                                got.Parent = in.I32();
                                got.Info.ParentName = in.Text();
                                got.Info.ChildCount = in.I32();
                                const std::int32_t children = in.I32();
                                for (std::int32_t k = 0; k < children && in.Ok(); ++k)
                                {
                                    got.Children.push_back(in.I32());
                                    got.Info.Children.push_back({ 0, in.Text(), "GameObject" });
                                }
                                const std::int32_t components = in.I32();
                                for (std::int32_t k = 0; k < components && in.Ok(); ++k)
                                {
                                    UnityComponent entry;
                                    got.Components.push_back(in.I32());
                                    entry.HasEnabled = in.U8() != 0;
                                    entry.Enabled = in.U8() != 0;
                                    got.Info.Components.push_back(std::move(entry));
                                }
                                if (in.Ok())
                                {
                                    inspection.ObjectName = got.Info.Name;
                                    slots.push_back(got.Transform);
                                    slots.push_back(got.Parent);
                                    slots.insert(slots.end(), got.Children.begin(), got.Children.end());
                                    slots.insert(slots.end(), got.Components.begin(), got.Components.end());
                                    gameObject = std::move(got);
                                }
                                else
                                    inspection.Error = "The agent's answer about this GameObject is damaged";
                            }
                            else
                            {
                                inspection.ObjectName = in.Text();
                                owner = in.I32();
                                inspection.OwnerName = in.Text();
                                const std::uint8_t enabled = in.U8();
                                UnityTransform read;
                                ReadTransformReply(in, read);
                                if (in.Ok())
                                {
                                    if (enabled < 2)
                                        inspection.Enabled = enabled != 0;
                                    if (read.Read)
                                        transform = read;
                                    slots.push_back(owner);
                                }
                                else
                                    inspection.Error = "The agent's answer about this object is damaged";
                            }
                        }
                        std::vector<std::pair<std::size_t, PendingValue>> values;
                        for (std::size_t k = 0; k < getters.size(); ++k)
                        {
                            const AgentReply& reply = replies[static_cast<std::size_t>(getterFirst) + k];
                            const MonoMember& member = layout.Members[getters[k]];
                            PendingValue pending;
                            if (!reply.Ok())
                                pending.Value.Error = reply.Error();
                            else
                            {
                                AgentCursor in(reply.Payload);
                                pending = ReadValue(in, member.Type);
                                if (pending.Reference)
                                    slots.push_back(pending.Slot);
                            }
                            values.emplace_back(getters[k], std::move(pending));
                        }
                        if (fieldsReply >= 0 && replies[static_cast<std::size_t>(fieldsReply)].Ok())
                        {
                            AgentCursor in(replies[static_cast<std::size_t>(fieldsReply)].Payload);
                            for (const std::size_t i : referenceFields)
                            {
                                const PendingValue pending = ReadValue(in, layout.Members[i].Type);
                                if (pending.Reference)
                                    slots.push_back(pending.Slot);
                            }
                        }
                        if (elementsReply >= 0 && replies[static_cast<std::size_t>(elementsReply)].Ok())
                        {
                            AgentCursor in(replies[static_cast<std::size_t>(elementsReply)].Payload);
                            in.I32();
                            const std::int32_t count = in.I32();
                            for (std::int32_t k = 0; k < count && in.Ok(); ++k)
                                slots.push_back(in.I32());
                        }
                        LearnSlots(slots);
                        if (gameObject)
                        {
                            UnityGameObject info = std::move(gameObject->Info);
                            info.Object = inspection.Object;
                            info.Transform.Object = AddressOf(gameObject->Transform);
                            info.Parent = AddressOf(gameObject->Parent);
                            if (!info.Parent)
                                info.ParentName.clear();
                            else if (info.ParentName.empty())
                                info.ParentName = "?";
                            for (std::size_t k = 0; k < info.Children.size(); ++k)
                            {
                                info.Children[k].Object = AddressOf(gameObject->Children[k]);
                                if (info.Children[k].Name.empty())
                                    info.Children[k].Name = "?";
                            }
                            info.Children.erase(std::remove_if(info.Children.begin(), info.Children.end(), [](const UnityHit& hit) { return !hit.Object; }), info.Children.end());
                            for (std::size_t k = 0; k < info.Components.size(); ++k)
                            {
                                info.Components[k].Object = AddressOf(gameObject->Components[k]);
                                info.Components[k].Class = ClassNameOf(info.Components[k].Object);
                            }
                            info.Components.erase(std::remove_if(info.Components.begin(), info.Components.end(), [](const UnityComponent& entry) { return !entry.Object; }), info.Components.end());
                            inspection.GameObject = std::move(info);
                        }
                        if (unityReply >= 0 && !layout.GameObject)
                        {
                            inspection.Owner = AddressOf(owner);
                            if (!inspection.Owner)
                                inspection.OwnerName.clear();
                            if (transform)
                            {
                                transform->Object = inspection.Object;
                                inspection.Transform = transform;
                            }
                        }
                        if (!getters.empty())
                        {
                            std::map<int, MonoReadValue> kept;
                            for (const auto& [index, value] : inspection.Values)
                            {
                                if (index >= 0 && index < static_cast<int>(layout.Members.size()) &&
                                    layout.Members[static_cast<std::size_t>(index)].Kind == MonoMemberKind::Method)
                                    kept.emplace(index, value);
                            }
                            for (auto& [index, pending] : values)
                            {
                                Resolve(pending);
                                kept[static_cast<int>(index)] = std::move(pending.Value);
                            }
                            inspection.Values = std::move(kept);
                        }
                    }
                }
            }
        }
        inspection.Loading = false;
        inspection.RefreshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        inspection.Calls = m_statCalls;
        inspection.RoundTrips = m_statTrips;
        inspection.FrameWaits = m_statFrames;
        {
            std::lock_guard state(m_stateLock);
            const auto found = m_inspections.find(id);
            if (found != m_inspections.end())
            {
                inspection.Generation = found->second.Generation + 1;
                found->second = std::move(inspection);
            }
        }
        Publish();
    }

    // ---------------------------------------------------------------------
    // Edits

    void UnityExplorer::SetActive(std::uint64_t gameObject, bool active, int refresh)
    {
        Post(active ? "Activating an object" : "Deactivating an object", [this, gameObject, active, refresh]
            {
                if (Status ready = Il2() ? Il2EnsureHook() : EnsureAgent(); !ready)
                    return Fail(ready.Text);
                const int slot = SlotOf(gameObject);
                if (!slot)
                    return Fail(std::string("SetActive: ") + kUnheldText);
                if (Il2())
                {
                    AgentArg value;
                    value.Kind = AgentProtocol::ArgBytes;
                    value.Bytes = { static_cast<std::uint8_t>(active ? 1 : 0) };
                    if (Status done = Il2Edit("SetActive failed", Il2Method("UnityEngine.GameObject", "SetActive", { "System.Boolean" }), gameObject, { value }); !done)
                        return Fail(done.Text);
                }
                else
                {
                    AgentRequest request;
                    request.Op(AgentProtocol::SetActive).I32(slot).U8(active ? 1 : 0);
                    if (Status done = Command(request, "SetActive failed"); !done)
                        return Fail(done.Text);
                }
                {
                    std::lock_guard state(m_stateLock);
                    for (UnityNode& node : m_tree.Nodes)
                    {
                        if (node.Object == gameObject)
                            node.Active = active;
                    }
                }
                if (refresh)
                    DoInspect(refresh, false);
                Publish();
            });
    }

    void UnityExplorer::SetEnabled(std::uint64_t behaviour, bool enabled, int refresh)
    {
        Post(enabled ? "Enabling a component" : "Disabling a component", [this, behaviour, enabled, refresh]
            {
                if (Status ready = Il2() ? Il2EnsureHook() : EnsureAgent(); !ready)
                    return Fail(ready.Text);
                const int slot = SlotOf(behaviour);
                if (!slot)
                    return Fail(std::string("Setting enabled: ") + kUnheldText);
                if (Il2())
                {
                    AgentArg value;
                    value.Kind = AgentProtocol::ArgBytes;
                    value.Bytes = { static_cast<std::uint8_t>(enabled ? 1 : 0) };
                    if (Status done = Il2Edit("Setting enabled failed", Il2Method("UnityEngine.Behaviour", "set_enabled", { "System.Boolean" }), behaviour, { value }); !done)
                        return Fail(done.Text);
                }
                else
                {
                    AgentRequest request;
                    request.Op(AgentProtocol::SetEnabled).I32(slot).U8(enabled ? 1 : 0);
                    if (Status done = Command(request, "Setting enabled failed"); !done)
                        return Fail(done.Text);
                }
                if (refresh)
                    DoInspect(refresh, false);
            });
    }

    void UnityExplorer::SetName(std::uint64_t object, std::string name, int refresh)
    {
        Post("Renaming an object", [this, object, name, refresh]
            {
                if (Status ready = Il2() ? Il2EnsureHook() : EnsureAgent(); !ready)
                    return Fail(ready.Text);
                const int slot = SlotOf(object);
                if (!slot)
                    return Fail(std::string("Renaming: ") + kUnheldText);
                if (Il2())
                {
                    AgentArg value;
                    value.Kind = AgentProtocol::ArgText;
                    value.Text = name;
                    if (Status done = Il2Edit("Renaming failed", Il2Method("UnityEngine.Object", "set_name", { "System.String" }), object, { value }); !done)
                        return Fail(done.Text);
                }
                else
                {
                    AgentRequest request;
                    request.Op(AgentProtocol::SetName).I32(slot).Text(name);
                    if (Status done = Command(request, "Renaming failed"); !done)
                        return Fail(done.Text);
                }
                {
                    std::lock_guard state(m_stateLock);
                    for (UnityNode& node : m_tree.Nodes)
                    {
                        if (node.Object == object)
                            node.Name = name;
                    }
                }
                if (refresh)
                    DoInspect(refresh, false);
                Publish();
            });
    }

    void UnityExplorer::SetVector(std::uint64_t transform, int which, const float value[3], int refresh)
    {
        const std::array<float, 3> vector{ value[0], value[1], value[2] };
        Post("Moving an object", [this, transform, which, vector, refresh]
            {
                if (Status ready = Il2() ? Il2EnsureHook() : EnsureAgent(); !ready)
                    return Fail(ready.Text);
                const int slot = SlotOf(transform);
                if (!slot)
                    return Fail(std::string("Moving: ") + kUnheldText);
                if (Il2())
                {
                    static const char* const kSetters[] = { "set_localPosition", "set_localEulerAngles", "set_localScale" };
                    AgentArg value;
                    value.Kind = AgentProtocol::ArgBytes;
                    value.Bytes.resize(sizeof(float) * 3);
                    std::memcpy(value.Bytes.data(), vector.data(), value.Bytes.size());
                    if (Status done = Il2Edit("Moving failed", Il2Method("UnityEngine.Transform", kSetters[std::clamp(which, 0, 2)], { "UnityEngine.Vector3" }), transform, { value }); !done)
                        return Fail(done.Text);
                }
                else
                {
                    AgentRequest request;
                    request.Op(AgentProtocol::SetVector).I32(slot).I32(std::clamp(which, 0, 2)).F32(vector[0]).F32(vector[1]).F32(vector[2]);
                    if (Status done = Command(request, "Moving failed"); !done)
                        return Fail(done.Text);
                }
                if (refresh)
                    DoInspect(refresh, false);
            });
    }

    void UnityExplorer::SetRunInBackground(bool enabled)
    {
        Post(enabled ? "Keeping the game running in the background" : "Letting the game pause in the background", [this, enabled]
            {
                // runInBackground is a real engine property, so the agent sets
                // it on the main thread. If the game is not rendering (so the
                // main thread never reaches the agent) this cannot be set; the
                // game has to be in the foreground once to turn it on.
                if (Il2())
                {
                    if (Status done = Il2SetRunInBackground(enabled); !done)
                        Fail(done.Text);
                    return;
                }
                if (Status ready = EnsureAgent(); !ready)
                    return Fail(ready.Text);
                AgentRequest request;
                request.Op(AgentProtocol::RunInBackground).I32(enabled ? 1 : 0);
                const Result<AgentLink::Exchanged> answer = Ask(request, std::chrono::seconds(5));
                if (!answer)
                    return Fail("Bring the game to the foreground once so this can run on its main thread, then try again (" + answer.Error().Text + ")");
                const AgentReply& reply = answer->Replies.front();
                if (!reply.Ok())
                    return Fail("Could not change Application.runInBackground: " + reply.Error());
                AgentCursor in(reply.Payload);
                const std::int32_t now = in.I32();
                const bool changed = in.U8() != 0;
                std::lock_guard state(m_stateLock);
                m_status.RunInBackground = now >= 0 ? now != 0 : enabled;
                m_status.ChangedRunInBackground = changed;
            });
    }

    std::optional<UnityExplorer::AgentArg> UnityExplorer::ArgumentFor(const MonoTypeInfo& type, const std::string& text, std::string& error)
    {
        AgentArg arg;
        if (type.Code == MonoTypeCode::ByRef)
        {
            error = "a ref or out value cannot be passed from here";
            return std::nullopt;
        }
        if (type.ValueType)
        {
            if (type.Class && !IsPrimitive(type.ValueCode))
                LoadLayout(type.Class);
            const auto bytes = Parse(type, text, &error);
            if (!bytes)
                return std::nullopt;
            arg.Kind = AgentProtocol::ArgBytes;
            arg.Bytes = *bytes;
            return arg;
        }
        const std::string trimmed = Trim(text);
        if (type.Code == MonoTypeCode::String)
        {
            if (trimmed == "null")
                return arg;
            std::string body = text;
            if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                body = body.substr(1, body.size() - 2);
            arg.Kind = AgentProtocol::ArgText;
            arg.Text = body;
            return arg;
        }
        if (trimmed.empty() || trimmed == "null")
            return arg;
        std::string digits = trimmed;
        if (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0)
            digits = digits.substr(2);
        const auto address = ParseHex(digits);
        if (!address || !ClassOf(*address))
        {
            error = "type null, or the address of an object";
            return std::nullopt;
        }
        arg.Slot = SlotOf(*address);
        if (!arg.Slot)
        {
            error = "Manifold holds no object at that address; open the object in the explorer first";
            return std::nullopt;
        }
        arg.Kind = AgentProtocol::ArgSlot;
        return arg;
    }

    void UnityExplorer::SetMember(int id, int member, std::string text)
    {
        Post("Changing a value", [this, id, member, text] { DoSetMember(id, member, text); });
    }

    void UnityExplorer::DoSetMember(int id, int memberIndex, const std::string& text)
    {
        const std::optional<UnityInspection> inspection = Inspection(id);
        if (!inspection || !inspection->Layout || memberIndex < 0 || memberIndex >= static_cast<int>(inspection->Layout->Members.size()))
            return;
        const MonoMember& member = inspection->Layout->Members[static_cast<std::size_t>(memberIndex)];
        const auto process = Proc();
        if (!process)
            return;
        std::string error;
        // Writing into an object the game has collected would overwrite
        // whatever now lives in that memory.
        if (!member.Static && inspection->Object && (inspection->Gone || ClassOf(inspection->Object) != inspection->Klass))
            return Fail(member.Name + ": " + kGoneText);
        if (member.Kind == MonoMemberKind::Field && member.Type.ValueType)
        {
            if (member.Literal)
                return Fail(member.Name + " is a constant");
            if (!member.Static && !inspection->Object)
                return Fail(member.Name + " needs an object");
            if (member.Type.Class && !IsPrimitive(member.Type.ValueCode))
                LoadLayout(member.Type.Class);
            const auto bytes = Parse(member.Type, text, &error);
            if (!bytes)
                return Fail(member.Name + ": " + error);
            if (member.Static && (!member.StaticData || member.Offset < 0))
                return Fail(member.Name + " is a thread static and cannot be written here");
            const std::uint64_t address = member.Static ? member.StaticData + static_cast<std::uint64_t>(member.Offset)
                                                        : inspection->Object + static_cast<std::uint64_t>(member.Offset);
            if (Status written = process->Write(address, bytes->data(), bytes->size()); !written)
                return Fail(member.Name + ": " + written.Text);
            DoInspect(id, false);
            return;
        }
        if (member.Kind != MonoMemberKind::Field && member.Kind != MonoMemberKind::Property)
            return;
        if (member.Kind == MonoMemberKind::Field && member.Literal)
            return Fail(member.Name + " is a constant");
        if (member.Kind == MonoMemberKind::Property && (!member.Setter || (!member.SetterToken && !Il2())))
            return Fail(member.Name + " has no setter");
        if (!member.Static && !inspection->Object)
            return Fail(member.Name + " needs an object");
        const int self = member.Static ? 0 : SlotOf(inspection->Object);
        if (!member.Static && !self)
            return Fail(member.Name + ": " + kUnheldText);
        if (Status ready = Il2() ? Il2EnsureHook() : EnsureAgent(); !ready)
            return Fail(ready.Text);
        const auto arg = ArgumentFor(member.Type, text, error);
        if (!arg)
            return Fail(member.Name + ": " + error);
        if (Il2())
        {
            if (!Il2Fits(member.Type, *arg, error))
                return Fail(member.Name + ": " + error);
            const Status done = member.Kind == MonoMemberKind::Field
                ? Il2SetField(*inspection, member, *arg)
                : Il2Edit("Setting " + member.Name + " failed", member.Setter, member.Static ? 0 : inspection->Object, { *arg });
            if (!done)
                return Fail(done.Text);
            DoInspect(id, member.Kind == MonoMemberKind::Property);
            return;
        }
        AgentRequest request;
        if (member.Kind == MonoMemberKind::Field)
        {
            request.Op(AgentProtocol::SetField).I32(self).Text(member.Image).U32(member.Token);
            WriteArg(request, *arg);
        }
        else
        {
            request.Op(AgentProtocol::Invoke).I32(InspectionGroup(id)).Text(member.Image).U32(member.SetterToken).I32(self).I32(0).I32(1);
            WriteArg(request, *arg);
        }
        if (Status done = Command(request, "Setting " + member.Name + " failed"); !done)
            return Fail(done.Text);
        DoInspect(id, member.Kind == MonoMemberKind::Property);
    }

    void UnityExplorer::InvokeMethod(int id, int member, std::vector<std::string> arguments)
    {
        Post("Calling a method", [this, id, member, arguments] { DoInvokeMethod(id, member, arguments); });
    }

    void UnityExplorer::DoInvokeMethod(int id, int memberIndex, const std::vector<std::string>& arguments)
    {
        const std::optional<UnityInspection> inspection = Inspection(id);
        if (!inspection || !inspection->Layout || memberIndex < 0 || memberIndex >= static_cast<int>(inspection->Layout->Members.size()))
            return;
        const MonoMember& member = inspection->Layout->Members[static_cast<std::size_t>(memberIndex)];
        if (member.Kind != MonoMemberKind::Method)
            return;
        if (!member.Static && !inspection->Object)
            return Fail(member.Name + " needs an object");
        MonoReadValue value;
        std::vector<AgentArg> args;
        std::string error;
        const int self = member.Static ? 0 : SlotOf(inspection->Object);
        if (!member.Static && !self)
            error = kUnheldText;
        if (!member.Token && !Il2())
            error = "this runtime does not name the method's token";
        for (std::size_t i = 0; i < member.Params.size() && error.empty(); ++i)
        {
            const auto arg = ArgumentFor(member.Params[i], i < arguments.size() ? arguments[i] : std::string(), error);
            if (arg)
                args.push_back(*arg);
            else
                error = (i < member.ParamNames.size() && !member.ParamNames[i].empty() ? member.ParamNames[i] : "argument " + std::to_string(i + 1)) + ": " + error;
        }
        if (!error.empty())
            value.Error = error;
        else if (Il2())
            value = Il2Invoke(*inspection, member, args, InspectionGroup(id));
        else if (Status ready = EnsureAgent(); !ready)
            value.Error = ready.Text;
        else
        {
            AgentRequest request;
            request.Op(AgentProtocol::Invoke)
                .I32(InspectionGroup(id))
                .Text(member.Image)
                .U32(member.Token)
                .I32(self)
                .I32(member.Type.ValueType ? std::max(1, member.Type.Size) : static_cast<int>(PointerBytes()))
                .I32(static_cast<std::int32_t>(args.size()));
            for (const AgentArg& arg : args)
                WriteArg(request, arg);
            const Result<AgentLink::Exchanged> answer = Ask(request, std::chrono::seconds(30));
            if (!answer)
                value.Error = answer.Error().Text;
            else if (!answer->Replies.front().Ok())
                value.Error = answer->Replies.front().Error();
            else
            {
                AgentCursor in(answer->Replies.front().Payload);
                PendingValue pending = ReadValue(in, member.Type);
                if (pending.Reference)
                    LearnSlots({ pending.Slot });
                Resolve(pending);
                value = std::move(pending.Value);
                LOG_INFO("Unity", "Called " + member.Declaring + ":" + member.Name);
            }
        }
        {
            std::lock_guard state(m_stateLock);
            if (const auto found = m_inspections.find(id); found != m_inspections.end())
            {
                found->second.Values[memberIndex] = value;
                ++found->second.Generation;
            }
        }
        Publish();
    }
}
