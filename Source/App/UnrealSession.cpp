#include "App/UnrealSession.h"

#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <cstring>

namespace Manifold
{
    namespace
    {
        template <class T>
        T Bytes(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            T value{};
            if (offset + sizeof(T) <= bytes.size())
                std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }
    }

    UnrealSession::UnrealSession(TargetSession& target, BackgroundWork& work, Notifier notify) : m_target(target), m_work(work), m_notify(std::move(notify))
    {
        m_target.SetExtraSymbols([this](std::string_view name) { return ResolveSymbol(name); });
    }

    std::shared_ptr<const Target::UnrealRuntime> UnrealSession::Runtime() const
    {
        std::lock_guard lock(m_lock);
        return m_runtime;
    }

    std::string UnrealSession::Unavailable() const
    {
        return m_stopped ? "(Unreal Engine tool shut down)" : "(Unreal Engine not resolved)";
    }

    void UnrealSession::RegisterTypes(Lua::LuaEngine& lua)
    {
        Lua::CustomType name;
        name.Name = Target::kUnrealFNameType;
        name.String = true;
        name.SetByteCount(8);
        name.NativeText = [this](std::span<const std::uint8_t> bytes) -> std::optional<std::string>
            {
                const auto runtime = Runtime();
                if (!runtime || !m_config.Types)
                    return m_config.Types ? Unavailable() : std::string("(Unreal types are off)");
                return runtime->NameFromParts(Bytes<std::uint32_t>(bytes, 0), Bytes<std::uint32_t>(bytes, 4)).value_or("(no name)");
            };
        name.NativeBytes = [this](std::string_view text) -> std::optional<std::vector<std::uint8_t>>
            {
                const auto runtime = Runtime();
                if (!runtime)
                    return std::nullopt;
                const auto value = runtime->FindName(Trim(text));
                if (!value)
                    return std::nullopt;
                std::vector<std::uint8_t> out(8);
                std::memcpy(out.data(), &*value, 8);
                return out;
            };
        lua.RegisterCustomType(std::move(name));

        Lua::CustomType string;
        string.Name = Target::kUnrealFStringType;
        string.String = true;
        string.SetByteCount(16);
        string.NativeText = [this](std::span<const std::uint8_t> bytes) -> std::optional<std::string>
            {
                const auto runtime = Runtime();
                if (!runtime || !m_config.Types)
                    return m_config.Types ? Unavailable() : std::string("(Unreal types are off)");
                const auto data = Bytes<std::uint64_t>(bytes, 0);
                const auto count = Bytes<std::int32_t>(bytes, 8);
                if (count == 0 || data == 0)
                    return std::string();
                if (data < 0x10000 || data > 0x7FFFFFFFFFFF || count < 0 || count > 0x10000)
                    return std::string("(not a string)");
                const int length = std::min(count - 1, 512);
                if (length <= 0)
                    return std::string();
                return runtime->WideText(data, static_cast<std::size_t>(length)).value_or("??");
            };
        lua.RegisterCustomType(std::move(string));

        Lua::CustomType index;
        index.Name = Target::kUnrealObjectIndexType;
        index.String = true;
        index.SetByteCount(8);
        index.NativeText = [this](std::span<const std::uint8_t> bytes) -> std::optional<std::string>
            {
                const auto runtime = Runtime();
                if (!runtime || !m_config.Types)
                    return m_config.Types ? Unavailable() : std::string("(Unreal types are off)");
                const auto slot = Bytes<std::uint32_t>(bytes, 0);
                if (slot == 0 || slot == 0xFFFFFFFFu)
                    return std::string("None");
                const auto object = runtime->ObjectAt(static_cast<int>(slot));
                if (!object)
                    return Format("#%u (empty)", slot);
                return runtime->NameOf(*object).value_or("?") + " (" + runtime->NameOf(runtime->ClassOf(*object)).value_or("?") + ")";
            };
        lua.RegisterCustomType(std::move(index));
    }

    void UnrealSession::Reset()
    {
        {
            std::lock_guard lock(m_lock);
            m_runtime.reset();
            m_arrays.clear();
        }
        m_state = m_stopped ? UnrealState::Stopped : UnrealState::Idle;
        m_failure.clear();
        m_symbols.clear();
    }

    void UnrealSession::ProcessOpened()
    {
        CancelWork();
        Reset();
        if (!m_stopped && m_config.AutoStart)
            Start(true);
    }

    void UnrealSession::ProcessClosed()
    {
        CancelWork();
        Reset();
    }

    void UnrealSession::CancelWork()
    {
        m_work.CancelGroup(kWorkGroup);
        m_work.CompleteGroup(kWorkGroup);
    }

    void UnrealSession::Start(bool automatic)
    {
        if (Resolving())
            return;
        const std::shared_ptr<Target::Process> process = m_target.Process();
        if (!process)
        {
            if (!automatic)
                m_notify(LogLevel::Error, Status::Fail(ErrorCode::NoProcess).Text);
            return;
        }
        if (!automatic)
            m_stopped = false;
        if (m_state == UnrealState::Ready)
            DropCoreSymbols();
        Reset();
        m_state = UnrealState::Resolving;
        const std::uint64_t generation = m_target.Generation();
        m_generation = generation;
        const Target::UnrealConfig config = m_config;
        m_work.Start({ .Title = "Reading the Unreal Engine reflection", .Group = kWorkGroup, .Cancellable = true, .Quiet = automatic },
            [this, process, config, automatic, generation](WorkProgress& progress) -> BackgroundWork::Finish
            {
                auto runtime = std::make_shared<Target::UnrealRuntime>();
                const Status status = runtime->Resolve(process, config, !automatic, &progress);
                const bool cancelled = progress.Cancelled();
                return [this, runtime, status, automatic, generation, cancelled]()
                    {
                        if (cancelled)
                        {
                            if (generation == m_target.Generation())
                                Reset();
                            return;
                        }
                        Finish(runtime, status, automatic, generation);
                    };
            });
    }

