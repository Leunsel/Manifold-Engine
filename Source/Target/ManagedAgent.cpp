#include "Target/ManagedAgent.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace Manifold::Target
{
    namespace
    {
        namespace P = AgentProtocol;

        constexpr std::uint32_t kMaxResponse = 64u << 20;
        constexpr std::uint32_t kMaxReplies = 1u << 20;
        constexpr auto kStalled = std::chrono::seconds(3);
        constexpr auto kPoll = std::chrono::milliseconds(1);
    }

    AgentRequest& AgentRequest::Op(std::uint32_t op)
    {
        Close();
        m_open = m_body.size();
        m_opened = true;
        const std::uint32_t head[2] = { op, 0 };
        Put(head, sizeof(head));
        ++m_count;
        return *this;
    }

    AgentRequest& AgentRequest::I32(std::int32_t value)
    {
        Put(&value, sizeof(value));
        return *this;
    }

    AgentRequest& AgentRequest::U32(std::uint32_t value)
    {
        Put(&value, sizeof(value));
        return *this;
    }

    AgentRequest& AgentRequest::I64(std::int64_t value)
    {
        Put(&value, sizeof(value));
        return *this;
    }

    AgentRequest& AgentRequest::F32(float value)
    {
        Put(&value, sizeof(value));
        return *this;
    }

    AgentRequest& AgentRequest::U8(std::uint8_t value)
    {
        Put(&value, sizeof(value));
        return *this;
    }

    AgentRequest& AgentRequest::Bytes(std::span<const std::uint8_t> value)
    {
        I32(static_cast<std::int32_t>(value.size()));
        Put(value.data(), value.size());
        return *this;
    }

    AgentRequest& AgentRequest::Text(std::string_view value)
    {
        return Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    void AgentRequest::Put(const void* data, std::size_t size)
    {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        m_body.insert(m_body.end(), bytes, bytes + size);
    }

    void AgentRequest::Close()
    {
        if (!m_opened)
            return;
        const std::uint32_t length = static_cast<std::uint32_t>(m_body.size() - m_open - 8);
        std::memcpy(m_body.data() + m_open + 4, &length, sizeof(length));
        m_opened = false;
    }

    std::vector<std::uint8_t> AgentRequest::Finish()
    {
        Close();
        std::vector<std::uint8_t> out(4 + m_body.size());
        std::memcpy(out.data(), &m_count, sizeof(m_count));
        if (!m_body.empty())
            std::memcpy(out.data() + 4, m_body.data(), m_body.size());
        return out;
    }

    std::string AgentReply::Error() const
    {
        if (Ok())
            return {};
        AgentCursor cursor(Payload);
        std::string text = cursor.Text();
        return text.empty() ? std::string("the request failed inside the game") : text;
    }

    Result<std::vector<AgentReply>> ParseAgentReplies(std::span<const std::uint8_t> response)
    {
        const auto broken = [](const std::string& what) { return Status::Fail(ErrorCode::MonoCallFailed, "the agent's answer is damaged: " + what); };
        if (response.size() < 4)
            return broken("it is cut short");
        std::uint32_t count = 0;
        std::memcpy(&count, response.data(), 4);
        if (count > kMaxReplies || count > (response.size() - 4) / 12)
            return broken("it claims " + std::to_string(count) + " replies");
        std::vector<AgentReply> replies;
        replies.reserve(count);
        std::size_t at = 4;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (response.size() - at < 12)
                return broken("reply " + std::to_string(i) + " is cut short");
            std::uint32_t head[3] = {};
            std::memcpy(head, response.data() + at, sizeof(head));
            at += sizeof(head);
            if (head[2] > response.size() - at)
                return broken("reply " + std::to_string(i) + " runs past the end");
            AgentReply reply;
            reply.Op = head[0];
            reply.Status = head[1];
            reply.Payload = response.subspan(at, head[2]);
            at += head[2];
            replies.push_back(reply);
        }
        if (at != response.size())
            return broken(std::to_string(response.size() - at) + " bytes follow the last reply");
        return replies;
    }

    bool AgentCursor::Take(void* out, std::size_t size)
    {
        if (!m_ok || m_data.size() - m_at < size)
        {
            m_ok = false;
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, m_data.data() + m_at, size);
        m_at += size;
        return true;
    }

    std::int32_t AgentCursor::I32()
    {
        std::int32_t value = 0;
        Take(&value, sizeof(value));
        return value;
    }

    std::int64_t AgentCursor::I64()
    {
        std::int64_t value = 0;
        Take(&value, sizeof(value));
        return value;
    }

    float AgentCursor::F32()
    {
        float value = 0.0f;
        Take(&value, sizeof(value));
        return value;
    }

    std::uint8_t AgentCursor::U8()
    {
        std::uint8_t value = 0;
        Take(&value, sizeof(value));
        return value;
    }

    std::vector<std::uint8_t> AgentCursor::Bytes()
    {
        const std::int32_t length = I32();
        if (!m_ok || length < 0 || static_cast<std::size_t>(length) > m_data.size() - m_at)
        {
            m_ok = false;
            return {};
        }
        std::vector<std::uint8_t> value(m_data.begin() + static_cast<std::ptrdiff_t>(m_at), m_data.begin() + static_cast<std::ptrdiff_t>(m_at + static_cast<std::size_t>(length)));
        m_at += static_cast<std::size_t>(length);
        return value;
    }

    std::string AgentCursor::Text()
    {
        const std::vector<std::uint8_t> bytes = Bytes();
        return std::string(bytes.begin(), bytes.end());
    }

    std::optional<std::uint64_t> AgentLink::StaticValue(std::uint64_t field, std::size_t bytes)
    {
        const std::uint64_t out = m_mono.m_call.Scratch(8);
        if (!out || !m_vtable || !m_mono.CallFn("mono_field_static_get_value", m_vtable, field, out))
            return std::nullopt;
        std::uint64_t value = 0;
        if (!m_mono.m_process->ReadExact(out, &value, std::min<std::size_t>(bytes, 8)))
            return std::nullopt;
        return value;
    }

    std::optional<std::uint64_t> AgentLink::InvokeStatic(std::uint64_t method)
    {
        const std::uint64_t exception = m_mono.m_call.Scratch(8);
        if (!exception || !method)
            return std::nullopt;
        const auto result = m_mono.CallFn("mono_runtime_invoke", method, 0, 0, exception);
        std::uint64_t thrown = 0;
        if (!result || !m_mono.m_process->ReadExact(exception, &thrown, sizeof(thrown)) || thrown)
            return std::nullopt;
        return *result;
    }

    std::string AgentLink::RemoteError()
    {
        const auto text = StaticValue(m_errorField, 8);
        if (!text || !*text)
            return {};
        const MonoObjectLayout layout = MonoLayoutFor(m_mono.m_process->PointerSize());
        std::int32_t length = 0;
        if (!m_mono.m_process->ReadExact(*text + layout.StringLength, &length, sizeof(length)) || length <= 0)
            return {};
        std::wstring wide(static_cast<std::size_t>(std::min(length, 1024)), L'\0');
        const std::size_t got = m_mono.m_process->Read(*text + layout.StringChars, wide.data(), wide.size() * 2);
        wide.resize(got / 2);
        return WideToUtf8(wide);
    }

    Status AgentLink::Connect()
    {
        if (m_mailbox)
            return Status::Success();
        const std::uint64_t klass = m_mono.ManagedClass("Manifold.Managed", "Agent");
        if (!klass)
            return Status::Fail(ErrorCode::MonoCallFailed, "the managed agent could not be loaded into the game");
        std::lock_guard lock(m_mono.m_lock);
        if (!m_mono.m_process || !m_mono.m_present)
            return Status::Fail(ErrorCode::NoProcess);
        Process& process = *m_mono.m_process;
        RemoteCall& call = m_mono.m_call;
        const auto text = [&](std::string_view value) -> std::uint64_t
            {
                const std::uint64_t at = call.Scratch(value.size() + 1);
                if (at && !value.empty())
                    process.Write(at, value.data(), value.size());
                return at;
            };
        call.ResetScratch();
        m_vtable = m_mono.CallFn("mono_class_vtable", m_mono.m_domain, klass).value_or(0);
        m_begin = m_mono.CallFn("mono_class_get_method_from_name", klass, text("Begin"), 0).value_or(0);
        m_stopNow = m_mono.CallFn("mono_class_get_method_from_name", klass, text("StopNow"), 0).value_or(0);
        m_mailboxField = m_mono.CallFn("mono_class_get_field_from_name", klass, text("Mailbox")).value_or(0);
        m_slotsField = m_mono.CallFn("mono_class_get_field_from_name", klass, text("Slots")).value_or(0);
        m_errorField = m_mono.CallFn("mono_class_get_field_from_name", klass, text("LastError")).value_or(0);
        call.ResetScratch();
        if (!m_vtable || !m_begin || !m_stopNow || !m_mailboxField || !m_slotsField || !m_errorField)
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent in the game does not match this build");
        m_class = klass;
        m_process = m_mono.m_process;
        const MonoObjectLayout layout = MonoLayoutFor(process.PointerSize());
        const auto begun = InvokeStatic(m_begin);
        std::int32_t state = -1;
        if (begun && *begun)
            process.ReadExact(*begun + layout.ObjectHeader, &state, sizeof(state));
        const std::uint64_t mailbox = StaticValue(m_mailboxField, 8).value_or(0);
        const std::uint64_t slots = StaticValue(m_slotsField, 8).value_or(0);
        const std::string remote = RemoteError();
        call.ResetScratch();
        if (state != P::kStateHooked || !mailbox || !slots)
        {
            Forget();
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent could not hook into the game" + (remote.empty() ? std::string() : ": " + remote));
        }
        m_mailbox = mailbox;
        if (Read32(P::kOffMagic) != P::kMagic || Read32(P::kOffVersion) != P::kVersion)
        {
            Forget();
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent's mailbox in the game has another format");
        }
        m_request = process.ReadPointer(m_mailbox + P::kOffRequest).value_or(0);
        m_capacity = Read32(P::kOffRequestCapacity);
        m_slots = slots;
        m_seq = std::max(Read32(P::kOffRequestSeq), Read32(P::kOffDoneSeq));
        m_heartbeat = Read32(P::kOffHeartbeat);
        LOG_INFO("Unity", "The managed agent is in the game");
        return Status::Success();
    }

    std::uint32_t AgentLink::Read32(std::uint32_t offset) const
    {
        std::uint32_t value = 0;
        if (m_process && m_mailbox)
            m_process->ReadExact(m_mailbox + offset, &value, sizeof(value));
        return value;
    }

    void AgentLink::Write32(std::uint32_t offset, std::uint32_t value) const
    {
        if (m_process && m_mailbox)
            m_process->Write(m_mailbox + offset, &value, sizeof(value));
    }

    int AgentLink::RemoteState() const
    {
        return static_cast<int>(Read32(P::kOffState));
    }

    std::uint64_t AgentLink::Frames() const
    {
        return Read32(P::kOffFrames);
    }

    Result<AgentLink::Exchanged> AgentLink::Exchange(const std::vector<std::uint8_t>& request, std::chrono::milliseconds timeout)
    {
        if (!m_mailbox || !m_process)
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent is not in the game");
        if (!m_process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (request.size() > m_capacity)
            return Status::Fail(ErrorCode::MonoCallFailed, "the request is too large for the agent");
        if (Read32(P::kOffDoneSeq) != m_seq)
        {
            const auto wait = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (Read32(P::kOffDoneSeq) != m_seq && std::chrono::steady_clock::now() < wait)
                std::this_thread::sleep_for(kPoll);
            if (Read32(P::kOffDoneSeq) != m_seq)
                return Status::Fail(ErrorCode::MonoCallFailed, "the game has not finished the previous request yet");
        }
        if (RemoteState() != P::kStateHooked)
        {
            std::lock_guard lock(m_mono.m_lock);
            m_mono.m_call.ResetScratch();
            const auto begun = InvokeStatic(m_begin);
            std::int32_t state = -1;
            if (begun && *begun)
                m_process->ReadExact(*begun + MonoLayoutFor(m_process->PointerSize()).ObjectHeader, &state, sizeof(state));
            const std::string remote = state == P::kStateHooked ? std::string() : RemoteError();
            m_mono.m_call.ResetScratch();
            if (state != P::kStateHooked)
                return Status::Fail(ErrorCode::MonoCallFailed, "the agent could not hook into the game again" + (remote.empty() ? std::string() : ": " + remote));
        }
        if (Status written = m_process->Write(m_request, request.data(), request.size()); !written)
            return written;
        Write32(P::kOffRequestSize, static_cast<std::uint32_t>(request.size()));
        Write32(P::kOffHeartbeat, ++m_heartbeat);
        Write32(P::kOffRequestSeq, ++m_seq);

        const std::uint64_t startFrames = Frames();
        std::uint64_t seenFrames = startFrames;
        const auto started = std::chrono::steady_clock::now();
        auto progressed = started;
        while (Read32(P::kOffDoneSeq) != m_seq)
        {
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t frames = Frames();
            if (frames != seenFrames)
            {
                seenFrames = frames;
                progressed = now;
            }
            else if (now - progressed > kStalled)
                return Status::Fail(ErrorCode::MonoCallFailed, "the game is not running frames (paused, minimized or loading); turn on Keep running in the background or bring it to the front");
            if (now - started > timeout)
                return Status::Fail(ErrorCode::MonoCallFailed, "the game did not answer in time");
            if (!m_process->Alive())
                return Status::Fail(ErrorCode::NoProcess);
            std::this_thread::sleep_for(kPoll);
        }
        const std::uint64_t response = m_process->ReadPointer(m_mailbox + P::kOffResponse).value_or(0);
        const std::uint32_t size = Read32(P::kOffResponseSize);
        if (!response || size < 4 || size > kMaxResponse)
            return Status::Fail(ErrorCode::MonoCallFailed, "the agent's answer could not be read");
        Exchanged out;
        out.Bytes.resize(size);
        if (!m_process->ReadExact(response, out.Bytes.data(), out.Bytes.size()))
            return Status::Fail(ErrorCode::ReadFailed, "the agent's answer could not be read");
        Result<std::vector<AgentReply>> replies = ParseAgentReplies(out.Bytes);
        if (!replies)
            return replies.Error();
        out.Replies = std::move(*replies);
        out.Frames = Frames() - startFrames;
        return out;
    }

    std::map<int, std::uint64_t> AgentLink::Addresses(const std::vector<int>& slots) const
    {
        std::map<int, std::uint64_t> found;
        if (!m_process || !m_slots)
            return found;
        const MonoObjectLayout layout = MonoLayoutFor(m_process->PointerSize());
        for (const int slot : slots)
        {
            if (slot <= 0 || found.count(slot))
                continue;
            const std::uint64_t index = static_cast<std::uint32_t>(slot) & P::kSlotIndexMask;
            if (!index)
                continue;
            if (const auto address = m_process->ReadPointer(m_slots + layout.ArrayData + layout.Pointer * index))
                found[slot] = *address;
        }
        return found;
    }

    void AgentLink::Heartbeat()
    {
        if (m_mailbox)
            Write32(P::kOffHeartbeat, ++m_heartbeat);
    }

    void AgentLink::Stop()
    {
        if (!m_mailbox || !m_process)
        {
            Forget();
            return;
        }
        bool stopped = RemoteState() != P::kStateHooked;
        if (!stopped && m_process->Alive())
        {
            AgentRequest request;
            request.Op(P::Stop);
            const auto answered = Exchange(request.Finish(), std::chrono::milliseconds(1500));
            const auto wait = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (answered && RemoteState() == P::kStateHooked && std::chrono::steady_clock::now() < wait)
                std::this_thread::sleep_for(kPoll);
            stopped = RemoteState() != P::kStateHooked;
            if (!stopped)
            {
                std::lock_guard lock(m_mono.m_lock);
                if (m_mono.m_present)
                {
                    m_mono.m_call.ResetScratch();
                    InvokeStatic(m_stopNow);
                    m_mono.m_call.ResetScratch();
                }
                LOG_INFO("Unity", "The game ran no frame, so the agent was taken out from the worker thread");
            }
        }
        LOG_INFO("Unity", "Took the managed agent out of the game's frame callback");
        Forget();
    }

    void AgentLink::Forget()
    {
        m_process.reset();
        m_class = m_vtable = m_begin = m_stopNow = 0;
        m_mailboxField = m_slotsField = m_errorField = 0;
        m_mailbox = m_request = m_slots = 0;
        m_capacity = 0;
        m_seq = m_heartbeat = 0;
    }
}
