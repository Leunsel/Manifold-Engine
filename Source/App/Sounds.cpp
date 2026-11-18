#include "App/Sounds.h"

#include "App/Settings.h"
#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Resource.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")

namespace Manifold::Sounds
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        struct Slot
        {
            std::vector<std::uint8_t> Wave;
            bool Loaded = false;
        };

        std::mutex g_lock;
        std::array<Slot, 2> g_slots;
        std::thread g_loader;
        bool g_loading = false;
        int g_wanted = -1;
        std::atomic<bool> g_enabled{ false };

        std::size_t Index(Cue cue)
        {
            return cue == Cue::Success ? 0 : 1;
        }

        std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        std::vector<std::uint8_t> Embedded(Cue cue)
        {
            HMODULE module = ::GetModuleHandleW(nullptr);
            HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(cue == Cue::Success ? IDR_SOUND_SUCCESS : IDR_SOUND_ERROR), RT_RCDATA);
            if (!resource)
                return {};
            HGLOBAL handle = ::LoadResource(module, resource);
            const auto* data = handle ? static_cast<const std::uint8_t*>(::LockResource(handle)) : nullptr;
            const DWORD size = ::SizeofResource(module, resource);
            return data ? std::vector<std::uint8_t>(data, data + size) : std::vector<std::uint8_t>();
        }

        bool IsWave(const std::vector<std::uint8_t>& bytes)
        {
            return bytes.size() > 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0;
        }

        std::vector<std::uint8_t> Load(Cue cue)
        {
            const std::filesystem::path folder = Paths::SoundsFolder();
            const std::string name = FileName(cue);
            std::error_code error;
            for (const char* extension : { ".wav", ".mp3" })
            {
                const std::filesystem::path path = folder / (name + extension);
                if (!std::filesystem::exists(path, error))
                    continue;
                std::vector<std::uint8_t> bytes = ReadAll(path);
                if (IsWave(bytes))
                    return bytes;
                std::vector<std::uint8_t> wave = DecodeToWave(bytes);
                if (!wave.empty())
                    return wave;
                LOG_WARN("Sounds", "Could not read " + path.string() + "; the built-in sound is used instead");
            }
            const std::vector<std::uint8_t> embedded = Embedded(cue);
            if (IsWave(embedded))
                return embedded;
            std::vector<std::uint8_t> wave = DecodeToWave(embedded);
            if (!embedded.empty() && wave.empty())
                LOG_WARN("Sounds", "The built-in sound could not be decoded. Windows needs its media features (Media Foundation) for MP3; a Success.wav in the sounds folder works without them.");
            return wave;
        }

        void Start(const std::vector<std::uint8_t>& wave)
        {
            if (!wave.empty())
                ::PlaySoundW(reinterpret_cast<LPCWSTR>(wave.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }

        void LoadAll()
        {
            SetThreadName(L"Sound loader");
            std::array<std::vector<std::uint8_t>, 2> loaded{ Load(Cue::Success), Load(Cue::Error) };
            std::lock_guard lock(g_lock);
            for (std::size_t i = 0; i < loaded.size(); ++i)
            {
                g_slots[i].Wave = std::move(loaded[i]);
                g_slots[i].Loaded = true;
            }
            g_loading = false;
            if (g_wanted >= 0)
                Start(g_slots[static_cast<std::size_t>(g_wanted)].Wave);
            g_wanted = -1;
        }

        void Request(Cue cue, bool reload)
        {
            std::lock_guard lock(g_lock);
            Slot& slot = g_slots[Index(cue)];
            if (slot.Loaded && !reload)
            {
                Start(slot.Wave);
                return;
            }
            g_wanted = static_cast<int>(Index(cue));
            if (g_loading)
                return;
            if (g_loader.joinable())
                g_loader.join();
            if (reload)
            {
                ::PlaySoundW(nullptr, nullptr, 0);
                for (Slot& each : g_slots)
                    each = {};
            }
            g_loading = true;
            g_loader = std::thread(&LoadAll);
        }

        void Append(std::vector<std::uint8_t>& out, const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        void Append32(std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            Append(out, &value, sizeof(value));
        }
    }

    void SetEnabled(bool enabled)
    {
        g_enabled = enabled;
    }

    bool Enabled()
    {
        return g_enabled;
    }

    const char* FileName(Cue cue)
    {
        return cue == Cue::Success ? "Success" : "Error";
    }

    void Play(Cue cue)
    {
        if (!g_enabled)
            return;
        static std::array<std::chrono::steady_clock::time_point, 2> last{};
        const auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point& previous = last[Index(cue)];
        if (previous != std::chrono::steady_clock::time_point{} && now - previous < std::chrono::seconds(1))
            return;
        previous = now;
        Request(cue, false);
    }

    void Preview(Cue cue)
    {
        Request(cue, true);
    }

    void Stop()
    {
        std::thread loader;
        {
            std::lock_guard lock(g_lock);
            g_wanted = -1;
            loader = std::move(g_loader);
        }
        if (loader.joinable())
            loader.join();
        ::PlaySoundW(nullptr, nullptr, 0);
    }

    std::vector<std::uint8_t> DecodeToWave(const std::vector<std::uint8_t>& encoded)
    {
        if (encoded.empty())
            return {};
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::uint8_t> pcm;
        std::vector<std::uint8_t> format;
        if (SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
            ComPtr<IStream> stream;
            stream.Attach(::SHCreateMemStream(encoded.data(), static_cast<UINT>(encoded.size())));
            ComPtr<IMFByteStream> bytes;
            ComPtr<IMFSourceReader> reader;
            ComPtr<IMFMediaType> wanted;
            ComPtr<IMFMediaType> actual;
            if (stream && SUCCEEDED(::MFCreateMFByteStreamOnStream(stream.Get(), &bytes)))
            {
                ComPtr<IMFAttributes> attributes;
                if (SUCCEEDED(bytes.As(&attributes)))
                {
                    attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"audio/mpeg");
                    attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME, L"sound.mp3");
                }
            }
            if (bytes && SUCCEEDED(::MFCreateSourceReaderFromByteStream(bytes.Get(), nullptr, &reader)) && SUCCEEDED(::MFCreateMediaType(&wanted)))
            {
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
                wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                WAVEFORMATEX* wave = nullptr;
                UINT32 waveSize = 0;
                if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, wanted.Get())) &&
                    SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual)) &&
                    SUCCEEDED(::MFCreateWaveFormatExFromMFMediaType(actual.Get(), &wave, &waveSize)) && wave)
                {
                    Append(format, wave, 16);
                    ::CoTaskMemFree(wave);
                    for (;;)
                    {
                        DWORD flags = 0;
                        ComPtr<IMFSample> sample;
                        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)))
                            break;
                        if (sample)
                        {
                            ComPtr<IMFMediaBuffer> buffer;
                            BYTE* data = nullptr;
                            DWORD length = 0;
                            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
                            {
                                Append(pcm, data, length);
                                buffer->Unlock();
                            }
                        }
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                            break;
                    }
                }
            }
            reader.Reset();
            bytes.Reset();
            stream.Reset();
            ::MFShutdown();
        }
        if (SUCCEEDED(com))
            ::CoUninitialize();
        if (pcm.empty() || format.size() != 16)
            return {};
        std::vector<std::uint8_t> out;
        out.reserve(44 + pcm.size());
        Append(out, "RIFF", 4);
        Append32(out, static_cast<std::uint32_t>(4 + 8 + format.size() + 8 + pcm.size()));
        Append(out, "WAVE", 4);
        Append(out, "fmt ", 4);
        Append32(out, static_cast<std::uint32_t>(format.size()));
        out.insert(out.end(), format.begin(), format.end());
        Append(out, "data", 4);
        Append32(out, static_cast<std::uint32_t>(pcm.size()));
        out.insert(out.end(), pcm.begin(), pcm.end());
        return out;
    }
}
