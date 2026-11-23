#pragma once

#include "App/TargetSession.h"
#include "Core/BackgroundWork.h"
#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Target/Unreal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Manifold::Lua
{
    class LuaEngine;
}

namespace Manifold
{
    enum class UnrealState
    {
        Idle,
        NotUnreal,
        Resolving,
        Ready,
        Failed,
        Stopped
    };

    struct UnrealArray
    {
        Target::UnrealField Field;
        std::uint64_t Header = 0;
    };

    class UnrealSession
    {
    public:
        using Notifier = std::function<void(LogLevel level, std::string text)>;

        UnrealSession(TargetSession& target, BackgroundWork& work, Notifier notify);

        UnrealSession(const UnrealSession&) = delete;
        UnrealSession& operator=(const UnrealSession&) = delete;

        void RegisterTypes(Lua::LuaEngine& lua);
        void RegisterStringType(int pointerSize);
        void ProcessOpened();
        void ProcessClosed();
        void Start(bool automatic = false);
        Status ResolveNow();
        void SetReadyHandler(std::function<void()> handler) { m_ready = std::move(handler); }
        void Stop();
        void CancelWork();
        void Tick();
        bool RetryPending() const { return m_retryAt.has_value(); }

        UnrealState State() const { return m_state; }
        bool Resolving() const { return m_work.Busy(kWorkGroup); }
        bool Ready() const { return m_state == UnrealState::Ready && Runtime() != nullptr; }
        bool Stopped() const { return m_stopped; }
        const std::string& Failure() const { return m_failure; }
        std::string StatusText() const;
        std::shared_ptr<const Target::UnrealRuntime> Runtime() const;
        std::uint64_t Generation() const { return m_generation; }

        Target::UnrealConfig& Config() { return m_config; }
        const Target::UnrealConfig& Config() const { return m_config; }
        bool Dissects() const { return m_config.Dissect && Ready(); }
        bool Names() const { return m_config.Names && Ready(); }

        void RememberArrays(const std::vector<std::pair<std::uint64_t, UnrealArray>>& arrays);
        std::optional<UnrealArray> ArrayAt(std::uint64_t data, const Target::Process* reader = nullptr) const;
        std::optional<std::uint64_t> ResolveSymbol(std::string_view name) const;

    private:
        static constexpr const char* kWorkGroup = "unreal";
        static constexpr std::size_t kArrayMemory = 20000;
        static constexpr int kRetryDelays[] = { 5, 10, 20, 30 };

        void Finish(std::shared_ptr<Target::UnrealRuntime> runtime, const Status& status, bool automatic, std::uint64_t generation);
        void Reset();
        void RegisterCoreSymbols();
        void DropCoreSymbols();
        std::string Unavailable() const;

        TargetSession& m_target;
        BackgroundWork& m_work;
        Notifier m_notify;
        std::function<void()> m_ready;
        Lua::LuaEngine* m_lua = nullptr;
        Target::UnrealConfig m_config;
        UnrealState m_state = UnrealState::Idle;
        std::string m_failure;
        bool m_stopped = false;
        std::uint64_t m_generation = 0;
        std::vector<std::string> m_symbols;
        std::size_t m_retries = 0;
        std::optional<std::chrono::steady_clock::time_point> m_retryAt;

        mutable std::mutex m_lock;
        std::shared_ptr<const Target::UnrealRuntime> m_runtime;
        std::unordered_map<std::uint64_t, UnrealArray> m_arrays;
    };
}