    Status UnrealSession::ResolveNow()
    {
        const std::shared_ptr<Target::Process> process = m_target.Process();
        if (!process)
            return Status::Fail(ErrorCode::NoProcess);
        CancelWork();
        m_stopped = false;
        if (m_state == UnrealState::Ready)
            DropCoreSymbols();
        Reset();
        m_state = UnrealState::Resolving;
        m_generation = m_target.Generation();
        auto runtime = std::make_shared<Target::UnrealRuntime>();
        const Status status = runtime->Resolve(process, m_config, true, nullptr);
        Finish(runtime, status, false, m_generation);
        return status;
    }

    void UnrealSession::Finish(std::shared_ptr<Target::UnrealRuntime> runtime, const Status& status, bool automatic, std::uint64_t generation)
    {
        if (generation != m_target.Generation() || !m_target.Process() || m_stopped)
            return;
        if (!status)
        {
            m_failure = status.Text;
            if (automatic && runtime->Rejected())
            {
                m_state = UnrealState::NotUnreal;
                LOG_DEBUG("Unreal", status.Text);
                return;
            }
            m_state = UnrealState::Failed;
            LOG_WARN("Unreal", status.Text);
            m_notify(LogLevel::Warning, status.Text);
            return;
        }
        {
            std::lock_guard lock(m_lock);
            m_runtime = runtime;
        }
        m_state = UnrealState::Ready;
        RegisterCoreSymbols();
        m_target.RefreshSymbolProvider();
        const Target::UnrealSummary& summary = runtime->Summary();
        m_notify(LogLevel::Success, Format("Unreal Engine %s: %s objects, %s types (%.1f s)", summary.LegacyNames ? "4.22 or older" : "4.23+",
            Grouped(static_cast<std::uint64_t>(summary.ObjectCount)).c_str(), Grouped(summary.TypeCount).c_str(), summary.Seconds));
        if (m_ready)
            m_ready();
    }

    void UnrealSession::Stop()
    {
        CancelWork();
        DropCoreSymbols();
        m_stopped = true;
        Reset();
        m_target.RefreshSymbolProvider();
        LOG_INFO("Unreal", "Shut down. Extensions > Unreal Engine > Start, or manifold.unreal.start(), turns it on again.");
    }

    void UnrealSession::RegisterCoreSymbols()
    {
        const auto runtime = Runtime();
        if (!runtime || !m_target.Process())
            return;
        const Target::UnrealSummary& summary = runtime->Summary();
        const std::pair<std::string, std::uint64_t> symbols[] = { { "GObjects", summary.Objects }, { summary.LegacyNames ? "GNames" : "FNamePool", summary.NamePool },
            { "GameEngine", runtime->EngineObject() } };
        for (const auto& [name, value] : symbols)
        {
            if (value && m_target.Symbols().Register(name, value, "unreal"))
                m_symbols.push_back(name);
        }
    }

    void UnrealSession::DropCoreSymbols()
    {
        for (const std::string& name : m_symbols)
            m_target.Symbols().Unregister(name);
        m_symbols.clear();
    }

    std::string UnrealSession::StatusText() const
    {
        switch (m_state)
        {
        case UnrealState::Stopped: return "shut down";
        case UnrealState::Resolving: return "reading the reflection...";
        case UnrealState::NotUnreal: return "not an Unreal Engine game";
        case UnrealState::Failed: return "not resolved: " + m_failure;
        case UnrealState::Ready:
            if (const auto runtime = Runtime())
            {
                const Target::UnrealSummary& summary = runtime->Summary();
                return Format("%s · %s objects · %s types · %s", summary.LegacyNames ? "UE 4.22 or older" : "UE 4.23+", Grouped(static_cast<std::uint64_t>(runtime->Count())).c_str(),
                    Grouped(summary.TypeCount).c_str(), summary.Layout.Component == 8 ? "double vectors (UE5)" : "float vectors");
            }
            return "not resolved";
        default:
            return m_target.Process() ? "not started" : "no process";
        }
    }

    void UnrealSession::RememberArrays(const std::vector<std::pair<std::uint64_t, UnrealArray>>& arrays)
    {
        std::lock_guard lock(m_lock);
        if (m_arrays.size() + arrays.size() > kArrayMemory)
            m_arrays.clear();
        for (const auto& [data, array] : arrays)
            m_arrays[data] = array;
    }

    std::optional<UnrealArray> UnrealSession::ArrayAt(std::uint64_t data) const
    {
        std::optional<UnrealArray> found;
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_arrays.find(data); it != m_arrays.end())
                found = it->second;
        }
        const auto process = m_target.Process();
        if (!found || !process || process->ReadPointer(found->Header) != data)
            return std::nullopt;
        return found;
    }

    std::optional<std::uint64_t> UnrealSession::ResolveSymbol(std::string_view name) const
    {
        if (!m_config.Symbols)
            return std::nullopt;
        const auto runtime = Runtime();
        if (!runtime)
            return std::nullopt;
        return runtime->ResolveSymbol(name);
    }
}
