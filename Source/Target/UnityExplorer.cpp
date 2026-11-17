#include "Target/UnityExplorer.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::size_t kArgsSize = 0x10000;
        constexpr auto kVisitBudget = std::chrono::milliseconds(4);
        constexpr std::size_t kMaxNodes = 200000;
        constexpr std::size_t kMaxChildren = 20000;
        constexpr std::size_t kMaxHits = 5000;
        constexpr std::size_t kMaxArray = 200000;
        constexpr std::size_t kMaxShownChildren = 500;
        constexpr int kMaxMembers = 4096;
        constexpr std::uint32_t kFieldStatic = 0x10;
        constexpr std::uint32_t kFieldLiteral = 0x40;
        constexpr std::uint32_t kMethodStatic = 0x10;
        constexpr std::uint64_t kObjectHeader = 0x10;
        constexpr std::uint64_t kStringLength = 0x10;
        constexpr std::uint64_t kStringChars = 0x14;
        constexpr std::uint64_t kArrayLength = 0x18;
        constexpr std::uint64_t kArrayData = 0x20;

        // Set on the explorer's job thread, the only thread that may make
        // remote calls for names and layouts; drawing code only reads caches.
        thread_local bool t_jobThread = false;

        struct Vector3
        {
            float X = 0.0f, Y = 0.0f, Z = 0.0f;
        };

        int PrimitiveSize(int code)
        {
            switch (code)
            {
            case MonoTypeCode::Boolean: case MonoTypeCode::I1: case MonoTypeCode::U1: return 1;
            case MonoTypeCode::Char: case MonoTypeCode::I2: case MonoTypeCode::U2: return 2;
            case MonoTypeCode::I4: case MonoTypeCode::U4: case MonoTypeCode::R4: return 4;
            case MonoTypeCode::I8: case MonoTypeCode::U8: case MonoTypeCode::R8: return 8;
            case MonoTypeCode::I: case MonoTypeCode::U: case MonoTypeCode::Ptr: case MonoTypeCode::FnPtr: return 8;
            default: return 0;
            }
        }

        bool IsPrimitive(int code)
        {
            return PrimitiveSize(code) > 0;
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

        bool ParsePrimitive(int code, const std::string& text, std::uint8_t* out, std::string* error)
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
            const int size = PrimitiveSize(code);
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
            if (!m_mono.m_present || !m_mono.m_process)
                return;
            std::lock_guard state(m_stateLock);
            m_process = m_mono.m_process;
        }
        std::lock_guard lock(m_jobLock);
        if (m_stopping)
            return;
        m_jobs.emplace_back(std::move(task), std::move(job));
        if (!m_thread.joinable())
            m_thread = std::thread([this] { Loop(); });
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
                m_jobWake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_stopping)
                    return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                m_jobAbort = m_abort.load();
            }
            m_mono.m_call.InterruptMain(false);
            {
                std::lock_guard state(m_stateLock);
                m_status.Busy = true;
                m_status.Task = job.first;
            }
            Publish();
            try
            {
                job.second();
            }
            catch (const std::exception& error)
            {
                Fail(std::string("internal error: ") + error.what());
            }
            if (m_visiting)
            {
                m_mono.m_call.LeaveMain();
                m_visiting = false;
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
        bool running = false;
        {
            std::lock_guard lock(m_jobLock);
            m_jobs.clear();
            running = m_thread.joinable();
        }
        ++m_abort;
        if (running)
            m_mono.m_call.InterruptMain(true);
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
        m_mono.m_call.InterruptMain(true);
        if (m_thread.joinable())
            m_thread.join();
        m_thread = std::thread();
        m_mono.m_call.InterruptMain(false);

        // The worker still runs here (the bridge stops it afterwards), so the
        // callback can be taken out of the game and the pins freed.
        bool present = false;
        {
            std::lock_guard bridge(m_mono.m_lock);
            present = m_mono.m_present && m_mono.m_process && m_mono.m_process->Alive();
        }
        if (present && m_hooked)
        {
            // Everything that touches onBeforeRender's subscriber list, above all
            // taking our own callback back out, MUST run on the game's main
            // thread: that is the thread that iterates the list every frame to
            // call us, so running there means no other thread mutates the list
            // while it is being walked. Removing it from the worker corrupted the
            // list and crashed the game (the whole reason detach was unsafe).
            const bool entered = m_mono.m_call.EnterMain(std::chrono::milliseconds(2000)).Ok();
            if (m_originalRunInBackground)
            {
                const std::uint64_t setter = Method("UnityEngine.Application", "set_runInBackground", 1);
                const bool original = *m_originalRunInBackground;
                const bool restored = setter && entered && Invoke(true, setter, Arg::Ref(0), { Arg::Value<std::uint8_t>(original ? 1 : 0) }).Ok;
                if (!restored)
                    LOG_WARN("Unity", "Could not set Application.runInBackground back; it stays until the game restarts");
            }
            if (entered)
                Unhook();
            else
                LOG_WARN("Unity", "The game's main thread did not run during detach; the callback was left in (it is harmless and returns at once)");
            m_mono.m_call.LeaveMain();
        }
        if (present)
        {
            Unpin(m_treePins);
            Unpin(m_searchPins);
            Unpin(m_keepPins);
            for (auto& entry : m_inspectionPins)
                Unpin(entry.second);
            // m_args is deliberately NOT freed: its addresses were passed to
            // the runtime as argument and out-parameter pointers and still sit
            // in the attached worker's registers and stack frames, which the
            // game's garbage collector scans conservatively. Freeing it made
            // the next collection read a block header from an unmapped page and
            // kill the game. See the note in RemoteCall::Stop.
        }
        m_visiting = false;
        m_args = 0;
        m_argsUsed = 0;
        m_hooked = false;
        m_delegate = 0;
        m_delegatePin = 0;
        m_unhook = 0;
        m_dontDestroyHandle = 0;
        m_originalRunInBackground.reset();
        m_methods.clear();
        m_typeObjects.clear();
        m_types.clear();
        m_chains.clear();
        m_treePins.clear();
        m_searchPins.clear();
        m_keepPins.clear();
        m_inspectionPins.clear();
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

    std::optional<std::uint64_t> UnityExplorer::Worker(const char* function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        std::lock_guard bridge(m_mono.m_lock);
        if (!m_mono.m_present)
            return std::nullopt;
        return m_mono.CallFn(function, a0, a1, a2, a3);
    }

    std::optional<std::uint64_t> UnityExplorer::Main(const char* function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        std::uint64_t fn = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_present)
                return std::nullopt;
            fn = m_mono.Fn(function);
        }
        if (!fn)
            return std::nullopt;
        if (!m_visiting || !m_mono.m_call.InsideMain())
        {
            m_visitStart = std::chrono::steady_clock::now();
            m_visiting = true;
        }
        std::uint64_t result = 0;
        const Status status = m_mono.m_call.CallMain(fn, a0, a1, a2, a3, result);
        if (!status)
        {
            m_visiting = false;
            std::lock_guard state(m_stateLock);
            m_status.Error = status.Text;
            return std::nullopt;
        }
        return result;
    }

    std::optional<std::uint64_t> UnityExplorer::Exec(const char* function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        // Once the per-frame callback is in, run on the game's main thread (Main
        // enters it as needed); only before the hook exists, or on a non-Unity
        // Mono game that has no such event, does this fall back to the worker.
        if (m_hooked)
            return Main(function, a0, a1, a2, a3);
        return Worker(function, a0, a1, a2, a3);
    }

    void UnityExplorer::YieldFrame()
    {
        // Give the frame back to the game once this visit used its budget; the
        // next main-thread call waits for the following frame.
        if (m_visiting && std::chrono::steady_clock::now() - m_visitStart > kVisitBudget)
        {
            m_mono.m_call.LeaveMain();
            m_visiting = false;
        }
    }

    std::uint64_t UnityExplorer::ArgSpace(std::size_t bytes)
    {
        if (!m_args)
        {
            const auto process = Proc();
            if (!process)
                return 0;
            Status status;
            m_args = process->Allocate(kArgsSize, PAGE_READWRITE, 0, status);
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

    UnityExplorer::Invoked UnityExplorer::Invoke(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params)
    {
        Invoked out;
        const auto process = Proc();
        if (!method || !process)
        {
            out.Error = method ? "no process" : "the method was not found in this Unity version";
            return out;
        }
        m_argsUsed = 0;
        const std::uint64_t exception = ArgSpace(8);
        const std::uint64_t array = params.empty() ? 0 : ArgSpace(8 * params.size());
        if (!exception || (!params.empty() && !array))
        {
            out.Error = "no room for the arguments";
            return out;
        }
        std::vector<std::uint64_t> pointers;
        const auto place = [&](const Arg& arg) -> std::uint64_t
            {
                if (arg.Bytes.empty())
                    return arg.Pointer;
                const std::uint64_t at = ArgSpace(arg.Bytes.size());
                if (at)
                    process->Write(at, arg.Bytes.data(), arg.Bytes.size());
                return at;
            };
        for (const Arg& param : params)
            pointers.push_back(place(param));
        const std::uint64_t selfPointer = place(self);
        const std::uint64_t zero = 0;
        process->Write(exception, &zero, sizeof(zero));
        if (array)
            process->Write(array, pointers.data(), pointers.size() * 8);
        const auto result = main ? Main("mono_runtime_invoke", method, selfPointer, array, exception)
                                 : Worker("mono_runtime_invoke", method, selfPointer, array, exception);
        if (!result)
        {
            std::lock_guard state(m_stateLock);
            out.Error = m_status.Error.empty() ? "the call did not run" : m_status.Error;
            return out;
        }
        std::uint64_t thrown = 0;
        process->ReadExact(exception, &thrown, sizeof(thrown));
        if (thrown)
        {
            out.Error = ExceptionText(main, thrown);
            return out;
        }
        out.Ok = true;
        out.Result = *result;
        return out;
    }

    std::string UnityExplorer::ExceptionText(bool main, std::uint64_t exception)
    {
        std::string name = ClassNameOf(exception);
        if (name.empty())
            name = "an exception";
        const auto process = Proc();
        const std::uint64_t slot = ArgSpace(8);
        if (!process || !slot)
            return name;
        const std::uint64_t zero = 0;
        process->Write(slot, &zero, sizeof(zero));
        const auto text = main ? Main("mono_object_to_string", exception, slot) : Worker("mono_object_to_string", exception, slot);
        std::uint64_t nested = 0;
        process->ReadExact(slot, &nested, sizeof(nested));
        if (!text || !*text || nested)
            return name;
        std::string message = ReadString(*text, 2048);
        if (const std::size_t line = message.find_first_of("\r\n"); line != std::string::npos)
            message.resize(line);
        return message.empty() ? name : message;
    }

    std::string UnityExplorer::ReadString(std::uint64_t monoString, std::size_t limit) const
    {
        const auto process = Proc();
        if (!process || !monoString)
            return {};
        std::int32_t length = 0;
        if (!process->ReadExact(monoString + kStringLength, &length, sizeof(length)) || length <= 0)
            return {};
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), limit);
        std::wstring wide(count, L'\0');
        const std::size_t got = process->Read(monoString + kStringChars, wide.data(), count * 2);
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
        std::uint64_t length = 0;
        if (!process->ReadExact(monoArray + kArrayLength, &length, sizeof(length)) || length > 0x10000000ull)
            return items;
        if (total)
            *total = static_cast<std::size_t>(length);
        items.resize(static_cast<std::size_t>(std::min<std::uint64_t>(length, limit)));
        if (!items.empty() && !process->ReadExact(monoArray + kArrayData, items.data(), items.size() * 8))
            items.clear();
        return items;
    }

    template <typename T>
    std::optional<T> UnityExplorer::Unbox(std::uint64_t boxed) const
    {
        const auto process = Proc();
        if (!process || !boxed)
            return std::nullopt;
        T value{};
        if (!process->ReadExact(boxed + kObjectHeader, &value, sizeof(T)))
            return std::nullopt;
        return value;
    }

    std::optional<std::string> UnityExplorer::CallString(bool main, std::uint64_t method, const Arg& self)
    {
        const Invoked call = Invoke(main, method, self, {});
        if (!call.Ok)
            return std::nullopt;
        return ReadString(call.Result);
    }

    template <typename T>
    std::optional<T> UnityExplorer::CallValue(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params)
    {
        const Invoked call = Invoke(main, method, self, params);
        if (!call.Ok)
            return std::nullopt;
        return Unbox<T>(call.Result);
    }

    std::uint64_t UnityExplorer::CallObject(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params)
    {
        const Invoked call = Invoke(main, method, self, params);
        return call.Ok ? call.Result : 0;
    }

    std::uint64_t UnityExplorer::NewString(const std::string& text)
    {
        const auto process = Proc();
        const std::size_t mark = m_argsUsed;
        const std::uint64_t at = ArgSpace(text.size() + 1);
        std::uint64_t domain = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            domain = m_mono.m_domain;
        }
        std::uint64_t made = 0;
        if (process && at && process->Write(at, text.c_str(), text.size() + 1))
            made = Exec("mono_string_new", domain, at).value_or(0);
        m_argsUsed = mark;
        return made;
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

    std::uint64_t UnityExplorer::Method(std::string_view klassName, std::string_view name, int params, std::string_view firstParam)
    {
        const std::string key = std::string(klassName) + ":" + std::string(name) + "/" + std::to_string(params) + "/" + std::string(firstParam);
        if (const auto found = m_methods.find(key); found != m_methods.end())
            return found->second;
        const std::uint64_t klass = FindClass(klassName);
        std::uint64_t method = 0;
        const auto process = Proc();
        if (klass && process)
        {
            const std::size_t mark = m_argsUsed;
            if (firstParam.empty())
            {
                const std::uint64_t text = ArgSpace(name.size() + 1);
                const std::string copy(name);
                if (text && process->Write(text, copy.c_str(), copy.size() + 1))
                    method = Worker("mono_class_get_method_from_name", klass, text, static_cast<std::uint64_t>(static_cast<std::int64_t>(params))).value_or(0);
            }
            else
            {
                const std::uint64_t iter = ArgSpace(8);
                const std::uint64_t paramIter = ArgSpace(8);
                const std::uint64_t zero = 0;
                if (iter && paramIter && process->Write(iter, &zero, 8))
                {
                    for (int n = 0; n < kMaxMembers && !method; ++n)
                    {
                        const auto candidate = Worker("mono_class_get_methods", klass, iter);
                        if (!candidate || !*candidate)
                            break;
                        if (WorkerString("mono_method_get_name", *candidate) != name)
                            continue;
                        const auto signature = Worker("mono_method_signature", *candidate);
                        if (!signature || !*signature)
                            continue;
                        if (static_cast<int>(Worker("mono_signature_get_param_count", *signature).value_or(0) & 0xFFFFFFFFu) != params)
                            continue;
                        process->Write(paramIter, &zero, 8);
                        const auto first = Worker("mono_signature_get_params", *signature, paramIter);
                        if (first && *first && TypeName(*first) == firstParam)
                            method = *candidate;
                    }
                }
            }
            m_argsUsed = mark;
        }
        m_methods[key] = method;
        return method;
    }

    std::uint64_t UnityExplorer::TypeObject(std::uint64_t klass)
    {
        if (!klass)
            return 0;
        if (const auto found = m_typeObjects.find(klass); found != m_typeObjects.end())
            return found->second;
        std::uint64_t domain = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            domain = m_mono.m_domain;
        }
        std::uint64_t object = 0;
        if (const auto type = Worker("mono_class_get_type", klass); type && *type)
            object = Exec("mono_type_get_object", domain, *type).value_or(0);
        if (object)
        {
            // Probe level 1 deliberately creates the managed object but takes no
            // GC handle, to tell allocation and handle apart.
            if (m_probeLevel > 1)
            {
                if (const std::uint32_t pin = Pin(object))
                    m_keepPins.push_back(pin);
            }
            m_typeObjects[klass] = object;
        }
        return object;
    }

    std::uint32_t UnityExplorer::Pin(std::uint64_t object)
    {
        if (!object || !m_pinning)
            return 0;
        return static_cast<std::uint32_t>(Worker("mono_gchandle_new", object, 0).value_or(0) & 0xFFFFFFFFu);
    }

    void UnityExplorer::Unpin(std::vector<std::uint32_t>& pins)
    {
        for (const std::uint32_t pin : pins)
        {
            if (pin)
                Worker("mono_gchandle_free", pin);
        }
        pins.clear();
    }

    MonoTypeInfo UnityExplorer::TypeInfo(std::uint64_t type)
    {
        if (!type)
            return {};
        if (const auto found = m_types.find(type); found != m_types.end())
            return found->second;
        MonoTypeInfo info;
        info.Code = static_cast<int>(Worker("mono_type_get_type", type).value_or(0) & 0xFF);
        info.ValueCode = info.Code;
        info.Class = Worker("mono_class_from_mono_type", type).value_or(0);
        info.Name = TypeName(type);
        if (IsPrimitive(info.Code))
        {
            info.ValueType = true;
            info.Size = PrimitiveSize(info.Code);
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
                info.Size = std::max(1, PrimitiveSize(info.ValueCode));
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
        return process->ReadPointer(*vtable).value_or(0);
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
            layout->ElementSize = static_cast<int>(Worker("mono_array_element_size", klass).value_or(8) & 0xFFFF);
        }

        std::uint64_t domain = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            domain = m_mono.m_domain;
        }
        const std::size_t mark = m_argsUsed;
        const std::uint64_t iter = ArgSpace(8);
        const std::uint64_t paramIter = ArgSpace(8);
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
                        member.LiteralBytes.resize(static_cast<std::size_t>(member.Type.ValueType ? member.Type.Size : 8));
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
                        std::vector<std::uint64_t> clear(static_cast<std::size_t>(count), 0);
                        process->Write(names, clear.data(), clear.size() * 8);
                        Worker("mono_method_get_param_names", *method, names);
                        std::vector<std::uint64_t> pointers(static_cast<std::size_t>(count), 0);
                        process->ReadExact(names, pointers.data(), pointers.size() * 8);
                        for (const std::uint64_t pointer : pointers)
                            member.ParamNames.push_back(pointer ? m_mono.m_call.ReadCString(pointer, 128) : std::string());
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
        const int code = type.ValueCode ? type.ValueCode : type.Code;
        const auto need = [&](std::size_t n) { return size >= n; };
        if (type.Enum)
        {
            const int width = std::max(1, PrimitiveSize(code));
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
            if (!need(8)) break;
            shown.Text = "0x" + Hex(Load<std::uint64_t>(bytes));
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
                const std::int64_t at = static_cast<std::int64_t>(field.Offset) - static_cast<std::int64_t>(kObjectHeader);
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
        if (!need(8))
            return { "??" };
        const std::uint64_t pointer = Load<std::uint64_t>(bytes);
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
        const auto process = Proc();
        std::uint64_t length = 0;
        if (process && !className.empty() && className.back() == ']' && process->ReadExact(pointer + kArrayLength, &length, sizeof(length)))
            shown.Text = ShortName(className) + "  (" + std::to_string(length) + ")";
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
        const std::size_t size = static_cast<std::size_t>(field.Type.ValueType ? std::max(1, field.Type.Size) : 8);
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
            const std::uint64_t pointer = value.Raw.size() >= 8 ? Load<std::uint64_t>(value.Raw.data()) : 0;
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
        if (IsPrimitive(code))
        {
            if (!ParsePrimitive(code, text, out.data(), error))
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
            const std::int64_t at = static_cast<std::int64_t>(field.Offset) - static_cast<std::int64_t>(kObjectHeader);
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

    Status UnityExplorer::EnsureHook()
    {
        if (m_hooked)
            return Status::Success();
        // Last line of defence: nothing registers a callback in the game unless
        // the user turned the explorer on for this session.
        if (!m_enabled.load())
            return Status::Fail(ErrorCode::MonoCallFailed, "the scene explorer is switched off");
        // A runtime that is missing one of these silently loses a feature, so
        // record them once instead of letting calls fail with an empty reason.
        {
            static const char* const kNeeded[] = {
                "mono_class_get_parent", "mono_class_get_type", "mono_type_get_object", "mono_type_get_name", "mono_class_from_mono_type",
                "mono_runtime_invoke", "mono_class_get_method_from_name", "mono_method_signature", "mono_signature_get_param_count",
                "mono_signature_get_params", "mono_signature_get_return_type", "mono_class_get_properties", "mono_property_get_name",
                "mono_property_get_get_method", "mono_property_get_set_method", "mono_method_get_flags", "mono_gchandle_new",
                "mono_gchandle_free", "mono_string_new", "mono_object_to_string", "mono_free", "mono_class_vtable",
                "mono_vtable_get_static_field_data", "mono_field_static_get_value", "mono_field_set_value", "mono_method_get_param_names",
            };
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
        std::string hookName = "Application.onBeforeRender";
        std::uint64_t add = Method("UnityEngine.Application", "add_onBeforeRender", 1);
        std::uint64_t remove = Method("UnityEngine.Application", "remove_onBeforeRender", 1);
        if (!add || !remove)
        {
            hookName = "Canvas.willRenderCanvases";
            add = Method("UnityEngine.Canvas", "add_willRenderCanvases", 1);
            remove = Method("UnityEngine.Canvas", "remove_willRenderCanvases", 1);
        }
        if (!add || !remove)
            return Status::Fail(ErrorCode::MonoCallFailed, "this game has neither Application.onBeforeRender nor Canvas.willRenderCanvases (not a Unity game, or a very old Unity)");
        const auto process = Proc();
        if (!process)
            return Status::Fail(ErrorCode::NoProcess);

        // The event's delegate type comes from its add method, so it is right
        // for every Unity version (UnityAction, Canvas.WillRenderCanvases).
        std::uint64_t delegateClass = 0;
        if (const auto signature = Worker("mono_method_signature", add); signature && *signature)
        {
            const std::size_t mark = m_argsUsed;
            const std::uint64_t iter = ArgSpace(8);
            const std::uint64_t zero = 0;
            if (iter && process->Write(iter, &zero, 8))
                if (const auto type = Worker("mono_signature_get_params", *signature, iter); type && *type)
                    delegateClass = Worker("mono_class_from_mono_type", *type).value_or(0);
            m_argsUsed = mark;
        }
        const std::uint64_t delegateType = TypeObject(delegateClass);
        if (m_probeLevel <= 1)
        {
            LOG_INFO("Unity", std::string("Probe: made a Type object without any GC handle (") + (delegateType ? "ok" : "failed") + ")");
            return Status::Fail(ErrorCode::MonoCallFailed, "probe stopped after creating the Type object");
        }
        const std::uint64_t wrap = Method("System.Runtime.InteropServices.Marshal", "GetDelegateForFunctionPointer", 2, "System.IntPtr");
        const std::uint64_t stub = m_mono.m_call.MainStub();
        if (!delegateType || !wrap || !stub)
            return Status::Fail(ErrorCode::MonoCallFailed, "could not build the per-frame callback (Marshal.GetDelegateForFunctionPointer or the delegate type is missing)");
        const Invoked made = Invoke(false, wrap, Arg::Ref(0), { Arg::Value<std::uint64_t>(stub), Arg::Ref(delegateType) });
        if (!made.Ok || !made.Result)
            return Status::Fail(ErrorCode::MonoCallFailed, "could not wrap the per-frame callback: " + made.Error);
        const std::uint32_t pin = Pin(made.Result);
        if (m_probeLevel < 3)
        {
            // Probe: the delegate exists and is pinned, but nothing in the game
            // points at it yet.
            if (pin)
                m_keepPins.push_back(pin);
            m_delegate = made.Result;
            LOG_INFO("Unity", "Probe: the callback delegate was created but NOT registered");
            return Status::Fail(ErrorCode::MonoCallFailed, "probe stopped after creating the delegate");
        }
        const Invoked added = Invoke(false, add, Arg::Ref(0), { Arg::Ref(made.Result) });
        if (!added.Ok)
        {
            std::vector<std::uint32_t> pins = { pin };
            Unpin(pins);
            return Status::Fail(ErrorCode::MonoCallFailed, "could not register the per-frame callback on " + hookName + ": " + added.Error);
        }
        m_hooked = true;
        m_delegate = made.Result;
        m_delegatePin = pin;
        m_unhook = remove;
        LOG_INFO("Unity", "Registered the main-thread callback on " + hookName);
        {
            std::lock_guard state(m_stateLock);
            m_status.Hooked = true;
            m_status.Hook = hookName;
        }
        if (m_probeLevel < 4)
        {
            // Probe: registered, so the game calls the stub every frame, but we
            // never ask it to serve anything.
            LOG_INFO("Unity", "Probe: the callback is registered; not entering the main thread");
            Publish();
            return Status::Fail(ErrorCode::MonoCallFailed, "probe stopped after registering the callback");
        }
        if (const Status entered = m_mono.m_call.EnterMain(std::chrono::milliseconds(3000)); !entered)
        {
            // The run count separates "the game never called us" (not
            // rendering) from "it did, but the handshake went wrong".
            const std::uint64_t runs = m_mono.m_call.MainRuns();
            const std::uint32_t thread = m_mono.m_call.MainThreadId();
            {
                std::lock_guard state(m_stateLock);
                m_status.CallbackRuns = runs;
                m_status.MainThread = thread;
            }
            Publish();
            return entered;
        }
        m_visitStart = std::chrono::steady_clock::now();
        m_visiting = true;
        const auto version = CallString(true, Method("UnityEngine.Application", "get_unityVersion", 0), Arg::Ref(0));
        const auto background = CallValue<std::uint8_t>(true, Method("UnityEngine.Application", "get_runInBackground", 0), Arg::Ref(0));
        {
            std::lock_guard state(m_stateLock);
            m_status.CallbackRuns = m_mono.m_call.MainRuns();
            m_status.MainThread = m_mono.m_call.MainThreadId();
            m_status.UnityVersion = version.value_or(std::string());
            if (background)
                m_status.RunInBackground = *background != 0;
            m_status.Error.clear();
        }
        Publish();
        return Status::Success();
    }

    void UnityExplorer::Unhook()
    {
        if (!m_hooked)
            return;
        // On the MAIN thread (the caller has entered it): remove_onBeforeRender
        // edits the subscriber list that the same thread is iterating to call
        // us, so there is no concurrent writer. Doing this from the worker
        // raced that iteration and crashed the game.
        const Invoked removed = Invoke(true, m_unhook, Arg::Ref(0), { Arg::Ref(m_delegate) });
        if (!removed.Ok)
            LOG_WARN("Unity", "Could not take the main-thread callback out of the game (" + removed.Error + "); it stays harmless and returns at once");
        else
            LOG_INFO("Unity", "Took the main-thread callback out of the game");
        if (m_delegatePin)
        {
            std::vector<std::uint32_t> pins = { m_delegatePin };
            Unpin(pins);
        }
        m_hooked = false;
        m_delegate = 0;
        m_delegatePin = 0;
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

    void UnityExplorer::DoReadScenes()
    {
        {
            std::lock_guard state(m_stateLock);
            m_status.Error.clear();
        }
        if (Status hooked = EnsureHook(); !hooked)
        {
            Fail(hooked.Text);
            return;
        }
        const char* manager = "UnityEngine.SceneManagement.SceneManager";
        const char* sceneClass = "UnityEngine.SceneManagement.Scene";
        const auto count = CallValue<std::int32_t>(true, Method(manager, "get_sceneCount", 0), Arg::Ref(0));
        if (!count)
        {
            Fail("Could not read SceneManager.sceneCount (this Unity version may be older than 5.3)");
            return;
        }
        const int active = CallValue<std::int32_t>(true, Method(manager, "GetActiveScene", 0), Arg::Ref(0)).value_or(0);
        UnityTree tree;
        tree.Read = true;
        for (int i = 0; i < std::min(*count, 256); ++i)
        {
            const auto handle = CallValue<std::int32_t>(true, Method(manager, "GetSceneAt", 1), Arg::Ref(0), { Arg::Value<std::int32_t>(i) });
            if (!handle)
                continue;
            UnityScene scene;
            scene.Handle = *handle;
            const Arg self = Arg::Value<std::int32_t>(*handle);
            scene.Name = CallString(true, Method(sceneClass, "get_name", 0), self).value_or("?");
            scene.Loaded = CallValue<std::uint8_t>(true, Method(sceneClass, "get_isLoaded", 0), self).value_or(0) != 0;
            scene.RootCount = CallValue<std::int32_t>(true, Method(sceneClass, "get_rootCount", 0), self).value_or(-1);
            scene.BuildIndex = CallValue<std::int32_t>(true, Method(sceneClass, "get_buildIndex", 0), self).value_or(-1);
            scene.Active = scene.Handle == active;
            tree.Scenes.push_back(std::move(scene));
            YieldFrame();
        }
        UnityScene dontDestroy;
        dontDestroy.Name = "DontDestroyOnLoad";
        dontDestroy.DontDestroy = true;
        dontDestroy.Handle = m_dontDestroyHandle;
        dontDestroy.Loaded = true;
        tree.Scenes.push_back(std::move(dontDestroy));

        std::vector<std::uint32_t> old;
        {
            std::lock_guard state(m_stateLock);
            tree.Generation = m_tree.Generation + 1;
            m_tree = std::move(tree);
            old.swap(m_treePins);
        }
        Unpin(old);
        Publish();
    }

    int UnityExplorer::ResolveDontDestroyScene()
    {
        if (m_dontDestroyHandle)
            return m_dontDestroyHandle;
        // Unity lists no DontDestroyOnLoad scene; like other explorers, move a
        // fresh empty GameObject there, read its scene and destroy it again in
        // the same frame.
        std::uint64_t domain = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            domain = m_mono.m_domain;
        }
        const std::uint64_t klass = FindClass("UnityEngine.GameObject");
        const std::uint64_t probe = klass ? Main("mono_object_new", domain, klass).value_or(0) : 0;
        if (!probe)
            return 0;
        const Invoked made = Invoke(true, Method("UnityEngine.GameObject", ".ctor", 0), Arg::Ref(probe), {});
        if (!made.Ok)
            return 0;
        Invoke(true, Method("UnityEngine.Object", "DontDestroyOnLoad", 1), Arg::Ref(0), { Arg::Ref(probe) });
        const auto handle = CallValue<std::int32_t>(true, Method("UnityEngine.GameObject", "get_scene", 0), Arg::Ref(probe));
        Invoke(true, Method("UnityEngine.Object", "DestroyImmediate", 1), Arg::Ref(0), { Arg::Ref(probe) });
        m_dontDestroyHandle = handle.value_or(0);
        return m_dontDestroyHandle;
    }

    bool UnityExplorer::ReadNode(std::uint64_t gameObject, std::uint64_t knownTransform, UnityNode& node)
    {
        if (!gameObject)
            return false;
        node.Object = gameObject;
        const std::uint32_t pin = Pin(gameObject);
        if (pin)
            m_treePins.push_back(pin);
        const Arg self = Arg::Ref(gameObject);
        node.Name = CallString(true, Method("UnityEngine.Object", "get_name", 0), self).value_or("?");
        node.Active = CallValue<std::uint8_t>(true, Method("UnityEngine.GameObject", "get_activeSelf", 0), self).value_or(0) != 0;
        node.Transform = knownTransform ? knownTransform : CallObject(true, Method("UnityEngine.GameObject", "get_transform", 0), self);
        if (node.Transform && !knownTransform)
            if (const std::uint32_t transformPin = Pin(node.Transform))
                m_treePins.push_back(transformPin);
        node.ChildCount = node.Transform ? CallValue<std::int32_t>(true, Method("UnityEngine.Transform", "get_childCount", 0), Arg::Ref(node.Transform)).value_or(0) : 0;
        return true;
    }

    void UnityExplorer::DoExpandScene(int sceneIndex)
    {
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
        if (Status hooked = EnsureHook(); !hooked)
            return finish(hooked.Text);
        int handle = scene.Handle;
        if (scene.DontDestroy && !handle)
        {
            handle = ResolveDontDestroyScene();
            if (!handle)
                return finish("Could not find the DontDestroyOnLoad scene");
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation == generation)
                m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].Handle = handle;
        }
        const Invoked roots = Invoke(true, Method("UnityEngine.SceneManagement.Scene", "GetRootGameObjects", 0), Arg::Value<std::int32_t>(handle), {});
        if (!roots.Ok)
            return finish("Could not read the scene's objects: " + roots.Error);
        std::vector<std::uint32_t> arrayPin = { Pin(roots.Result) };
        std::size_t total = 0;
        const std::vector<std::uint64_t> objects = ReadArray(roots.Result, kMaxChildren, &total);
        std::vector<UnityNode> batch;
        const auto flush = [&]
            {
                std::lock_guard state(m_stateLock);
                if (m_tree.Generation != generation)
                    return false;
                for (UnityNode& node : batch)
                {
                    if (m_tree.Nodes.size() >= kMaxNodes)
                        break;
                    node.Scene = sceneIndex;
                    m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].Roots.push_back(static_cast<int>(m_tree.Nodes.size()));
                    m_tree.Nodes.push_back(std::move(node));
                }
                m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].RootCount = static_cast<int>(total);
                batch.clear();
                return true;
            };
        for (std::size_t i = 0; i < objects.size(); ++i)
        {
            if (Aborted())
                break;
            UnityNode node;
            if (ReadNode(objects[i], 0, node))
                batch.push_back(std::move(node));
            if (batch.size() >= 64)
            {
                if (!flush())
                    break;
                Publish();
            }
            YieldFrame();
        }
        flush();
        Unpin(arrayPin);
        finish({});
    }

    void UnityExplorer::DoExpandNode(int nodeIndex)
    {
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
        std::vector<UnityNode> children;
        if (Status hooked = EnsureHook(); !hooked)
            error = hooked.Text;
        else if (parent.Transform)
        {
            const int count = CallValue<std::int32_t>(true, Method("UnityEngine.Transform", "get_childCount", 0), Arg::Ref(parent.Transform)).value_or(0);
            for (int i = 0; i < std::min<int>(count, static_cast<int>(kMaxChildren)); ++i)
            {
                if (Aborted())
                    break;
                const std::uint64_t child = CallObject(true, Method("UnityEngine.Transform", "GetChild", 1), Arg::Ref(parent.Transform), { Arg::Value<std::int32_t>(i) });
                if (!child)
                    continue;
                if (const std::uint32_t pin = Pin(child))
                    m_treePins.push_back(pin);
                const std::uint64_t gameObject = CallObject(true, Method("UnityEngine.Component", "get_gameObject", 0), Arg::Ref(child));
                UnityNode node;
                if (ReadNode(gameObject, child, node))
                {
                    node.Parent = nodeIndex;
                    node.Scene = parent.Scene;
                    children.push_back(std::move(node));
                }
                YieldFrame();
            }
        }
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation == generation && nodeIndex < static_cast<int>(m_tree.Nodes.size()))
            {
                UnityNode& target = m_tree.Nodes[static_cast<std::size_t>(nodeIndex)];
                target.Loading = false;
                target.ChildrenLoaded = error.empty();
                for (UnityNode& node : children)
                {
                    if (m_tree.Nodes.size() >= kMaxNodes)
                        break;
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
        Unpin(m_searchPins);
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
        if (IsUnityObjectClass(klass))
        {
            if (Status hooked = EnsureHook(); !hooked)
            {
                result.Error = hooked.Text;
                publish(true);
                Publish();
                return;
            }
            const std::uint64_t type = TypeObject(klass);
            const std::uint64_t finder = request.IncludeHidden ? Method("UnityEngine.Resources", "FindObjectsOfTypeAll", 1, "System.Type")
                                                               : Method("UnityEngine.Object", "FindObjectsOfType", 1, "System.Type");
            const Invoked found = Invoke(true, finder, Arg::Ref(0), { Arg::Ref(type) });
            if (!found.Ok)
            {
                result.Error = "The search failed: " + found.Error;
                publish(true);
                Publish();
                return;
            }
            std::vector<std::uint32_t> arrayPin = { Pin(found.Result) };
            const std::vector<std::uint64_t> objects = ReadArray(found.Result, kMaxArray, &result.Total);
            const std::uint64_t getName = Method("UnityEngine.Object", "get_name", 0);
            const std::string filter = Trim(request.NameFilter);
            for (std::size_t i = 0; i < objects.size() && result.Hits.size() < kMaxHits; ++i)
            {
                if (Aborted())
                    break;
                if (!objects[i])
                    continue;
                UnityHit hit;
                hit.Object = objects[i];
                hit.Name = CallString(true, getName, Arg::Ref(objects[i])).value_or("?");
                if (!filter.empty() && !ContainsNoCase(hit.Name, filter))
                    continue;
                hit.Class = ClassNameOf(objects[i]);
                if (const std::uint32_t pin = Pin(objects[i]))
                    m_searchPins.push_back(pin);
                result.Hits.push_back(std::move(hit));
                if (result.Hits.size() % 100 == 0)
                {
                    publish(false);
                    Publish();
                }
                YieldFrame();
            }
            Unpin(arrayPin);
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
        std::vector<std::uint64_t> chunk;
        // Sweep once over a set of 8-byte values and hand every hit's ADDRESS
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
                        chunk.assign(static_cast<std::size_t>(size / 8), 0);
                        const std::size_t got = process->Read(at, chunk.data(), chunk.size() * 8) / 8;
                        for (std::size_t i = 0; i < got; ++i)
                        {
                            if (std::find(wanted.begin(), wanted.end(), chunk[i]) == wanted.end())
                                continue;
                            if (!hit(at + i * 8))
                                return;
                        }
                    }
                }
            };

        std::vector<std::uint64_t> vtables;
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
        int id = 0;
        {
            std::lock_guard state(m_stateLock);
            id = m_nextInspection++;
            UnityInspection& inspection = m_inspections[id];
            inspection.Id = id;
            inspection.Object = object;
            inspection.Klass = klass;
            inspection.FromScan = fromScan;
            inspection.Loading = true;
            inspection.Title = Hex(object);
        }
        Publish();
        Post("Inspecting an object", [this, id] { DoInspect(id, true); });
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
        Post("Closing an inspector", [this, id]
            {
                if (const auto found = m_inspectionPins.find(id); found != m_inspectionPins.end())
                {
                    Unpin(found->second);
                    m_inspectionPins.erase(found);
                }
            });
        Publish();
    }

    bool UnityExplorer::ReadTransform(std::uint64_t transform, UnityTransform& info)
    {
        if (!transform)
            return false;
        info.Object = transform;
        const Arg self = Arg::Ref(transform);
        const char* klass = "UnityEngine.Transform";
        const auto read = [&](const char* name, float* out)
            {
                if (const auto value = CallValue<Vector3>(true, Method(klass, name, 0), self))
                {
                    out[0] = value->X;
                    out[1] = value->Y;
                    out[2] = value->Z;
                    return true;
                }
                return false;
            };
        info.Read = read("get_localPosition", info.LocalPosition);
        read("get_localEulerAngles", info.LocalEuler);
        read("get_localScale", info.LocalScale);
        read("get_position", info.Position);
        return info.Read;
    }

    void UnityExplorer::ReadGameObject(std::uint64_t gameObject, UnityGameObject& info, std::vector<std::uint32_t>& pins)
    {
        info.Object = gameObject;
        const Arg self = Arg::Ref(gameObject);
        const char* go = "UnityEngine.GameObject";
        info.Name = CallString(true, Method("UnityEngine.Object", "get_name", 0), self).value_or("?");
        info.Tag = CallString(true, Method(go, "get_tag", 0), self).value_or(std::string());
        info.Layer = CallValue<std::int32_t>(true, Method(go, "get_layer", 0), self).value_or(0);
        info.ActiveSelf = CallValue<std::uint8_t>(true, Method(go, "get_activeSelf", 0), self).value_or(0) != 0;
        info.ActiveInHierarchy = CallValue<std::uint8_t>(true, Method(go, "get_activeInHierarchy", 0), self).value_or(0) != 0;
        if (const auto scene = CallValue<std::int32_t>(true, Method(go, "get_scene", 0), self))
            info.Scene = CallString(true, Method("UnityEngine.SceneManagement.Scene", "get_name", 0), Arg::Value<std::int32_t>(*scene)).value_or(std::string());
        const std::uint64_t transform = CallObject(true, Method(go, "get_transform", 0), self);
        if (transform)
        {
            pins.push_back(Pin(transform));
            ReadTransform(transform, info.Transform);
            const std::uint64_t parent = CallObject(true, Method("UnityEngine.Transform", "get_parent", 0), Arg::Ref(transform));
            if (parent)
            {
                info.Parent = CallObject(true, Method("UnityEngine.Component", "get_gameObject", 0), Arg::Ref(parent));
                pins.push_back(Pin(parent));
                pins.push_back(Pin(info.Parent));
                info.ParentName = CallString(true, Method("UnityEngine.Object", "get_name", 0), Arg::Ref(info.Parent)).value_or("?");
            }
            info.ChildCount = CallValue<std::int32_t>(true, Method("UnityEngine.Transform", "get_childCount", 0), Arg::Ref(transform)).value_or(0);
            for (int i = 0; i < std::min<int>(info.ChildCount, static_cast<int>(kMaxShownChildren)); ++i)
            {
                const std::uint64_t child = CallObject(true, Method("UnityEngine.Transform", "GetChild", 1), Arg::Ref(transform), { Arg::Value<std::int32_t>(i) });
                const std::uint64_t childObject = child ? CallObject(true, Method("UnityEngine.Component", "get_gameObject", 0), Arg::Ref(child)) : 0;
                if (!childObject)
                    continue;
                pins.push_back(Pin(childObject));
                info.Children.push_back({ childObject, CallString(true, Method("UnityEngine.Object", "get_name", 0), Arg::Ref(childObject)).value_or("?"), "GameObject" });
                YieldFrame();
            }
        }
        const std::uint64_t componentType = TypeObject(FindClass("UnityEngine.Component"));
        const std::uint64_t list = CallObject(true, Method(go, "GetComponents", 1, "System.Type"), self, { Arg::Ref(componentType) });
        if (list)
        {
            pins.push_back(Pin(list));
            for (const std::uint64_t component : ReadArray(list, 512))
            {
                if (!component)
                    continue;
                pins.push_back(Pin(component));
                UnityComponent entry;
                entry.Object = component;
                const std::uint64_t klass = ClassOf(component);
                entry.Class = ClassName(klass);
                const std::vector<std::string> chain = ChainOf(klass);
                entry.HasEnabled = std::find(chain.begin(), chain.end(), "UnityEngine.Behaviour") != chain.end();
                if (entry.HasEnabled)
                    entry.Enabled = CallValue<std::uint8_t>(true, Method("UnityEngine.Behaviour", "get_enabled", 0), Arg::Ref(component)).value_or(0) != 0;
                info.Components.push_back(std::move(entry));
                YieldFrame();
            }
        }
    }

    MonoReadValue UnityExplorer::ReadResult(const MonoTypeInfo& type, const Invoked& call, std::vector<std::uint32_t>& pins)
    {
        MonoReadValue value;
        if (!call.Ok)
        {
            value.Error = call.Error;
            return value;
        }
        value.Read = true;
        if (type.Code == MonoTypeCode::Void)
            return value;
        const auto process = Proc();
        if (type.ValueType)
        {
            value.Raw.assign(static_cast<std::size_t>(std::max(1, type.Size)), 0);
            if (!call.Result || !process || !process->ReadExact(call.Result + kObjectHeader, value.Raw.data(), value.Raw.size()))
            {
                value.Read = false;
                value.Error = "no value came back";
            }
            return value;
        }
        value.Raw.resize(8);
        std::memcpy(value.Raw.data(), &call.Result, 8);
        if (call.Result)
        {
            pins.push_back(Pin(call.Result));
            if (type.Code == MonoTypeCode::String || ClassNameOf(call.Result) == "System.String")
                value.Text = ReadString(call.Result, 1024);
        }
        return value;
    }

    void UnityExplorer::ReadProperties(UnityInspection& inspection, std::vector<std::uint32_t>& pins)
    {
        if (!inspection.Layout || inspection.FromScan)
            return;
        for (std::size_t i = 0; i < inspection.Layout->Members.size(); ++i)
        {
            if (Aborted())
                break;
            const MonoMember& member = inspection.Layout->Members[i];
            if (member.Kind != MonoMemberKind::Property || !member.Getter)
                continue;
            if (!member.Static && !inspection.Object)
                continue;
            const Invoked call = Invoke(true, member.Getter, Arg::Ref(member.Static ? 0 : inspection.Object), {});
            inspection.Values[static_cast<int>(i)] = ReadResult(member.Type, call, pins);
            YieldFrame();
        }
    }

    void UnityExplorer::DoInspect(int id, bool properties)
    {
        std::optional<UnityInspection> copy = Inspection(id);
        if (!copy)
            return;
        UnityInspection inspection = *copy;
        inspection.Error.clear();
        if (!inspection.Klass && inspection.Object)
            inspection.Klass = ClassOf(inspection.Object);
        if (!inspection.Klass)
        {
            inspection.Error = "This address does not hold a managed object.";
        }
        else
        {
            inspection.Layout = LoadLayout(inspection.Klass);
            inspection.Title = inspection.Layout->Name;
        }
        std::vector<std::uint32_t> pins;
        if (inspection.Layout && inspection.Object && !inspection.FromScan)
        {
            pins.push_back(Pin(inspection.Object));
            const MonoClassLayout& layout = *inspection.Layout;
            if (layout.UnityObject)
            {
                if (Status hooked = EnsureHook(); !hooked)
                    inspection.Error = hooked.Text;
                else
                {
                    const Arg self = Arg::Ref(inspection.Object);
                    const Invoked name = Invoke(true, Method("UnityEngine.Object", "get_name", 0), self, {});
                    if (name.Ok)
                        inspection.ObjectName = ReadString(name.Result);
                    else
                        inspection.Error = name.Error;
                    if (layout.GameObject)
                    {
                        UnityGameObject info;
                        ReadGameObject(inspection.Object, info, pins);
                        inspection.GameObject = std::move(info);
                    }
                    else if (layout.Component)
                    {
                        inspection.Owner = CallObject(true, Method("UnityEngine.Component", "get_gameObject", 0), self);
                        if (inspection.Owner)
                        {
                            pins.push_back(Pin(inspection.Owner));
                            inspection.OwnerName = CallString(true, Method("UnityEngine.Object", "get_name", 0), Arg::Ref(inspection.Owner)).value_or("?");
                        }
                        if (layout.Behaviour)
                            if (const auto enabled = CallValue<std::uint8_t>(true, Method("UnityEngine.Behaviour", "get_enabled", 0), self))
                                inspection.Enabled = *enabled != 0;
                        if (layout.Transform)
                        {
                            UnityTransform transform;
                            if (ReadTransform(inspection.Object, transform))
                                inspection.Transform = transform;
                        }
                    }
                }
            }
        }
        bool readProperties = false;
        if (properties && inspection.Layout && (inspection.Object || inspection.Klass))
        {
            bool any = false;
            for (const MonoMember& member : inspection.Layout->Members)
                any = any || (member.Kind == MonoMemberKind::Property && member.Getter && (member.Static || inspection.Object));
            if (any && !inspection.FromScan)
            {
                if (Status hooked = EnsureHook(); !hooked)
                {
                    if (inspection.Error.empty())
                        inspection.Error = "Properties were not read: " + hooked.Text;
                }
                else
                {
                    // Method results stay; property values are read again.
                    std::map<int, MonoReadValue> kept;
                    for (const auto& [index, value] : inspection.Values)
                    {
                        if (index >= 0 && index < static_cast<int>(inspection.Layout->Members.size()) &&
                            inspection.Layout->Members[static_cast<std::size_t>(index)].Kind == MonoMemberKind::Method)
                            kept.emplace(index, value);
                    }
                    inspection.Values = std::move(kept);
                    readProperties = true;
                }
            }
        }
        // Values carried over keep their objects alive under the new pins.
        for (const auto& [index, value] : inspection.Values)
        {
            if (value.Read && value.Raw.size() == 8 && index >= 0 && index < static_cast<int>(inspection.Layout ? inspection.Layout->Members.size() : 0) &&
                !inspection.Layout->Members[static_cast<std::size_t>(index)].Type.ValueType)
            {
                std::uint64_t pointer = 0;
                std::memcpy(&pointer, value.Raw.data(), 8);
                if (pointer)
                    pins.push_back(Pin(pointer));
            }
        }
        if (readProperties)
            ReadProperties(inspection, pins);
        inspection.Loading = false;
        std::vector<std::uint32_t> old;
        bool alive = false;
        {
            std::lock_guard state(m_stateLock);
            const auto found = m_inspections.find(id);
            if (found != m_inspections.end())
            {
                alive = true;
                inspection.Generation = found->second.Generation + 1;
                found->second = std::move(inspection);
            }
        }
        if (alive)
        {
            old.swap(m_inspectionPins[id]);
            m_inspectionPins[id] = std::move(pins);
        }
        else
            old = std::move(pins);
        Unpin(old);
        Publish();
    }

    // ---------------------------------------------------------------------
    // Edits

    void UnityExplorer::SetActive(std::uint64_t gameObject, bool active, int refresh)
    {
        Post(active ? "Activating an object" : "Deactivating an object", [this, gameObject, active, refresh]
            {
                if (Status hooked = EnsureHook(); !hooked)
                    return Fail(hooked.Text);
                const Invoked done = Invoke(true, Method("UnityEngine.GameObject", "SetActive", 1), Arg::Ref(gameObject), { Arg::Value<std::uint8_t>(active ? 1 : 0) });
                if (!done.Ok)
                    return Fail("SetActive failed: " + done.Error);
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
                if (Status hooked = EnsureHook(); !hooked)
                    return Fail(hooked.Text);
                const Invoked done = Invoke(true, Method("UnityEngine.Behaviour", "set_enabled", 1), Arg::Ref(behaviour), { Arg::Value<std::uint8_t>(enabled ? 1 : 0) });
                if (!done.Ok)
                    return Fail("Setting enabled failed: " + done.Error);
                if (refresh)
                    DoInspect(refresh, false);
            });
    }

    void UnityExplorer::SetName(std::uint64_t object, std::string name, int refresh)
    {
        Post("Renaming an object", [this, object, name, refresh]
            {
                if (Status hooked = EnsureHook(); !hooked)
                    return Fail(hooked.Text);
                const std::uint64_t text = NewString(name);
                std::vector<std::uint32_t> pins = { Pin(text) };
                const Invoked done = text ? Invoke(true, Method("UnityEngine.Object", "set_name", 1), Arg::Ref(object), { Arg::Ref(text) }) : Invoked{ false, 0, "could not make the string" };
                Unpin(pins);
                if (!done.Ok)
                    return Fail("Renaming failed: " + done.Error);
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
        const Vector3 vector{ value[0], value[1], value[2] };
        Post("Moving an object", [this, transform, which, vector, refresh]
            {
                if (Status hooked = EnsureHook(); !hooked)
                    return Fail(hooked.Text);
                static const char* const kSetters[] = { "set_localPosition", "set_localEulerAngles", "set_localScale" };
                const char* setter = kSetters[std::clamp(which, 0, 2)];
                const Invoked done = Invoke(true, Method("UnityEngine.Transform", setter, 1), Arg::Ref(transform), { Arg::Value(vector) });
                if (!done.Ok)
                    return Fail(std::string(setter) + " failed: " + done.Error);
                if (refresh)
                    DoInspect(refresh, false);
            });
    }

    void UnityExplorer::SetRunInBackground(bool enabled)
    {
        Post(enabled ? "Keeping the game running in the background" : "Letting the game pause in the background", [this, enabled]
            {
                const std::uint64_t setter = Method("UnityEngine.Application", "set_runInBackground", 1);
                const std::uint64_t getter = Method("UnityEngine.Application", "get_runInBackground", 0);
                if (!setter)
                    return Fail("This Unity version has no Application.runInBackground");
                // runInBackground is a real engine property, so the setter MUST
                // run on the main thread — never on the worker, where a Unity
                // engine call crashes the game. If the game is not rendering
                // (so the main thread never reaches the callback) this cannot be
                // set; the game has to be in the foreground once to turn it on.
                if (Status hooked = EnsureHook(); !hooked)
                    return Fail(hooked.Text);
                if (!m_mono.m_call.EnterMain(std::chrono::milliseconds(1500)))
                    return Fail("Bring the game to the foreground once so this can run on its main thread, then try again.");
                if (!m_originalRunInBackground)
                {
                    if (const auto before = CallValue<std::uint8_t>(true, getter, Arg::Ref(0)))
                        m_originalRunInBackground = *before != 0;
                }
                const Invoked done = Invoke(true, setter, Arg::Ref(0), { Arg::Value<std::uint8_t>(enabled ? 1 : 0) });
                if (!done.Ok)
                    return Fail("Could not change Application.runInBackground: " + done.Error);
                const auto now = CallValue<std::uint8_t>(true, getter, Arg::Ref(0));
                std::lock_guard state(m_stateLock);
                m_status.RunInBackground = now ? *now != 0 : enabled;
                m_status.ChangedRunInBackground = m_originalRunInBackground && *m_originalRunInBackground != enabled;
                if (m_originalRunInBackground && *m_originalRunInBackground == enabled)
                    m_originalRunInBackground.reset();
            });
    }

    std::optional<UnityExplorer::Arg> UnityExplorer::ArgumentFor(const MonoTypeInfo& type, const std::string& text, std::string& error)
    {
        if (type.ValueType)
        {
            if (type.Class && !IsPrimitive(type.ValueCode))
                LoadLayout(type.Class);
            const auto bytes = Parse(type, text, &error);
            if (!bytes)
                return std::nullopt;
            return Arg::Raw(*bytes);
        }
        const std::string trimmed = Trim(text);
        if (type.Code == MonoTypeCode::String)
        {
            if (trimmed == "null")
                return Arg::Ref(0);
            std::string body = text;
            if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                body = body.substr(1, body.size() - 2);
            const std::uint64_t made = NewString(body);
            if (!made)
            {
                error = "could not make the string";
                return std::nullopt;
            }
            return Arg::Ref(made);
        }
        if (trimmed.empty() || trimmed == "null")
            return Arg::Ref(0);
        std::string digits = trimmed;
        if (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0)
            digits = digits.substr(2);
        const auto address = ParseHex(digits);
        if (!address || !ClassOf(*address))
        {
            error = "type null, or the address of an object";
            return std::nullopt;
        }
        return Arg::Ref(*address);
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
        if (member.Kind == MonoMemberKind::Field)
        {
            if (member.Literal)
                return Fail(member.Name + " is a constant");
            if (!member.Static && !inspection->Object)
                return Fail(member.Name + " needs an object");
            if (member.Type.ValueType)
            {
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
            }
            else
            {
                const auto arg = ArgumentFor(member.Type, text, error);
                if (!arg)
                    return Fail(member.Name + ": " + error);
                std::vector<std::uint32_t> pins = { Pin(arg->Pointer) };
                const std::size_t mark = m_argsUsed;
                const std::uint64_t slot = ArgSpace(8);
                bool done = false;
                if (slot && process->Write(slot, &arg->Pointer, 8))
                {
                    if (member.Static)
                        done = member.VTable && Worker("mono_field_static_set_value", member.VTable, member.Handle, slot).has_value();
                    else
                        done = Worker("mono_field_set_value", inspection->Object, member.Handle, slot).has_value();
                }
                m_argsUsed = mark;
                Unpin(pins);
                if (!done)
                    return Fail("Could not write " + member.Name);
            }
        }
        else if (member.Kind == MonoMemberKind::Property)
        {
            if (!member.Setter)
                return Fail(member.Name + " has no setter");
            if (!member.Static && !inspection->Object)
                return Fail(member.Name + " needs an object");
            if (Status hooked = EnsureHook(); !hooked)
                return Fail(hooked.Text);
            const auto arg = ArgumentFor(member.Type, text, error);
            if (!arg)
                return Fail(member.Name + ": " + error);
            std::vector<std::uint32_t> pins = { Pin(arg->Pointer) };
            const Invoked done = Invoke(true, member.Setter, Arg::Ref(member.Static ? 0 : inspection->Object), { *arg });
            Unpin(pins);
            if (!done.Ok)
                return Fail("Setting " + member.Name + " failed: " + done.Error);
        }
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
        std::vector<std::uint32_t> pins;
        std::vector<Arg> args;
        std::string error;
        for (std::size_t i = 0; i < member.Params.size() && error.empty(); ++i)
        {
            const auto arg = ArgumentFor(member.Params[i], i < arguments.size() ? arguments[i] : std::string(), error);
            if (arg)
            {
                if (arg->Pointer)
                    pins.push_back(Pin(arg->Pointer));
                args.push_back(*arg);
            }
            else
                error = (i < member.ParamNames.size() && !member.ParamNames[i].empty() ? member.ParamNames[i] : "argument " + std::to_string(i + 1)) + ": " + error;
        }
        if (!error.empty())
            value.Error = error;
        else if (Status hooked = EnsureHook(); !hooked)
            value.Error = hooked.Text;
        else
        {
            const Invoked call = Invoke(true, member.Handle, Arg::Ref(member.Static ? 0 : inspection->Object), args);
            std::vector<std::uint32_t>& kept = m_inspectionPins[id];
            value = ReadResult(member.Type, call, kept);
            if (call.Ok)
                LOG_INFO("Unity", "Called " + member.Declaring + ":" + member.Name);
        }
        Unpin(pins);
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
