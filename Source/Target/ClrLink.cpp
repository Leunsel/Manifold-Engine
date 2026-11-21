#include "Target/ClrLink.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <atomic>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr unsigned kConnectMs = 10000;
        constexpr unsigned kOpenMs = 20000;
        constexpr DWORD kPipeBuffer = 1u << 20;

        std::atomic<unsigned> g_pipeCounter{ 0 };
    }

    std::filesystem::path ClrLink::HelperPath(bool target32)
    {
        wchar_t self[MAX_PATH * 2] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        return std::filesystem::path(self).parent_path() / (target32 ? L"ManifoldClr32.exe" : L"ManifoldClr.exe");
    }

    ClrLink::~ClrLink()
    {
        Stop();
    }

    bool ClrLink::Running() const
    {
        std::lock_guard lock(m_lock);
        return m_helper && ::WaitForSingleObject(m_helper, 0) == WAIT_TIMEOUT;
    }

    void ClrLink::EndHelper()
    {
        if (m_pipe)
        {
            ::CancelIoEx(m_pipe, nullptr);
            ::CloseHandle(m_pipe);
            m_pipe = nullptr;
        }
        if (m_helper)
        {
            // Closing the pipe ends a healthy helper on its own; one that is
            // stuck inside the DAC is ended here.
            if (::WaitForSingleObject(m_helper, 2000) == WAIT_TIMEOUT)
                ::TerminateProcess(m_helper, 1);
            ::CloseHandle(m_helper);
            m_helper = nullptr;
        }
        if (m_event)
        {
            ::CloseHandle(m_event);
            m_event = nullptr;
        }
    }

    void ClrLink::Stop()
    {
        std::lock_guard lock(m_lock);
        EndHelper();
        m_info = {};
    }

    bool ClrLink::Transfer(void* buffer, std::size_t size, bool write, unsigned timeoutMs)
    {
        auto* bytes = static_cast<std::uint8_t*>(buffer);
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        while (size)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = m_event;
            ::ResetEvent(m_event);
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 24));
            DWORD done = 0;
            const BOOL started = write ? ::WriteFile(m_pipe, bytes, chunk, &done, &overlapped) : ::ReadFile(m_pipe, bytes, chunk, &done, &overlapped);
            if (!started && ::GetLastError() != ERROR_IO_PENDING)
                return false;
            if (!started)
            {
                const ULONGLONG now = ::GetTickCount64();
                const DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
                HANDLE handles[] = { m_event, m_helper };
                const DWORD woke = ::WaitForMultipleObjects(2, handles, FALSE, wait);
                if (woke != WAIT_OBJECT_0)
                {
                    ::CancelIoEx(m_pipe, &overlapped);
                    ::GetOverlappedResult(m_pipe, &overlapped, &done, TRUE);
                    return false;
                }
                if (!::GetOverlappedResult(m_pipe, &overlapped, &done, FALSE))
                    return false;
            }
            if (done == 0)
                return false;
            bytes += done;
            size -= done;
        }
        return true;
    }

    std::optional<std::vector<std::uint8_t>> ClrLink::Call(ClrProtocol::Writer& request, unsigned timeoutMs, std::string& error)
    {
        std::lock_guard lock(m_lock);
        if (!m_pipe || !m_helper)
        {
            error = "the .NET helper is not running";
            return std::nullopt;
        }
        std::vector<std::uint8_t> message = request.Done();
        std::uint32_t size = 0;
        std::vector<std::uint8_t> reply;
        bool ok = Transfer(message.data(), message.size(), true, timeoutMs) && Transfer(&size, sizeof(size), false, timeoutMs);
        if (ok && (size == 0 || size > ClrProtocol::kMaxMessage))
            ok = false;
        if (ok)
        {
            reply.resize(size);
            ok = Transfer(reply.data(), reply.size(), false, timeoutMs);
        }
        if (!ok)
        {
            const bool exited = ::WaitForSingleObject(m_helper, 0) == WAIT_OBJECT_0;
            DWORD code = 0;
            if (exited)
                ::GetExitCodeProcess(m_helper, &code);
            error = exited ? "the .NET helper ended (exit code " + Hex(static_cast<std::uint32_t>(code), 8) + ")"
                           : Format("the .NET helper did not answer within %u ms and was ended", timeoutMs);
            LOG_WARN("Mono", error);
            EndHelper();
            return std::nullopt;
        }
        if (reply[0] != 0)
        {
            ClrProtocol::Reader in(std::span<const std::uint8_t>(reply).subspan(1));
            error = in.Text();
            if (error.empty())
                error = "the .NET helper refused the request";
            return std::nullopt;
        }
        reply.erase(reply.begin());
        return reply;
    }

    Status ClrLink::Start(const Process& process)
    {
        Stop();
        std::lock_guard lock(m_lock);
        const bool target32 = process.PointerSize() == 4;
        const std::filesystem::path helper = HelperPath(target32);
        std::error_code exists;
        if (!std::filesystem::exists(helper, exists))
            return Status::Fail(ErrorCode::ClrHelperFailed, WideToUtf8(helper.filename().wstring()) + " is missing next to the program");

        const std::wstring pipeName = L"\\\\.\\pipe\\manifold-clr-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++g_pipeCounter) + L"-"
            + std::to_wstring(::GetTickCount64());
        m_pipe = ::CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kPipeBuffer, kPipeBuffer, 0, nullptr);
        if (m_pipe == INVALID_HANDLE_VALUE)
        {
            m_pipe = nullptr;
            return Status::Fail(ErrorCode::ClrHelperFailed, "could not create its pipe (" + SystemErrorText(::GetLastError()) + ")");
        }
        m_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // The helper gets its own read-only copy of the game's handle, and
        // only that one: the handle list keeps every other inheritable handle
        // of the program out of it.
        HANDLE readOnly = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), process.Handle(), ::GetCurrentProcess(), &readOnly, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, 0))
            readOnly = nullptr;
        SIZE_T attributeSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<std::uint8_t> attributeBytes(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
        const bool listed = readOnly && ::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)
            && ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &readOnly, sizeof(readOnly), nullptr, nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = listed ? attributes : nullptr;
        std::wstring command = L"\"" + helper.wstring() + L"\" --pipe=" + pipeName;
        PROCESS_INFORMATION info{};
        const BOOL started = ::CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, listed ? TRUE : FALSE,
            CREATE_NO_WINDOW | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, helper.parent_path().c_str(), &startup.StartupInfo, &info);
        const std::string startError = started ? std::string() : SystemErrorText(::GetLastError());
        if (listed)
            ::DeleteProcThreadAttributeList(attributes);
        const std::uint64_t handleValue = listed ? reinterpret_cast<std::uintptr_t>(readOnly) : 0;
        if (readOnly)
            ::CloseHandle(readOnly);
        if (!started)
        {
            EndHelper();
            return Status::Fail(ErrorCode::ClrHelperFailed, "could not start " + WideToUtf8(helper.filename().wstring()) + " (" + startError + ")");
        }
        ::CloseHandle(info.hThread);
        m_helper = info.hProcess;

        OVERLAPPED overlapped{};
        overlapped.hEvent = m_event;
        bool connected = ::ConnectNamedPipe(m_pipe, &overlapped) != FALSE;
        if (!connected)
        {
            const DWORD reason = ::GetLastError();
            if (reason == ERROR_PIPE_CONNECTED)
                connected = true;
            else if (reason == ERROR_IO_PENDING)
            {
                HANDLE handles[] = { m_event, m_helper };
                DWORD ignored = 0;
                connected = ::WaitForMultipleObjects(2, handles, FALSE, kConnectMs) == WAIT_OBJECT_0 && ::GetOverlappedResult(m_pipe, &overlapped, &ignored, FALSE);
                if (!connected)
                {
                    ::CancelIoEx(m_pipe, &overlapped);
                    ::GetOverlappedResult(m_pipe, &overlapped, &ignored, TRUE);
                }
            }
        }
        if (!connected)
        {
            EndHelper();
            return Status::Fail(ErrorCode::ClrHelperFailed, "it did not connect");
        }

        ClrProtocol::Writer open;
        open.U8(static_cast<std::uint8_t>(ClrProtocol::Op::Open));
        open.U32(ClrProtocol::kVersion);
        open.U32(::GetProcessId(process.Handle()));
        open.U64(handleValue);
        std::string error;
        m_lock.unlock();
        const auto reply = Call(open, kOpenMs, error);
        m_lock.lock();
        if (!reply)
        {
            EndHelper();
            return Status::Fail(ErrorCode::ClrHelperFailed, error);
        }
        ClrProtocol::Reader in(*reply);
        ClrRuntimeInfo runtime;
        runtime.Flavour = in.Text();
        runtime.Version = in.Text();
        runtime.RuntimePath = in.Text();
        runtime.DacPath = in.Text();
        runtime.PointerSize = static_cast<int>(in.U32());
        runtime.TypeStatics = in.U8() != 0;
        if (!in.Ok() || runtime.PointerSize != process.PointerSize())
        {
            EndHelper();
            return Status::Fail(ErrorCode::ClrHelperFailed, "its answer to Open did not fit this game");
        }
        m_info = std::move(runtime);
        return Status::Success();
    }
}
