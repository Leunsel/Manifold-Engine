#include "Scan/ScanStore.h"

#include "Core/Paths.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>

namespace Manifold::Scan
{
    namespace
    {
        constexpr std::size_t kWriteBuffer = 1024 * 1024;

        std::atomic<int> g_limit{ MemoryBudget::kAuto };
        std::atomic<std::uint64_t> g_used{ 0 };

        struct SystemMemory
        {
            std::uint64_t Total = 0;
            std::uint64_t Available = 0;
        };

        SystemMemory Query()
        {
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (!::GlobalMemoryStatusEx(&status))
                return {};
            return { status.ullTotalPhys, status.ullAvailPhys };
        }
    }

    namespace MemoryBudget
    {
        void SetLimit(int megabytes)
        {
            g_limit = std::max(megabytes, kAuto);
        }

        int Limit()
        {
            return g_limit.load();
        }

        std::uint64_t LimitBytes()
        {
            const int limit = g_limit.load();
            if (limit == kDiskOnly)
                return 0;
            if (limit == kAuto)
                return AutoBytes();
            return static_cast<std::uint64_t>(limit) * 1024 * 1024;
        }

        std::uint64_t AutoBytes()
        {
            return Query().Total / 2;
        }

        bool Reserve(std::uint64_t bytes)
        {
            const std::uint64_t limit = LimitBytes();
            if (limit == 0)
                return false;
            // ∑ The game and the system come first: a block is only taken while the
            //   RAM still free afterwards stays above 2 GB or an eighth of it,
            //   whatever the limit says. Without that a large scan would push the
            //   game into the page file, which is slower than the scan files.
            const SystemMemory memory = Query();
            const std::uint64_t keepFree = std::max<std::uint64_t>(2ull * 1024 * 1024 * 1024, memory.Total / 8);
            if (memory.Total && memory.Available < keepFree + bytes)
                return false;
            std::uint64_t used = g_used.load();
            do
            {
                if (used + bytes > limit)
                    return false;
            } while (!g_used.compare_exchange_weak(used, used + bytes));
            return true;
        }

        void Release(std::uint64_t bytes)
        {
            g_used.fetch_sub(std::min(bytes, g_used.load()));
        }

        std::uint64_t Used()
        {
            return g_used.load();
        }
    }

    ScanStore::~ScanStore()
    {
        if (m_file)
            ::CloseHandle(static_cast<HANDLE>(m_file));
        MemoryBudget::Release(m_reserved);
    }

    bool ScanStore::ToMemory(const std::uint8_t*& bytes, std::size_t& size)
    {
        while (size > 0 && !m_spilled)
        {
            const std::size_t at = static_cast<std::size_t>(m_ramBytes % kBlock);
            if (at == 0 && m_ramBytes == m_blocks.size() * kBlock)
            {
                if (!MemoryBudget::Reserve(kBlock))
                {
                    m_spilled = true;
                    break;
                }
                // ∑ Not zero-filled: every byte is written before it is read, and
                //   clearing 16 MB per block made small scans slower than the file.
                std::unique_ptr<std::uint8_t[]> block(new (std::nothrow) std::uint8_t[kBlock]);
                if (!block)
                {
                    MemoryBudget::Release(kBlock);
                    m_spilled = true;
                    break;
                }
                m_reserved += kBlock;
                m_blocks.push_back(std::move(block));
            }
            const std::size_t take = std::min(size, kBlock - at);
            std::memcpy(m_blocks.back().get() + at, bytes, take);
            bytes += take;
            size -= take;
            m_ramBytes += take;
        }
        return true;
    }

    bool ScanStore::Append(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        ToMemory(bytes, size);
        if (size == 0)
            return true;
        if (m_buffer.size() + size > kWriteBuffer && !Flush())
            return false;
        if (size > kWriteBuffer)
            return WriteToFile(bytes, size);
        if (m_buffer.capacity() < kWriteBuffer)
            m_buffer.reserve(kWriteBuffer);
        m_buffer.insert(m_buffer.end(), bytes, bytes + size);
        return true;
    }

    bool ScanStore::Flush()
    {
        if (m_buffer.empty())
            return true;
        const bool ok = WriteToFile(m_buffer.data(), m_buffer.size());
        m_buffer.clear();
        return ok;
    }

    bool ScanStore::OpenFile()
    {
        if (m_file)
            return true;
        static std::atomic<unsigned> counter{ 0 };
        const std::filesystem::path path = Paths::ScanFolder() /
            (std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(counter.fetch_add(1)) + L"-" + std::to_wstring(::GetTickCount64()) + L".scan");
        const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        m_file = handle;
        return true;
    }

    bool ScanStore::WriteToFile(const void* data, std::size_t size)
    {
        if (!OpenFile())
            return false;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        while (size > 0)
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(m_fileBytes);
            overlapped.OffsetHigh = static_cast<DWORD>(m_fileBytes >> 32);
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
            DWORD done = 0;
            if (!::WriteFile(static_cast<HANDLE>(m_file), bytes, want, &done, &overlapped) || done == 0)
                return false;
            bytes += done;
            size -= done;
            m_fileBytes += done;
        }
        return true;
    }

    bool ScanStore::ReadAt(std::uint64_t offset, void* buffer, std::size_t size) const
    {
        auto* out = static_cast<std::uint8_t*>(buffer);
        while (size > 0 && offset < m_ramBytes)
        {
            const std::size_t block = static_cast<std::size_t>(offset / kBlock);
            const std::size_t at = static_cast<std::size_t>(offset % kBlock);
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>({ size, kBlock - at, m_ramBytes - offset }));
            std::memcpy(out, m_blocks[block].get() + at, take);
            out += take;
            offset += take;
            size -= take;
        }
        if (size == 0)
            return true;
        if (!m_file)
            return false;
        std::uint64_t fileOffset = offset - m_ramBytes;
        while (size > 0)
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(fileOffset);
            overlapped.OffsetHigh = static_cast<DWORD>(fileOffset >> 32);
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
            DWORD got = 0;
            if (!::ReadFile(static_cast<HANDLE>(m_file), out, want, &got, &overlapped) || got == 0)
                return false;
            out += got;
            fileOffset += got;
            size -= got;
        }
        return true;
    }
}
