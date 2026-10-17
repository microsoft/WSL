// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wil/com.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "AudioCaptureWorker.hpp"
#include "AudioWorkerTrace.hpp"
#include "WasapiInitialPacketPolicy.hpp"

namespace wsl::virtio_sound::audio_worker {
namespace {

    constexpr DWORD c_knownCaptureFlags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
    constexpr std::uint32_t c_maximumPacketBytes = MaximumCapturePacketFrames() * static_cast<std::uint32_t>(RequiredBytesPerFrame());

    std::wstring HresultMessage(const wchar_t* operation, HRESULT result)
    {
        std::wostringstream message;
        message << operation << L" failed (HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(result) << L").";
        return message.str();
    }

    CaptureError WasapiError(CaptureErrorCode code, const wchar_t* operation, HRESULT result)
    {
        detail::TraceAudioWorker(operation, static_cast<std::uint64_t>(code), static_cast<std::uint32_t>(result));
        return {code, static_cast<long>(result), HresultMessage(operation, result)};
    }

    CaptureError MmcssError(CaptureErrorCode code, const wchar_t* operation, DWORD win32Error)
    {
        const HRESULT result = HRESULT_FROM_WIN32(win32Error);
        std::wostringstream message;
        message << operation << L" failed (Win32 error " << win32Error << L", HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(result) << L").";
        detail::TraceAudioWorker(operation, static_cast<std::uint64_t>(code), static_cast<std::uint32_t>(result));
        return {code, static_cast<long>(result), message.str()};
    }

    bool SetWasapiErrorIfFailed(HRESULT result, CaptureErrorCode code, const wchar_t* operation, CaptureError& error)
    {
        if (!FAILED_LOG(result))
        {
            return false;
        }
        error = WasapiError(code, operation, result);
        return true;
    }

    WAVEFORMATEXTENSIBLE MakeRequiredFormat() noexcept
    {
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = RequiredCaptureFormat().m_channels;
        format.Format.nSamplesPerSec = RequiredCaptureFormat().m_sampleRate;
        format.Format.wBitsPerSample = RequiredCaptureFormat().m_bitsPerSample;
        format.Format.nBlockAlign = static_cast<WORD>(RequiredBytesPerFrame());
        format.Format.nAvgBytesPerSec = RequiredCaptureFormat().m_sampleRate * static_cast<DWORD>(RequiredBytesPerFrame());
        format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
        format.Samples.wValidBitsPerSample = RequiredCaptureFormat().m_bitsPerSample;
        format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        return format;
    }

    class WasapiCaptureDevice final : public ICaptureDevice
    {
    public:
        WasapiCaptureDevice() = default;
        ~WasapiCaptureDevice() override
        {
            try
            {
                CaptureError ignored;
                (void)Close(ignored);
            }
            catch (...)
            {
                // The destructor is noexcept. WorkerMain calls Close first so
                // normal shutdown errors remain observable there.
                detail::TraceAudioWorker(L"MmcssDestructorCloseException");
            }
        }

        bool OpenEndpoint(const std::wstring& endpointId, PcmFormat& negotiatedFormat, CaptureError& error) override
        {
            error = {};
            if (m_openAttempted)
            {
                error = {CaptureErrorCode::EndpointOpenFailed, 0, L"A WASAPI capture device instance can open only one endpoint."};
                return false;
            }
            m_openAttempted = true;
            if (endpointId.empty())
            {
                error = {CaptureErrorCode::InvalidEndpointId, 0, L"WASAPI requires an explicit capture endpoint ID."};
                return false;
            }

            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SetWasapiErrorIfFailed(comResult, CaptureErrorCode::EndpointOpenFailed, L"CoInitializeEx", error))
            {
                return false;
            }
            m_comInitialization.activate();

            HRESULT result = CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(m_enumerator.put()));
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"CoCreateInstance(MMDeviceEnumerator)", error))
            {
                return false;
            }

            // GetDevice takes the selected endpoint ID directly. There is
            // intentionally no default-endpoint fallback in this worker.
            result = m_enumerator->GetDevice(endpointId.c_str(), m_device.put());
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IMMDeviceEnumerator::GetDevice", error))
            {
                return false;
            }

            result = m_device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(m_endpoint.put()));
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IMMDevice::QueryInterface(IMMEndpoint)", error))
            {
                return false;
            }
            EDataFlow dataFlow = eAll;
            result = m_endpoint->GetDataFlow(&dataFlow);
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IMMEndpoint::GetDataFlow", error))
            {
                return false;
            }
            if (dataFlow != eCapture)
            {
                error = {CaptureErrorCode::EndpointIsNotCapture, 0, L"The selected endpoint is not a capture endpoint."};
                return false;
            }

            result = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(m_audioClient.put()));
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IMMDevice::Activate(IAudioClient)", error))
            {
                return false;
            }

            auto requiredFormat = MakeRequiredFormat();
            WAVEFORMATEX* closestMatchRaw = nullptr;
            result = m_audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &requiredFormat.Format, &closestMatchRaw);
            wil::unique_any<WAVEFORMATEX*, decltype(&CoTaskMemFree), CoTaskMemFree> closestMatch{closestMatchRaw};
            if (result != S_OK)
            {
                if (result == S_FALSE || result == AUDCLNT_E_UNSUPPORTED_FORMAT)
                {
                    error = {
                        CaptureErrorCode::UnsupportedFormat,
                        static_cast<long>(result),
                        L"The selected endpoint does not accept 48 kHz stereo S16LE in shared mode."};
                }
                else
                {
                    (void)FAILED_LOG(result);
                    error = WasapiError(CaptureErrorCode::EndpointOpenFailed, L"IAudioClient::IsFormatSupported", result);
                }
                return false;
            }

            result = m_audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, 0, 0, &requiredFormat.Format, nullptr);
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IAudioClient::Initialize", error))
            {
                return false;
            }

            m_captureEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!m_captureEvent)
            {
                error = {
                    CaptureErrorCode::EndpointOpenFailed,
                    static_cast<long>(HRESULT_FROM_WIN32(GetLastError())),
                    L"CreateEventW could not create the WASAPI capture event."};
                return false;
            }

            result = m_audioClient->SetEventHandle(m_captureEvent.get());
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::EndpointOpenFailed, L"IAudioClient::SetEventHandle", error))
            {
                return false;
            }

            result = m_audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_captureClient.put()));
            if (SetWasapiErrorIfFailed(
                    result, CaptureErrorCode::EndpointOpenFailed, L"IAudioClient::GetService(IAudioCaptureClient)", error))
            {
                return false;
            }

            negotiatedFormat = RequiredCaptureFormat();
            return true;
        }

        bool Start(CaptureError& error) override
        {
            error = {};
            if (!m_audioClient || !m_captureClient || !m_captureEvent)
            {
                error = {CaptureErrorCode::StartFailed, 0, L"The WASAPI endpoint was not fully initialized."};
                return false;
            }
            if (m_streamStarted || m_mmcssTaskHandle)
            {
                error = {CaptureErrorCode::StartFailed, 0, L"The WASAPI capture device can be started only once."};
                return false;
            }

            // This device is created and used by AudioCaptureWorker::WorkerMain,
            // so the MMCSS registration and its eventual reversion stay on the
            // same capture thread. Do not silently continue at a lower priority
            // if MMCSS registration fails; this candidate is measuring that
            // scheduling change explicitly.
            DWORD taskIndex = 0;
            m_mmcssTaskHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
            if (!m_mmcssTaskHandle)
            {
                error = MmcssError(CaptureErrorCode::StartFailed, L"AvSetMmThreadCharacteristicsW(Capture)", GetLastError());
                return false;
            }
            m_mmcssTaskIndex = taskIndex;
            m_mmcssThreadId = GetCurrentThreadId();
            detail::TraceAudioWorker(L"MmcssRegistered", m_mmcssTaskIndex, m_mmcssThreadId);

            const HRESULT result = m_audioClient->Start();
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::StartFailed, L"IAudioClient::Start", error))
            {
                return false;
            }
            m_streamStarted = true;
            m_initialPacketPolicy = WasapiInitialPacketPolicy{};
            return true;
        }

        CaptureReadResult ReadPacket(std::chrono::milliseconds timeout, const std::atomic<bool>& stopRequested, CapturePacket& packet, CaptureError& error) override
        {
            error = {};
            packet = {};
            if (stopRequested.load(std::memory_order_acquire))
            {
                return CaptureReadResult::Stopped;
            }
            if (!m_captureClient || !m_captureEvent)
            {
                error = {CaptureErrorCode::ReadFailed, 0, L"The WASAPI capture client is unavailable."};
                return CaptureReadResult::Failed;
            }

            UINT32 nextPacketFrames = 0;
            HRESULT result = m_captureClient->GetNextPacketSize(&nextPacketFrames);
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::ReadFailed, L"IAudioCaptureClient::GetNextPacketSize", error))
            {
                return CaptureReadResult::Failed;
            }
            if (nextPacketFrames == 0)
            {
                const auto timeoutCount = (std::max)(timeout.count(), std::chrono::milliseconds::rep{0});
                const auto maximumWait = static_cast<std::chrono::milliseconds::rep>(MAXDWORD - 1);
                const DWORD waitMilliseconds = static_cast<DWORD>((std::min)(timeoutCount, maximumWait));
                const DWORD waitResult = WaitForSingleObject(m_captureEvent.get(), waitMilliseconds);
                if (waitResult == WAIT_TIMEOUT)
                {
                    return stopRequested.load(std::memory_order_acquire) ? CaptureReadResult::Stopped : CaptureReadResult::Timeout;
                }
                if (waitResult != WAIT_OBJECT_0)
                {
                    const DWORD lastError = GetLastError();
                    error = {
                        CaptureErrorCode::ReadFailed,
                        static_cast<long>(HRESULT_FROM_WIN32(lastError)),
                        L"Waiting for the WASAPI capture event failed."};
                    return CaptureReadResult::Failed;
                }
                if (stopRequested.load(std::memory_order_acquire))
                {
                    return CaptureReadResult::Stopped;
                }
                result = m_captureClient->GetNextPacketSize(&nextPacketFrames);
                if (SetWasapiErrorIfFailed(result, CaptureErrorCode::ReadFailed, L"IAudioCaptureClient::GetNextPacketSize", error))
                {
                    return CaptureReadResult::Failed;
                }
                if (nextPacketFrames == 0)
                {
                    return CaptureReadResult::Timeout;
                }
            }

            if (nextPacketFrames > MaximumCapturePacketFrames())
            {
                error = {
                    CaptureErrorCode::InvalidPacket, 0, L"WASAPI reported a capture packet larger than the 48,000 frame bound."};
                return CaptureReadResult::Failed;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            result = m_captureClient->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
            if (SetWasapiErrorIfFailed(result, CaptureErrorCode::ReadFailed, L"IAudioCaptureClient::GetBuffer", error))
            {
                return CaptureReadResult::Failed;
            }
            packet.m_rawFlags = flags;
            (void)devicePosition;
            (void)qpcPosition;

            bool packetValid = true;
            CaptureError packetError;
            if (frames > MaximumCapturePacketFrames() || frames > c_maximumPacketBytes / RequiredBytesPerFrame())
            {
                packetValid = false;
                packetError = {
                    CaptureErrorCode::InvalidPacket, 0, L"WASAPI returned a capture packet larger than the 48,000 frame bound."};
            }
            else if ((flags & ~c_knownCaptureFlags) != 0)
            {
                packetValid = false;
                packetError = {CaptureErrorCode::ReadFailed, 0, L"WASAPI returned unsupported capture-buffer flags."};
            }
            else if (frames != 0)
            {
                const std::size_t byteCount = static_cast<std::size_t>(frames) * RequiredBytesPerFrame();
                try
                {
                    packet.m_frames = frames;
                    packet.m_s16LeBytes.resize(byteCount);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                    {
                        std::fill(packet.m_s16LeBytes.begin(), packet.m_s16LeBytes.end(), std::uint8_t{0});
                    }
                    else if (data == nullptr)
                    {
                        packetValid = false;
                        packetError = {
                            CaptureErrorCode::ReadFailed, 0, L"WASAPI returned a null data pointer for a non-silent packet."};
                    }
                    else
                    {
                        const std::size_t inputBytes = static_cast<std::size_t>(frames) * RequiredBytesPerFrame();
                        if (inputBytes != byteCount)
                        {
                            packetValid = false;
                            packetError = {
                                CaptureErrorCode::InvalidPacket, 0, L"WASAPI returned an incomplete S16LE frame packet."};
                        }
                        else
                        {
                            std::copy_n(data, byteCount, packet.m_s16LeBytes.begin());
                        }
                    }
                }
                catch (const std::bad_alloc&)
                {
                    packetValid = false;
                    packetError = {CaptureErrorCode::ReadFailed, 0, L"Memory allocation failed while copying a WASAPI packet."};
                }
            }

            const HRESULT releaseResult = m_captureClient->ReleaseBuffer(frames);
            if (SetWasapiErrorIfFailed(releaseResult, CaptureErrorCode::ReadFailed, L"IAudioCaptureClient::ReleaseBuffer", error))
            {
                return CaptureReadResult::Failed;
            }
            const WasapiPacketFlagDecision flagDecision = m_initialPacketPolicy.Evaluate(
                flags, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, frames, packetValid, true);
            packet.m_rawFlags = flagDecision.m_rawFlags;
            packet.m_discontinuity = flagDecision.m_discontinuity;
            if (flagDecision.m_traceStartupDataDiscontinuity)
            {
                detail::TraceAudioWorker(L"StartupDataDiscontinuityAccepted", flagDecision.m_rawFlags, frames);
            }
            if (!packetValid)
            {
                error = std::move(packetError);
                return CaptureReadResult::Failed;
            }
            if (frames == 0)
            {
                return CaptureReadResult::Timeout;
            }
            return CaptureReadResult::Packet;
        }

        bool Close(CaptureError& error) override
        {
            error = {};
            if (m_mmcssTaskHandle && GetCurrentThreadId() != m_mmcssThreadId)
            {
                const DWORD currentThreadId = GetCurrentThreadId();
                // Do not stop/release thread-affine WASAPI state or invoke the
                // MMCSS API from a different thread. A later Close on the
                // owner thread can still complete this cleanup.
                detail::TraceAudioWorker(L"MmcssRevertWrongThread", m_mmcssThreadId, currentThreadId);
                error = {
                    CaptureErrorCode::StopFailed,
                    static_cast<long>(HRESULT_FROM_WIN32(ERROR_INVALID_THREAD_ID)),
                    L"The WASAPI device must be closed on the capture thread that registered MMCSS."};
                return false;
            }
            if (m_streamStarted && m_audioClient)
            {
                const HRESULT result = m_audioClient->Stop();
                m_streamStarted = false;
                (void)SetWasapiErrorIfFailed(result, CaptureErrorCode::StopFailed, L"IAudioClient::Stop", error);
            }

            if (m_mmcssTaskHandle)
            {
                const DWORD currentThreadId = GetCurrentThreadId();
                const HANDLE taskHandle = m_mmcssTaskHandle;
                if (AvRevertMmThreadCharacteristics(taskHandle))
                {
                    m_mmcssTaskHandle = nullptr;
                    detail::TraceAudioWorker(L"MmcssReverted", m_mmcssTaskIndex, currentThreadId);
                    m_mmcssTaskIndex = 0;
                    m_mmcssThreadId = 0;
                }
                else
                {
                    const DWORD lastError = GetLastError();
                    detail::TraceAudioWorker(L"MmcssRevertFailed", m_mmcssTaskIndex, static_cast<std::uint32_t>(HRESULT_FROM_WIN32(lastError)));
                    if (error.m_code == CaptureErrorCode::None)
                    {
                        error = MmcssError(CaptureErrorCode::StopFailed, L"AvRevertMmThreadCharacteristics", lastError);
                    }
                }
            }

            m_captureClient.reset();
            m_audioClient.reset();
            m_endpoint.reset();
            m_device.reset();
            m_enumerator.reset();
            m_captureEvent.reset();
            m_comInitialization.reset();
            return error.m_code == CaptureErrorCode::None;
        }

    private:
        wil::com_ptr<IMMDeviceEnumerator> m_enumerator;
        wil::com_ptr<IMMDevice> m_device;
        wil::com_ptr<IMMEndpoint> m_endpoint;
        wil::com_ptr<IAudioClient> m_audioClient;
        wil::com_ptr<IAudioCaptureClient> m_captureClient;
        wil::unique_couninitialize_call m_comInitialization{false};
        wil::unique_event m_captureEvent;
        HANDLE m_mmcssTaskHandle{};
        DWORD m_mmcssTaskIndex{};
        DWORD m_mmcssThreadId{};
        bool m_streamStarted{};
        bool m_openAttempted{};
        WasapiInitialPacketPolicy m_initialPacketPolicy;
    };

    class WasapiCaptureDeviceFactory final : public ICaptureDeviceFactory
    {
    public:
        std::unique_ptr<ICaptureDevice> Create() override
        {
            return std::make_unique<WasapiCaptureDevice>();
        }
    };

} // namespace

std::unique_ptr<ICaptureDeviceFactory> CreateWasapiCaptureDeviceFactory()
{
    return std::make_unique<WasapiCaptureDeviceFactory>();
}

} // namespace wsl::virtio_sound::audio_worker
