#include "Core/SafeFile.h"

#include <algorithm>
#include <atomic>
#include <string>

#include <windows.h>

namespace Manifold
{
    bool WriteFileSafely(const std::filesystem::path& path, std::string_view bytes, bool replace)
    {
        static std::atomic<unsigned long> s_writes{ 0 };
        std::filesystem::path temporary = path;
        temporary += L"." + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++s_writes) + L".tmp";
        const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        bool ok = true;
        std::size_t at = 0;
        while (ok && at < bytes.size())
        {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - at, 16u * 1024u * 1024u));
            DWORD written = 0;
            ok = ::WriteFile(file, bytes.data() + at, chunk, &written, nullptr) && written == chunk;
            at += written;
        }
        // ∑ Without the flush the rename can reach the disk before the data: after a crash or power loss the old good file is
        //   gone and the new one is empty or cut short.
        ok = ok && ::FlushFileBuffers(file);
        ok = ::CloseHandle(file) && ok;
        if (ok)
            ok = ::MoveFileExW(temporary.c_str(), path.c_str(), (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok)
            ::DeleteFileW(temporary.c_str());
        return ok;
    }
}
