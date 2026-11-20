#pragma once

#include "App/TargetSession.h"
#include "Target/Rtti.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace Manifold
{
    struct Settings;

    class RttiSession
    {
    public:
        using Claims = std::function<bool(std::uint64_t address)>;

        RttiSession(TargetSession& target, Settings& settings);

        RttiSession(const RttiSession&) = delete;
        RttiSession& operator=(const RttiSession&) = delete;

        std::shared_ptr<Target::RttiRuntime> Runtime();
        void ProcessOpened();
        void ProcessClosed();
        void Reset();

        bool Enabled() const;
        void SetEnabled(bool enabled);
        bool Dissects() const { return Enabled() && m_target.Attached(); }
        std::string StatusText();

        void SetOtherEngines(Claims claims) { m_others = std::move(claims); }
        bool OthersClaim(std::uint64_t address) const { return m_others && m_others(address); }
        bool Names(std::uint64_t address);
        std::optional<std::uint64_t> ResolveSymbol(std::string_view name);

    private:
        static constexpr const char* kSymbolOwner = "rtti";

        void Drop();

        TargetSession& m_target;
        Settings& m_settings;
        Claims m_others;

        std::mutex m_lock;
        std::shared_ptr<Target::RttiRuntime> m_runtime;
        std::shared_ptr<std::atomic<bool>> m_alive;
        std::uint64_t m_generation = 0;
    };
}
