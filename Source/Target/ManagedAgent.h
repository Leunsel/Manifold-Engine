#pragma once

#include "Core/Types.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class MonoBridge;
    class Process;

    namespace AgentProtocol
    {
        constexpr std::uint32_t kMagic = 0x4741464Du;
        constexpr std::uint32_t kVersion = 3;

        constexpr std::uint32_t kOffMagic = 0;
        constexpr std::uint32_t kOffVersion = 4;
        constexpr std::uint32_t kOffRequestSeq = 8;
        constexpr std::uint32_t kOffDoneSeq = 12;
        constexpr std::uint32_t kOffRequest = 16;
        constexpr std::uint32_t kOffRequestCapacity = 24;
        constexpr std::uint32_t kOffRequestSize = 28;
        constexpr std::uint32_t kOffResponse = 32;
        constexpr std::uint32_t kOffResponseSize = 40;
        constexpr std::uint32_t kOffHeartbeat = 44;
        constexpr std::uint32_t kOffFrames = 48;
        constexpr std::uint32_t kOffState = 52;
        constexpr std::uint32_t kOffBusy = 56;
        constexpr std::uint32_t kControlSize = 64;

        constexpr int kStateIdle = 0;
        constexpr int kStateHooked = 1;
        constexpr int kStateFailed = 2;

        constexpr std::uint64_t kArrayData = 0x20;
        constexpr std::uint32_t kSlotIndexMask = 0xFFFFu;

        enum Op : std::uint32_t
        {
            Info = 1,
            Scenes = 2,
            Roots = 3,
            Children = 4,
            GameObject = 5,
            UnityObject = 6,
            Find = 7,
            Invoke = 8,
            Fields = 9,
            SetField = 10,
            SetActive = 11,
            SetEnabled = 12,
            SetName = 13,
            SetVector = 14,
            RunInBackground = 15,
            Release = 16,
            Native = 17,
            Stop = 18,
            Elements = 19,
            Hold = 20,
        };

        enum ArgKind : std::uint8_t
        {
            ArgNull = 0,
            ArgSlot = 1,
            ArgBytes = 2,
            ArgText = 3,
        };

        enum ValueKind : std::uint8_t
        {
            ValueNone = 0,
            ValueBytes = 1,
            ValueText = 2,
            ValueObject = 3,
        };
    }

    class AgentRequest
    {
    public:
        AgentRequest& Op(std::uint32_t op);
        AgentRequest& I32(std::int32_t value);
        AgentRequest& U32(std::uint32_t value);
        AgentRequest& I64(std::int64_t value);
        AgentRequest& F32(float value);
        AgentRequest& U8(std::uint8_t value);
        AgentRequest& Bytes(std::span<const std::uint8_t> value);
        AgentRequest& Text(std::string_view value);

        std::size_t Count() const { return m_count; }
        std::vector<std::uint8_t> Finish();

    private:
        void Put(const void* data, std::size_t size);
        void Close();

        std::vector<std::uint8_t> m_body;
        std::size_t m_open = 0;
        bool m_opened = false;
        std::uint32_t m_count = 0;
    };

    struct AgentReply
    {
        std::uint32_t Op = 0;
        std::uint32_t Status = 0;
        std::span<const std::uint8_t> Payload;

        bool Ok() const { return Status == 0; }
        std::string Error() const;
    };

    Result<std::vector<AgentReply>> ParseAgentReplies(std::span<const std::uint8_t> response);

    class AgentCursor
    {
    public:
        explicit AgentCursor(std::span<const std::uint8_t> data) : m_data(data) {}

        bool Ok() const { return m_ok; }
        std::int32_t I32();
        std::uint32_t U32() { return static_cast<std::uint32_t>(I32()); }
        std::int64_t I64();
        float F32();
        std::uint8_t U8();
        std::vector<std::uint8_t> Bytes();
        std::string Text();

    private:
        bool Take(void* out, std::size_t size);

        std::span<const std::uint8_t> m_data;
        std::size_t m_at = 0;
        bool m_ok = true;
    };

    class AgentLink
    {
    public:
        explicit AgentLink(MonoBridge& mono) : m_mono(mono) {}

        Status Connect();
        bool Connected() const { return m_mailbox != 0; }
        int RemoteState() const;
        std::uint64_t Frames() const;

        struct Exchanged
        {
            std::vector<std::uint8_t> Bytes;
            std::vector<AgentReply> Replies;
            std::uint64_t Frames = 0;
        };
        Result<Exchanged> Exchange(const std::vector<std::uint8_t>& request, std::chrono::milliseconds timeout);

        std::map<int, std::uint64_t> Addresses(const std::vector<int>& slots) const;
        void Heartbeat();
        void Stop();
        void Forget();

    private:
        std::optional<std::uint64_t> StaticValue(std::uint64_t field, std::size_t bytes);
        std::optional<std::uint64_t> InvokeStatic(std::uint64_t method);
        std::string RemoteError();
        std::uint32_t Read32(std::uint32_t offset) const;
        void Write32(std::uint32_t offset, std::uint32_t value) const;

        MonoBridge& m_mono;
        std::shared_ptr<Process> m_process;
        std::uint64_t m_class = 0;
        std::uint64_t m_vtable = 0;
        std::uint64_t m_begin = 0;
        std::uint64_t m_stopNow = 0;
        std::uint64_t m_mailboxField = 0;
        std::uint64_t m_slotsField = 0;
        std::uint64_t m_errorField = 0;
        std::uint64_t m_mailbox = 0;
        std::uint64_t m_request = 0;
        std::uint32_t m_capacity = 0;
        std::uint64_t m_slots = 0;
        std::uint32_t m_seq = 0;
        std::uint32_t m_heartbeat = 0;
    };
}
