#include <windows.h>
#include <d2d1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <string>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "resource.h"

#pragma comment(lib, "d2d1.lib")

namespace
{
    constexpr UINT RenderTickMessage = WM_APP + 1;
    constexpr DWORD VideoStreamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    enum class ScreenSaverMode
    {
        Configure,
        Run,
        Preview,
    };

    struct AppState;

    struct MonitorWindow
    {
        AppState* owner = nullptr;
        HWND hwnd = nullptr;
        RECT monitorRect{};
        ID2D1HwndRenderTarget* renderTarget = nullptr;
        ID2D1Bitmap* frameBitmap = nullptr;
        int targetWidth = 0;
        int targetHeight = 0;
    };

    struct AppState
    {
        POINT startupCursor{};
        bool ignoreInitialMouseMove = true;
        bool shuttingDown = false;
        RECT virtualBounds{};
        std::wstring extractedVideoPath;
        IMFSourceReader* reader = nullptr;
        UINT32 frameWidth = 0;
        UINT32 frameHeight = 0;
        LONG frameStride = 0;
        WORD frameBitCount = 32;
        std::vector<BYTE> currentFrame;
        std::vector<BYTE> latestFrame;
        std::mutex frameMutex;
        uint64_t latestFrameSerial = 0;
        uint64_t presentedFrameSerial = 0;
        std::atomic<bool> stopRequested = false;
        std::atomic<bool> decodeFailed = false;
        std::thread decodeThread;
        HANDLE renderTimer = nullptr;
        HRESULT lastError = S_OK;
        std::wstring lastErrorStage;
        ID2D1Factory* d2dFactory = nullptr;
        std::vector<MonitorWindow> windows;
    };

    std::wstring HResultToString(HRESULT hr)
    {
        wchar_t* buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageW(
            flags,
            nullptr,
            static_cast<DWORD>(hr),
            0,
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message;
        if (length > 0 && buffer)
        {
            message.assign(buffer, length);
            while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
            {
                message.pop_back();
            }
        }
        else
        {
            message = L"Unknown error";
        }

        if (buffer)
        {
            LocalFree(buffer);
        }

        wchar_t hrText[32]{};
        wsprintfW(hrText, L" (0x%08X)", static_cast<unsigned int>(hr));
        message += hrText;
        return message;
    }

    ScreenSaverMode ParseMode()
    {
        if (__argc <= 1)
        {
            return ScreenSaverMode::Configure;
        }

        std::wstring arg = __wargv[1];
        for (auto& ch : arg)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }

        if (arg == L"/s" || arg == L"-s")
        {
            return ScreenSaverMode::Run;
        }

        if (arg == L"/p" || arg == L"-p")
        {
            return ScreenSaverMode::Preview;
        }

        return ScreenSaverMode::Configure;
    }

    BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT rect, LPARAM lParam)
    {
        auto* monitors = reinterpret_cast<std::vector<RECT>*>(lParam);
        if (!monitors || !rect)
        {
            return FALSE;
        }

        monitors->push_back(*rect);
        return TRUE;
    }

    RECT ComputeVirtualBounds(const std::vector<RECT>& monitors)
    {
        RECT bounds{};

        if (monitors.empty())
        {
            bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            bounds.right = bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            bounds.bottom = bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            return bounds;
        }

        bounds = monitors.front();
        for (const auto& rect : monitors)
        {
            bounds.left = std::min(bounds.left, rect.left);
            bounds.top = std::min(bounds.top, rect.top);
            bounds.right = std::max(bounds.right, rect.right);
            bounds.bottom = std::max(bounds.bottom, rect.bottom);
        }

        return bounds;
    }

    int VirtualWidth(const AppState& state)
    {
        return state.virtualBounds.right - state.virtualBounds.left;
    }

    int VirtualHeight(const AppState& state)
    {
        return state.virtualBounds.bottom - state.virtualBounds.top;
    }

    std::wstring CreateTempVideoPath()
    {
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);

        std::filesystem::path base = std::filesystem::path(tempPath) / L"NativeVideoScrSaver";
        std::filesystem::create_directories(base);
        return (base / L"packaged-video.mp4").wstring();
    }

    bool ExtractPackagedVideo(std::wstring& outputPath)
    {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PACKAGED_VIDEO), RT_RCDATA);
        if (!resource)
        {
            return false;
        }

        HGLOBAL resourceData = LoadResource(nullptr, resource);
        if (!resourceData)
        {
            return false;
        }

        void* bytes = LockResource(resourceData);
        DWORD size = SizeofResource(nullptr, resource);
        if (!bytes || size == 0)
        {
            return false;
        }

        outputPath = CreateTempVideoPath();
        HANDLE file = CreateFileW(
            outputPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
        CloseHandle(file);

        return ok && written == size;
    }

    bool SeekToStart(AppState& state)
    {
        PROPVARIANT position{};
        HRESULT hr = InitPropVariantFromInt64(0, &position);
        if (FAILED(hr))
        {
            state.lastErrorStage = L"Create seek position";
            state.lastError = hr;
            return false;
        }

        state.lastErrorStage = L"Seek to start";
        hr = state.reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        hr = state.reader->Flush(VideoStreamIndex);
        if (FAILED(hr))
        {
            state.lastErrorStage = L"Flush reader after seek";
            state.lastError = hr;
            return false;
        }

        return true;
    }

    bool CopySampleToBuffer(AppState& state, IMFSample* sample, std::vector<BYTE>& targetBuffer)
    {
        IMFMediaBuffer* buffer = nullptr;
        BYTE* source = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;

        state.lastErrorStage = L"Convert sample to contiguous buffer";
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        state.lastErrorStage = L"Lock media buffer";
        hr = buffer->Lock(&source, &maxLength, &currentLength);
        if (FAILED(hr))
        {
            state.lastError = hr;
            buffer->Release();
            return false;
        }

        const size_t bytesNeeded = static_cast<size_t>(state.frameStride) * state.frameHeight;
        if (currentLength < bytesNeeded)
        {
            buffer->Unlock();
            buffer->Release();
            state.lastErrorStage = L"Validate buffer size";
            state.lastError = E_FAIL;
            return false;
        }

        std::copy(source, source + bytesNeeded, targetBuffer.begin());

        buffer->Unlock();
        buffer->Release();
        return true;
    }

    bool ReadNextFrame(AppState& state, std::vector<BYTE>& decodeBuffer, LONGLONG& sampleTimeOut)
    {
        while (true)
        {
            DWORD streamFlags = 0;
            IMFSample* sample = nullptr;
            LONGLONG sampleTime = 0;

            state.lastErrorStage = L"Read next video sample";
            HRESULT hr = state.reader->ReadSample(
                VideoStreamIndex,
                0,
                nullptr,
                &streamFlags,
                &sampleTime,
                &sample);

            if (FAILED(hr))
            {
                state.lastError = hr;
                if (sample)
                {
                    sample->Release();
                }
                return false;
            }

            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                if (sample)
                {
                    sample->Release();
                }

                if (!SeekToStart(state))
                {
                    return false;
                }
                continue;
            }

            if (!sample)
            {
                continue;
            }

            sampleTimeOut = sampleTime;
            const bool ok = CopySampleToBuffer(state, sample, decodeBuffer);
            sample->Release();
            return ok;
        }
    }

    bool InitializeReader(AppState& state)
    {
        state.lastErrorStage = L"Extract embedded video";
        if (!ExtractPackagedVideo(state.extractedVideoPath))
        {
            state.lastError = HRESULT_FROM_WIN32(GetLastError());
            return false;
        }

        IMFAttributes* attributes = nullptr;
        IMFMediaType* outputType = nullptr;
        IMFMediaType* currentType = nullptr;

        state.lastErrorStage = L"Create reader attributes";
        HRESULT hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        state.lastErrorStage = L"Create source reader";
        hr = MFCreateSourceReaderFromURL(state.extractedVideoPath.c_str(), attributes, &state.reader);
        attributes->Release();
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        state.lastErrorStage = L"Select video stream";
        hr = state.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        hr = state.reader->SetStreamSelection(VideoStreamIndex, TRUE);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        const GUID candidateSubtypes[] =
        {
            MFVideoFormat_RGB32,
            MFVideoFormat_ARGB32,
        };

        bool mediaTypeConfigured = false;
        for (const auto& subtype : candidateSubtypes)
        {
            state.lastErrorStage = L"Create RGB output type";
            hr = MFCreateMediaType(&outputType);
            if (FAILED(hr))
            {
                state.lastError = hr;
                return false;
            }

            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, subtype);

            state.lastErrorStage = L"Configure reader output type";
            hr = state.reader->SetCurrentMediaType(VideoStreamIndex, nullptr, outputType);
            outputType->Release();
            outputType = nullptr;

            if (SUCCEEDED(hr))
            {
                mediaTypeConfigured = true;
                break;
            }
        }

        if (!mediaTypeConfigured)
        {
            state.lastError = hr;
            state.lastErrorStage = L"Configure reader output type";
            return false;
        }

        state.lastErrorStage = L"Read negotiated media type";
        hr = state.reader->GetCurrentMediaType(VideoStreamIndex, &currentType);
        if (FAILED(hr))
        {
            state.lastError = hr;
            return false;
        }

        hr = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &state.frameWidth, &state.frameHeight);
        if (FAILED(hr) || state.frameWidth == 0 || state.frameHeight == 0)
        {
            currentType->Release();
            state.lastErrorStage = L"Read frame size";
            state.lastError = FAILED(hr) ? hr : E_FAIL;
            return false;
        }

        state.frameBitCount = 32;
        LONG stride = static_cast<LONG>(state.frameWidth * 4);
        state.frameStride = stride;
        currentType->Release();

        state.currentFrame.resize(static_cast<size_t>(state.frameStride) * state.frameHeight);
        state.latestFrame.resize(static_cast<size_t>(state.frameStride) * state.frameHeight);
        return true;
    }

    void ShutdownReader(AppState& state)
    {
        state.stopRequested = true;

        if (state.decodeThread.joinable())
        {
            state.decodeThread.join();
        }

        if (state.renderTimer)
        {
            DeleteTimerQueueTimer(nullptr, state.renderTimer, INVALID_HANDLE_VALUE);
            state.renderTimer = nullptr;
        }

        if (state.reader)
        {
            state.reader->Release();
            state.reader = nullptr;
        }

        if (state.d2dFactory)
        {
            state.d2dFactory->Release();
            state.d2dFactory = nullptr;
        }

        if (!state.extractedVideoPath.empty())
        {
            DeleteFileW(state.extractedVideoPath.c_str());
        }
    }

    void ReleaseRenderResources(MonitorWindow& monitor)
    {
        if (monitor.frameBitmap)
        {
            monitor.frameBitmap->Release();
            monitor.frameBitmap = nullptr;
        }

        if (monitor.renderTarget)
        {
            monitor.renderTarget->Release();
            monitor.renderTarget = nullptr;
        }

        monitor.targetWidth = 0;
        monitor.targetHeight = 0;
    }

    bool EnsureRenderTarget(AppState& state, MonitorWindow& monitor, int width, int height)
    {
        if (!state.d2dFactory)
        {
            state.lastErrorStage = L"Create Direct2D factory";
            const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &state.d2dFactory);
            if (FAILED(hr))
            {
                state.lastError = hr;
                return false;
            }
        }

        if (!monitor.renderTarget)
        {
            state.lastErrorStage = L"Create Direct2D render target";
            const D2D1_RENDER_TARGET_PROPERTIES renderProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
                monitor.hwnd,
                D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                D2D1_PRESENT_OPTIONS_NONE);
            const HRESULT hr = state.d2dFactory->CreateHwndRenderTarget(renderProps, hwndProps, &monitor.renderTarget);
            if (FAILED(hr))
            {
                state.lastError = hr;
                return false;
            }

            // Keep a 1:1 pixel coordinate system so source/destination math matches monitor pixels.
            monitor.renderTarget->SetDpi(96.0f, 96.0f);
        }

        if (monitor.targetWidth != width || monitor.targetHeight != height)
        {
            const HRESULT hr = monitor.renderTarget->Resize(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)));
            if (FAILED(hr))
            {
                state.lastErrorStage = L"Resize Direct2D render target";
                state.lastError = hr;
                ReleaseRenderResources(monitor);
                return false;
            }

            monitor.targetWidth = width;
            monitor.targetHeight = height;
        }

        return true;
    }

    bool UpdateFrameBitmap(AppState& state, MonitorWindow& monitor)
    {
        if (!monitor.renderTarget)
        {
            state.lastErrorStage = L"Update Direct2D frame bitmap";
            state.lastError = E_POINTER;
            return false;
        }

        if (!monitor.frameBitmap)
        {
            state.lastErrorStage = L"Create Direct2D frame bitmap";
            const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            const HRESULT hr = monitor.renderTarget->CreateBitmap(
                D2D1::SizeU(state.frameWidth, state.frameHeight),
                state.currentFrame.data(),
                static_cast<UINT32>(state.frameStride),
                &properties,
                &monitor.frameBitmap);
            if (FAILED(hr))
            {
                state.lastError = hr;
                return false;
            }
            return true;
        }

        state.lastErrorStage = L"Copy frame into Direct2D bitmap";
        const HRESULT hr = monitor.frameBitmap->CopyFromMemory(nullptr, state.currentFrame.data(), static_cast<UINT32>(state.frameStride));
        if (FAILED(hr))
        {
            state.lastError = hr;
            monitor.frameBitmap->Release();
            monitor.frameBitmap = nullptr;
            return false;
        }

        return true;
    }

    bool TryPromoteLatestFrame(AppState& state)
    {
        std::lock_guard<std::mutex> lock(state.frameMutex);
        if (state.latestFrameSerial == state.presentedFrameSerial)
        {
            return false;
        }

        state.currentFrame = state.latestFrame;
        state.presentedFrameSerial = state.latestFrameSerial;
        return true;
    }

    RECT ComputeCoverSourceRect(const AppState& state)
    {
        const double srcW = static_cast<double>(state.frameWidth);
        const double srcH = static_cast<double>(state.frameHeight);
        const double dstW = static_cast<double>(VirtualWidth(state));
        const double dstH = static_cast<double>(VirtualHeight(state));

        const double srcAspect = srcW / srcH;
        const double dstAspect = dstW / dstH;

        RECT source{ 0, 0, static_cast<LONG>(state.frameWidth), static_cast<LONG>(state.frameHeight) };

        if (srcAspect > dstAspect)
        {
            const double targetWidth = srcH * dstAspect;
            const double crop = (srcW - targetWidth) / 2.0;
            source.left = static_cast<LONG>(crop);
            source.right = static_cast<LONG>(srcW - crop);
        }
        else if (srcAspect < dstAspect)
        {
            const double targetHeight = srcW / dstAspect;
            const double crop = (srcH - targetHeight) / 2.0;
            source.top = static_cast<LONG>(crop);
            source.bottom = static_cast<LONG>(srcH - crop);
        }

        return source;
    }

    RECT ComputeMonitorSourceRect(const AppState& state, const RECT& fullSourceRect, const RECT& monitorRect)
    {
        const double relLeft = static_cast<double>(monitorRect.left - state.virtualBounds.left);
        const double relTop = static_cast<double>(monitorRect.top - state.virtualBounds.top);
        const double relWidth = static_cast<double>(monitorRect.right - monitorRect.left);
        const double relHeight = static_cast<double>(monitorRect.bottom - monitorRect.top);

        const double sourceWidth = static_cast<double>(fullSourceRect.right - fullSourceRect.left);
        const double sourceHeight = static_cast<double>(fullSourceRect.bottom - fullSourceRect.top);

        RECT source{};
        source.left = static_cast<LONG>(fullSourceRect.left + (relLeft / VirtualWidth(state)) * sourceWidth);
        source.top = static_cast<LONG>(fullSourceRect.top + (relTop / VirtualHeight(state)) * sourceHeight);
        source.right = static_cast<LONG>(source.left + (relWidth / VirtualWidth(state)) * sourceWidth);
        source.bottom = static_cast<LONG>(source.top + (relHeight / VirtualHeight(state)) * sourceHeight);
        return source;
    }

    bool PaintFrameForWindow(AppState& state, MonitorWindow& monitor)
    {
        RECT clientRect{};
        GetClientRect(monitor.hwnd, &clientRect);
        const int width = clientRect.right - clientRect.left;
        const int height = clientRect.bottom - clientRect.top;

        if (!EnsureRenderTarget(state, monitor, width, height))
        {
            return false;
        }

        if (!state.currentFrame.empty() && !UpdateFrameBitmap(state, monitor))
        {
            return false;
        }

        monitor.renderTarget->BeginDraw();
        monitor.renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        if (monitor.frameBitmap)
        {
            const RECT fullSourceRect = ComputeCoverSourceRect(state);
            const RECT sourceRect = ComputeMonitorSourceRect(state, fullSourceRect, monitor.monitorRect);
            const D2D1_SIZE_F targetSize = monitor.renderTarget->GetSize();
            const D2D1_RECT_F src = D2D1::RectF(
                static_cast<float>(sourceRect.left),
                static_cast<float>(sourceRect.top),
                static_cast<float>(sourceRect.right),
                static_cast<float>(sourceRect.bottom));
            const D2D1_RECT_F dst = D2D1::RectF(0.0f, 0.0f, targetSize.width, targetSize.height);
            monitor.renderTarget->DrawBitmap(monitor.frameBitmap, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        }

        const HRESULT hr = monitor.renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            ReleaseRenderResources(monitor);
            return true;
        }

        if (FAILED(hr))
        {
            state.lastErrorStage = L"Present Direct2D frame";
            state.lastError = hr;
            return false;
        }

        return true;
    }

    void InvalidateAllWindows(AppState& state)
    {
        for (const auto& window : state.windows)
        {
            if (window.hwnd && IsWindow(window.hwnd))
            {
                InvalidateRect(window.hwnd, nullptr, FALSE);
            }
        }
    }

    void CloseAllWindows(AppState& state)
    {
        if (state.shuttingDown)
        {
            return;
        }

        state.shuttingDown = true;
        for (auto& window : state.windows)
        {
            if (window.hwnd && IsWindow(window.hwnd))
            {
                DestroyWindow(window.hwnd);
            }
        }
    }

    bool AllWindowsClosed(const AppState& state)
    {
        for (const auto& window : state.windows)
        {
            if (window.hwnd != nullptr)
            {
                return false;
            }
        }

        return true;
    }

    VOID CALLBACK RenderTimerCallback(PVOID parameter, BOOLEAN)
    {
        auto* state = reinterpret_cast<AppState*>(parameter);
        if (!state || state->stopRequested)
        {
            return;
        }

        if (!state->windows.empty() && state->windows.front().hwnd && IsWindow(state->windows.front().hwnd))
        {
            PostMessageW(state->windows.front().hwnd, RenderTickMessage, 0, 0);
        }
    }

    bool StartRenderTimer(AppState& state)
    {
        const BOOL ok = CreateTimerQueueTimer(
            &state.renderTimer,
            nullptr,
            RenderTimerCallback,
            &state,
            0,
            8,
            WT_EXECUTEDEFAULT);
        if (!ok)
        {
            state.lastErrorStage = L"Create render timer";
            state.lastError = HRESULT_FROM_WIN32(GetLastError());
            return false;
        }

        return true;
    }

    void DecodeLoop(AppState* state)
    {
        if (!state)
        {
            return;
        }

        std::vector<BYTE> decodeBuffer(static_cast<size_t>(state->frameStride) * state->frameHeight);
        bool hasClockBase = false;
        LONGLONG baseSampleTime = 0;
        LONGLONG lastSampleTime = -1;
        auto baseWallClock = std::chrono::steady_clock::now();

        while (!state->stopRequested)
        {
            LONGLONG sampleTime = 0;
            if (!ReadNextFrame(*state, decodeBuffer, sampleTime))
            {
                state->decodeFailed = true;
                break;
            }

            // When playback loops, sample timestamps jump back to the beginning.
            // Reset the wall-clock anchor so each loop keeps original speed.
            if (!hasClockBase || lastSampleTime < 0 || sampleTime <= lastSampleTime)
            {
                hasClockBase = true;
                baseSampleTime = sampleTime;
                baseWallClock = std::chrono::steady_clock::now();
            }

            const LONGLONG delta100ns = sampleTime - baseSampleTime;
            const auto targetTime = baseWallClock + std::chrono::microseconds(delta100ns / 10);
            const auto now = std::chrono::steady_clock::now();
            if (targetTime > now)
            {
                std::this_thread::sleep_until(targetTime);
            }

            lastSampleTime = sampleTime;

            {
                std::lock_guard<std::mutex> lock(state->frameMutex);
                state->latestFrame = decodeBuffer;
                ++state->latestFrameSerial;
            }
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* monitor = reinterpret_cast<MonitorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (message)
        {
        case WM_NCCREATE:
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* monitorWindow = reinterpret_cast<MonitorWindow*>(createStruct->lpCreateParams);
            monitorWindow->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(monitorWindow));
            return TRUE;
        }
        case RenderTickMessage:
            if (monitor && monitor->owner)
            {
                if (monitor->owner->decodeFailed)
                {
                    const std::wstring message =
                        L"Video rendering failed.\n\nStage: " +
                        monitor->owner->lastErrorStage +
                        L"\nError: " +
                        HResultToString(monitor->owner->lastError);
                    MessageBoxW(hwnd, message.c_str(), L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
                    CloseAllWindows(*monitor->owner);
                    return 0;
                }

                if (TryPromoteLatestFrame(*monitor->owner))
                {
                    InvalidateAllWindows(*monitor->owner);
                }
            }
            return 0;
        case WM_PAINT:
            if (monitor && monitor->owner)
            {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                const bool ok = PaintFrameForWindow(*monitor->owner, *monitor);
                EndPaint(hwnd, &ps);
                if (!ok)
                {
                    const std::wstring message =
                        L"Video rendering failed.\n\nStage: " +
                        monitor->owner->lastErrorStage +
                        L"\nError: " +
                        HResultToString(monitor->owner->lastError);
                    MessageBoxW(hwnd, message.c_str(), L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
                    CloseAllWindows(*monitor->owner);
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (monitor)
            {
                if (monitor->renderTarget)
                {
                    monitor->renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                    monitor->targetWidth = LOWORD(lParam);
                    monitor->targetHeight = HIWORD(lParam);
                }
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (monitor && monitor->owner)
            {
                POINT current{};
                GetCursorPos(&current);
                const int dx = abs(current.x - monitor->owner->startupCursor.x);
                const int dy = abs(current.y - monitor->owner->startupCursor.y);

                if (monitor->owner->ignoreInitialMouseMove && dx < 8 && dy < 8)
                {
                    return 0;
                }

                monitor->owner->ignoreInitialMouseMove = false;
                if (dx >= 8 || dy >= 8)
                {
                    CloseAllWindows(*monitor->owner);
                }
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_MOUSEWHEEL:
            if (monitor && monitor->owner)
            {
                CloseAllWindows(*monitor->owner);
                return 0;
            }
            break;
        case WM_DESTROY:
            if (monitor)
            {
                ReleaseRenderResources(*monitor);
                monitor->hwnd = nullptr;
                if (monitor->owner && AllWindowsClosed(*monitor->owner))
                {
                    PostQuitMessage(0);
                }
            }
            return 0;
        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    int RunScreenSaver(HINSTANCE instance)
    {
        AppState state{};
        GetCursorPos(&state.startupCursor);

        std::vector<RECT> monitors;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
        state.virtualBounds = ComputeVirtualBounds(monitors);

        const wchar_t className[] = L"NativeVideoScrSaverWindow";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = className;
        wc.hCursor = nullptr;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

        if (!RegisterClassExW(&wc))
        {
            return 1;
        }

        ShowCursor(FALSE);

        state.windows.reserve(monitors.size());
        for (const auto& monitorRect : monitors)
        {
            MonitorWindow window{};
            window.owner = &state;
            window.monitorRect = monitorRect;
            state.windows.push_back(window);
        }

        for (auto& window : state.windows)
        {
            const int width = window.monitorRect.right - window.monitorRect.left;
            const int height = window.monitorRect.bottom - window.monitorRect.top;

            HWND hwnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                className,
                L"NativeVideoScrSaver",
                WS_POPUP | WS_VISIBLE,
                window.monitorRect.left,
                window.monitorRect.top,
                width,
                height,
                nullptr,
                nullptr,
                instance,
                &window);

            if (!hwnd)
            {
                CloseAllWindows(state);
                ShowCursor(TRUE);
                return 1;
            }
        }

        if (!InitializeReader(state))
        {
            const std::wstring message =
                L"Native renderer could not start.\n\nStage: " +
                state.lastErrorStage +
                L"\nError: " +
                HResultToString(state.lastError);
            MessageBoxW(state.windows.front().hwnd, message.c_str(), L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
            CloseAllWindows(state);
            ShutdownReader(state);
            ShowCursor(TRUE);
            return 1;
        }

        state.decodeThread = std::thread(DecodeLoop, &state);

        if (!StartRenderTimer(state))
        {
            const std::wstring message =
                L"Native renderer could not start.\n\nStage: " +
                state.lastErrorStage +
                L"\nError: " +
                HResultToString(state.lastError);
            MessageBoxW(state.windows.front().hwnd, message.c_str(), L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
            CloseAllWindows(state);
            ShutdownReader(state);
            ShowCursor(TRUE);
            return 1;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        ShutdownReader(state);
        ShowCursor(TRUE);
        return static_cast<int>(msg.wParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto mode = ParseMode();

    if (mode == ScreenSaverMode::Preview)
    {
        MessageBoxW(nullptr, L"/p preview host embedding is not implemented yet.", L"NativeVideoScrSaver", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (mode == ScreenSaverMode::Configure)
    {
        MessageBoxW(nullptr, L"Native completed mode: normalized MP4 + Media Foundation decode + per-monitor buffered rendering.", L"NativeVideoScrSaver", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"COM initialization failed.", L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"Media Foundation initialization failed.", L"NativeVideoScrSaver", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    const int exitCode = RunScreenSaver(instance);
    MFShutdown();
    CoUninitialize();
    return exitCode;
}
