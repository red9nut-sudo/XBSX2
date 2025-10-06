// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"

// Required for XAudio2 + MediaFoundation
#include <xaudio2.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <shcore.h>
#include <ppltasks.h>
#include <string>
#include <vector>
#include <Windows.h>
#include <VersionHelpers.h>
#include <gamingdeviceinformation.h>

// ✅ This links XAudio2 automatically — NO manual download needed
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;

// If anything tries to read this as an initializer, we're in trouble.
static const LARGE_INTEGER lfreq = []() {
    LARGE_INTEGER ret = {};
    QueryPerformanceFrequency(&ret);
    return ret;
}();

// This gets leaked... oh well.
static thread_local HANDLE s_sleep_timer;
static thread_local bool s_sleep_timer_created = false;

static HANDLE GetSleepTimer()
{
    if (s_sleep_timer_created)
        return s_sleep_timer;

    s_sleep_timer_created = true;
    s_sleep_timer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_sleep_timer)
        s_sleep_timer = CreateWaitableTimer(nullptr, TRUE, nullptr);

    return s_sleep_timer;
}

u64 GetTickFrequency()
{
    return lfreq.QuadPart;
}

u64 GetCPUTicks()
{
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return count.QuadPart;
}

u64 GetPhysicalMemory()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return status.ullTotalPhys;
}

u64 GetAvailablePhysicalMemory()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return status.ullAvailPhys;
}

std::string GetOSVersionString()
{
    std::string retval;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);

    if (IsWindows10OrGreater())
    {
        retval = "Microsoft ";
        retval += IsWindowsServer() ? "Windows Server 2016+" : "Windows 10+";
    }
    else
        retval = "Unsupported Operating System!";

    return retval;
}

std::string GetConsoleModelString()
{
    std::string model;
    GAMING_DEVICE_MODEL_INFORMATION deviceInfo = {};
    HRESULT hr = GetGamingDeviceModelInformation(&deviceInfo);

    if (SUCCEEDED(hr))
    {
        switch (deviceInfo.deviceId)
        {
            case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
                model = "Xbox One";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
                model = "Xbox One S";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
                model = "Xbox One X";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
                model = "Xbox One X Developer Kit";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
                model = "Xbox Series S";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
                model = "Xbox Series X";
                break;
            case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
                model = "Xbox Series X Developer Kit";
                break;
            default:
                model = "Unknown Xbox model";
                break;
        }
    }
    else
    {
        model = "Error detecting console model";
    }
    return model;
}

bool Common::InhibitScreensaver(bool inhibit)
{
    EXECUTION_STATE flags = ES_CONTINUOUS;
    if (inhibit)
        flags |= ES_DISPLAY_REQUIRED;
    SetThreadExecutionState(flags);
    return true;
}

void Common::SetMousePosition(int x, int y)
{
    SetCursorPos(x, y);
}

bool Common::PlaySoundAsync(const char* filename)
{
    static ComPtr<IXAudio2> xaudio2;
    static IXAudio2MasteringVoice* masteringVoice = nullptr;
    static std::mutex audioMutex;
    std::lock_guard<std::mutex> lock(audioMutex);

    // Initialize XAudio2 if needed
    if (!xaudio2)
    {
        HRESULT hr = XAudio2Create(xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr))
        {
            Console.Error("Failed to init XAudio2 engine. Error Details: %08X", hr);
            return false;
        }

        hr = xaudio2->CreateMasteringVoice(&masteringVoice);
        if (FAILED(hr))
        {
            Console.Error("XAudio2 CreateMasteringVoice failure: %08X", hr);
            xaudio2.Reset();
            return false;
        }
    }

    // Initialize MediaFoundation
    if (FAILED(MFStartup(MF_VERSION)))
    {
        Console.Error("MFStartup failed");
        return false;
    }

    // Convert filename to wide string
    std::wstring wfilename(filename, filename + strlen(filename));

    // Create source reader
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wfilename.c_str(), nullptr, &reader)))
    {
        Console.Error("Failed to create source reader for: %s", filename);
        return false;
    }

    // Get audio format
    ComPtr<IMFMediaType> nativeType;
    if (FAILED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &nativeType)))
        return false;

    UINT32 size = 0;
    WAVEFORMATEX* wfx = nullptr;
    if (FAILED(MFCreateWaveFormatExFromMFMediaType(nativeType.Get(), &wfx, &size)))
        return false;

    // Read audio data
    std::vector<BYTE> audioData;
    while (true)
    {
        DWORD streamIndex, flags;
        LONGLONG timestamp;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;

        if (sample)
        {
            ComPtr<IMFMediaBuffer> buffer;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE* data = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength)))
                {
                    audioData.insert(audioData.end(), data, data + currentLength);
                    buffer->Unlock();
                }
            }
        }
    }

    if (audioData.empty())
    {
        CoTaskMemFree(wfx);
        return false;
    }

    // Create source voice
    IXAudio2SourceVoice* sourceVoice = nullptr;
    if (FAILED(xaudio2->CreateSourceVoice(&sourceVoice, wfx)))
    {
        Console.Error("XAudio2 CreateSourceVoice failure: %08X");
        CoTaskMemFree(wfx);
        return false;
    }

    // Submit buffer
    XAUDIO2_BUFFER buffer = {};
    buffer.pAudioData = audioData.data();
    buffer.AudioBytes = static_cast<UINT32>(audioData.size());
    buffer.Flags = XAUDIO2_END_OF_STREAM;

    if (FAILED(sourceVoice->SubmitSourceBuffer(&buffer)))
    {
        sourceVoice->DestroyVoice();
        CoTaskMemFree(wfx);
        return false;
    }

    // Start playback
    if (FAILED(sourceVoice->Start(0)))
    {
        sourceVoice->DestroyVoice();
        CoTaskMemFree(wfx);
        return false;
    }

    CoTaskMemFree(wfx);

    // In production: wait for playback, then DestroyVoice
    // For now, we let it play and clean up on exit

    return true;
}

void Threading::Sleep(int ms)
{
    ::Sleep(ms);
}

void Threading::SleepUntil(u64 ticks)
{
    const s64 diff = static_cast<s64>(ticks - GetCPUTicks());
    if (diff <= 0)
        return;

    const HANDLE hTimer = GetSleepTimer();
    if (!hTimer)
        return;

    const u64 one_hundred_nanos_diff = (static_cast<u64>(diff) * 10000000ULL) / GetTickFrequency();
    if (one_hundred_nanos_diff == 0)
        return;

    LARGE_INTEGER fti;
    fti.QuadPart = -static_cast<s64>(one_hundred_nanos_diff);
    if (SetWaitableTimer(hTimer, &fti, 0, nullptr, nullptr, FALSE))
    {
        WaitForSingleObject(hTimer, INFINITE);
        return;
    }
}

// === STUBS FOR UWP ===
bool Common::AttachMousePositionCb(std::function<void(int, int)> cb) { return false; }
void Common::DetachMousePositionCb() {}

namespace Achievements {
    void SwitchToRAIntegration() {}
    namespace RAIntegration {
        void ActivateMenuItem(int) {}
        std::vector<std::tuple<int, std::string, bool>> GetMenuItems() { return {}; }
        void MainWindowChanged(void*) {}
    }
}
