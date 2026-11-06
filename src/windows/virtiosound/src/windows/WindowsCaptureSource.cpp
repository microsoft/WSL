// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <WtsApi32.h>
#include <UserEnv.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "WindowsCaptureSource.hpp"
#include "RestrictedUserToken.hpp"
#include "WorkerPipeSecurity.hpp"
#include "JobBoundProcessStartup.hpp"
#include "AudioSourceTrace.hpp"
#include "AudioIpcCodec.hpp"
#include "AudioPipeTransport.hpp"
#include "S16LePcmRing.hpp"
#include "CaptureStopDeadline.hpp"

namespace wsl::virtio_sound::native::windows {
namespace {

    using namespace std::chrono_literals;
    using wsl::virtio_sound::audio_ipc::Header;
    using wsl::virtio_sound::audio_ipc::Message;
    using wsl::virtio_sound::audio_ipc::MessageType;

    constexpr std::size_t c_captureRingFrames = 262144U;
    constexpr std::size_t c_pcmBytesPerFrame = 4U;
    constexpr std::uint32_t c_maximumPcmFrames = 48000U;
    constexpr DWORD c_pipeConnectTimeoutMs = 5000U;
    constexpr DWORD c_pipeMessageTimeoutMs = 1000U;
    // Linux virtio-snd waits up to 1000 ms for a control response. Fail worker
    // startup before that deadline so a parked START descriptor is always bounded.
    constexpr DWORD c_workerStartTimeoutMs = 750U;
    constexpr DWORD c_workerStopTimeoutMs = 2000U;
    constexpr DWORD c_workerCloseTimeoutMs = 2000U;
    constexpr DWORD c_controllerCadenceMs = 5U;
    constexpr DWORD c_sessionCheckCadenceMs = 1000U;
    constexpr DWORD c_partialFrameTimeoutMs = 1000U;
    using WtsMemory = wil::unique_any<PVOID, decltype(&WTSFreeMemory), WTSFreeMemory>;
    using EnvironmentBlock = wil::unique_any<LPVOID, decltype(&DestroyEnvironmentBlock), DestroyEnvironmentBlock>;

    bool QueryTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS informationClass, std::vector<std::uint8_t>& bytes) noexcept
    {
        DWORD required = 0U;
        (void)GetTokenInformation(token, informationClass, nullptr, 0U, &required);
        if (required == 0U)
        {
            return false;
        }
        try
        {
            bytes.resize(required);
        }
        catch (...)
        {
            return false;
        }
        return GetTokenInformation(token, informationClass, bytes.data(), required, &required) != FALSE;
    }

    bool QueryTokenUserAndSession(HANDLE token, std::vector<std::uint8_t>& userBytes, DWORD& sessionId) noexcept
    {
        if (!QueryTokenInformation(token, TokenUser, userBytes))
        {
            return false;
        }
        std::vector<std::uint8_t> sessionBytes;
        if (!QueryTokenInformation(token, TokenSessionId, sessionBytes) || sessionBytes.size() != sizeof(DWORD))
        {
            return false;
        }
        std::memcpy(&sessionId, sessionBytes.data(), sizeof(sessionId));
        if (sessionId == 0U)
        {
            return false;
        }
        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBytes.data());
        return IsValidSid(tokenUser->User.Sid) != FALSE;
    }

    bool IsSessionActive(DWORD sessionId) noexcept
    {
        LPWSTR stateText = nullptr;
        DWORD stateBytes = 0U;
        const BOOL querySucceeded =
            WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSConnectState, &stateText, &stateBytes);
        WtsMemory stateMemory{stateText};
        if (querySucceeded == FALSE || stateText == nullptr || stateBytes < sizeof(WTS_CONNECTSTATE_CLASS))
        {
            return false;
        }
        WTS_CONNECTSTATE_CLASS state{};
        std::memcpy(&state, stateText, sizeof(state));
        return state == WTSActive;
    }

    bool IsValidUtf16Endpoint(const std::wstring& endpoint, std::u16string& utf16) noexcept
    {
        if (endpoint.empty() || endpoint.size() > 2048U)
        {
            return false;
        }
        try
        {
            utf16.clear();
            utf16.reserve((std::min)(endpoint.size(), wsl::virtio_sound::audio_ipc::c_maxEndpointCodeUnits));
            if constexpr (sizeof(wchar_t) == sizeof(char16_t))
            {
                for (wchar_t value : endpoint)
                {
                    if (value == L'\0' || utf16.size() >= wsl::virtio_sound::audio_ipc::c_maxEndpointCodeUnits)
                    {
                        return false;
                    }
                    utf16.push_back(static_cast<char16_t>(value));
                }
            }
            else
            {
                for (wchar_t value : endpoint)
                {
                    const std::uint32_t codePoint = static_cast<std::uint32_t>(value);
                    if (codePoint == 0U || codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
                    {
                        return false;
                    }
                    if (codePoint <= 0xffffU)
                    {
                        if (utf16.size() >= wsl::virtio_sound::audio_ipc::c_maxEndpointCodeUnits)
                        {
                            return false;
                        }
                        utf16.push_back(static_cast<char16_t>(codePoint));
                    }
                    else
                    {
                        if (utf16.size() + 2U > wsl::virtio_sound::audio_ipc::c_maxEndpointCodeUnits)
                        {
                            return false;
                        }
                        const std::uint32_t adjusted = codePoint - 0x10000U;
                        utf16.push_back(static_cast<char16_t>(0xd800U + (adjusted >> 10U)));
                        utf16.push_back(static_cast<char16_t>(0xdc00U + (adjusted & 0x3ffU)));
                    }
                }
            }
            std::vector<std::uint8_t> ignored;
            wsl::virtio_sound::audio_ipc::Error error{};
            return wsl::virtio_sound::audio_ipc::BuildStartPayload(utf16, ignored, error);
        }
        catch (...)
        {
            utf16.clear();
            return false;
        }
    }

    std::wstring FinalPathFromHandle(HANDLE file)
    {
        std::vector<wchar_t> path(32768U, L'\0');
        const DWORD length =
            GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0U || length >= path.size())
        {
            throw std::runtime_error("Unable to validate an audio worker path");
        }
        return std::wstring(path.data(), length);
    }

    std::wstring DirectoryName(const std::wstring& path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return {};
        }
        if (slash == 0U)
        {
            return path.substr(0U, 1U);
        }
        return path.substr(0U, slash);
    }

    bool OrdinalEqualsIgnoreCase(const std::wstring& left, const std::wstring& right) noexcept
    {
        if (left.size() > static_cast<std::size_t>(INT_MAX) || right.size() > static_cast<std::size_t>(INT_MAX))
        {
            return false;
        }
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    std::wstring ValidateAdjacentWorkerPath(const std::wstring& workerPath)
    {
        if (workerPath.empty() || workerPath.size() >= 32767U || workerPath.find(L'\0') != std::wstring::npos ||
            workerPath.find(L'"') != std::wstring::npos)
        {
            throw std::runtime_error("Invalid audio worker path");
        }

        std::vector<wchar_t> serviceBuffer(32768U, L'\0');
        const DWORD serviceLength = GetModuleFileNameW(nullptr, serviceBuffer.data(), static_cast<DWORD>(serviceBuffer.size()));
        if (serviceLength == 0U || serviceLength >= serviceBuffer.size())
        {
            throw std::runtime_error("Unable to validate the service image path");
        }
        const std::wstring servicePath(serviceBuffer.data(), serviceLength);
        wil::unique_hfile serviceFile{CreateFileW(
            servicePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!serviceFile)
        {
            throw std::runtime_error("Unable to validate the service image");
        }
        const std::wstring serviceFinalPath = FinalPathFromHandle(serviceFile.get());

        std::vector<wchar_t> fullBuffer(32768U, L'\0');
        const DWORD fullLength = GetFullPathNameW(workerPath.c_str(), static_cast<DWORD>(fullBuffer.size()), fullBuffer.data(), nullptr);
        if (fullLength == 0U || fullLength >= fullBuffer.size())
        {
            throw std::runtime_error("Unable to resolve the audio worker path");
        }
        const std::wstring fullPath(fullBuffer.data(), fullLength);
        wil::unique_hfile workerFile{CreateFileW(
            fullPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!workerFile)
        {
            throw std::runtime_error("Unable to open the audio worker image");
        }

        BY_HANDLE_FILE_INFORMATION workerInformation{};
        if (GetFileInformationByHandle(workerFile.get(), &workerInformation) == FALSE ||
            (workerInformation.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
        {
            throw std::runtime_error("The audio worker image is not a regular adjacent file");
        }
        const std::wstring workerFinalPath = FinalPathFromHandle(workerFile.get());
        if (!OrdinalEqualsIgnoreCase(DirectoryName(serviceFinalPath), DirectoryName(workerFinalPath)))
        {
            throw std::runtime_error("The audio worker must be adjacent to the service image");
        }
        return workerFinalPath;
    }

    std::wstring QuoteWindowsArgument(const std::wstring& argument)
    {
        std::wstring quoted;
        quoted.push_back(L'"');
        std::size_t slashCount = 0U;
        for (wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++slashCount;
                continue;
            }
            if (character == L'"')
            {
                quoted.append(slashCount * 2U + 1U, L'\\');
                quoted.push_back(L'"');
                slashCount = 0U;
                continue;
            }
            quoted.append(slashCount, L'\\');
            slashCount = 0U;
            quoted.push_back(character);
        }
        quoted.append(slashCount * 2U, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    std::wstring CreatePipeName()
    {
        GUID id{};
        const HRESULT result = CoCreateGuid(&id);
        THROW_IF_FAILED(result);
        std::array<wchar_t, 40U> guidText{};
        if (StringFromGUID2(id, guidText.data(), static_cast<int>(guidText.size())) == 0)
        {
            throw std::runtime_error("Unable to create an audio pipe name");
        }
        return std::wstring(L"\\\\.\\pipe\\wsl-audio-") + guidText.data();
    }

    bool IsSuccessfulPipeResult(wsl::virtio_sound::audio_ipc::windows::PipeResult result) noexcept
    {
        return result == wsl::virtio_sound::audio_ipc::windows::PipeResult::Success;
    }

    bool IsExpectedMessage(const Message& message, MessageType type, std::uint64_t epoch, std::uint64_t sequence) noexcept
    {
        wsl::virtio_sound::audio_ipc::Error error{};
        return wsl::virtio_sound::audio_ipc::ValidateMessage(message, error) && message.m_header.m_type == type &&
               message.m_header.m_epoch == epoch && message.m_header.m_sequence == sequence && message.m_payload.empty();
    }

} // namespace

class WindowsCaptureSource::Impl final
{
public:
    explicit Impl(std::size_t capacityFrames) : m_ring(PcmFormat{}, capacityFrames)
    {
        (void)m_ring.CapacityFrames();
    }

    ~Impl()
    {
        Shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void Initialize(HANDLE restrictedUserToken, const std::wstring& workerPath, const std::wstring& endpointId)
    {
        if (restrictedUserToken == nullptr || restrictedUserToken == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("A restricted user token is required");
        }
        m_wakeEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        m_transportCancelEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!m_wakeEvent || !m_transportCancelEvent)
        {
            throw std::runtime_error("Unable to initialize worker supervision events");
        }
        std::u16string endpointUtf16;
        if (!IsValidUtf16Endpoint(endpointId, endpointUtf16))
        {
            throw std::runtime_error("The explicit audio endpoint ID is invalid");
        }
        wsl::virtio_sound::audio_ipc::Error codecError{};
        if (!wsl::virtio_sound::audio_ipc::BuildStartPayload(endpointUtf16, m_startPayload, codecError))
        {
            throw std::runtime_error("The explicit audio endpoint ID is invalid");
        }
        m_workerImagePath = ValidateAdjacentWorkerPath(workerPath);

        HANDLE duplicatedToken = detail::DuplicateRestrictedUserTokenAsPrimary(restrictedUserToken);
        if (duplicatedToken == nullptr)
        {
            throw std::runtime_error("Unable to create a primary restricted worker token");
        }
        m_token.reset(duplicatedToken);
        std::vector<std::uint8_t> userBytes;
        if (!QueryTokenUserAndSession(m_token.get(), userBytes, m_sessionId))
        {
            throw std::runtime_error("The restricted worker token has no active user session");
        }
        const auto* user = reinterpret_cast<const TOKEN_USER*>(userBytes.data());
        const DWORD sidLength = GetLengthSid(user->User.Sid);
        if (sidLength == 0U)
        {
            throw std::runtime_error("The worker user SID is invalid");
        }
        m_userSid.assign(reinterpret_cast<const std::uint8_t*>(user->User.Sid), reinterpret_cast<const std::uint8_t*>(user->User.Sid) + sidLength);
        if (!IsSessionActive(m_sessionId))
        {
            throw std::runtime_error("The restricted worker session is not active");
        }

        CreateServerPipe();
        CreateAndLaunchWorker();
        ConnectAndVerifyWorker();
        CompleteHandshake();

        const auto beforeReset = m_ring.Snapshot().m_resetFailures;
        m_ring.Reset();
        if (m_ring.Snapshot().m_resetFailures != beforeReset)
        {
            throw std::runtime_error("Unable to initialize the capture PCM ring");
        }
        m_readiness = SourceReadiness::Ready;
        m_controller = std::thread([this]() noexcept { ControllerMain(); });
    }

    bool StartCapture(std::uint32_t periodFrames) noexcept
    {
        detail::TraceAudioSource(L"StartRequested", periodFrames);
        if (periodFrames == 0U || periodFrames > c_maximumPcmFrames)
        {
            return false;
        }
        bool result = false;
        {
            auto stateLock = m_stateLock.lock_exclusive();
            if (m_terminalFailure || m_shutdownRequested || m_highestEpoch == (std::numeric_limits<std::uint64_t>::max)())
            {
                SetFailedLocked();
            }
            else
            {
                const auto previousResetFailures = m_ring.Snapshot().m_resetFailures;
                m_ring.Reset();
                const PcmRingSnapshot afterReset = m_ring.Snapshot();
                if (afterReset.m_resetFailures != previousResetFailures || afterReset.m_generation == 0U)
                {
                    SetFailedLocked();
                }
                else
                {
                    ++m_highestEpoch;
                    m_desiredEpoch = m_highestEpoch;
                    m_desiredGeneration = afterReset.m_generation;
                    m_desiredPeriodFrames = periodFrames;
                    m_desiredRunning = true;
                    m_readiness = SourceReadiness::Pending;
                    m_ringPrimedReported = false;
                    m_startDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_workerStartTimeoutMs);
                    result = true;
                }
            }
        }
        if (m_wakeEvent)
        {
            (void)SetEvent(m_wakeEvent.get());
        }
        return result;
    }

    void StopCapture() noexcept
    {
        {
            auto stateLock = m_stateLock.lock_exclusive();
            if (m_terminalFailure)
            {
                return;
            }
            const auto previousResetFailures = m_ring.Snapshot().m_resetFailures;
            m_ring.Reset();
            if (m_ring.Snapshot().m_resetFailures != previousResetFailures)
            {
                SetFailedLocked();
            }
            else
            {
                if (m_desiredRunning || m_wirePhase != WirePhase::Idle)
                {
                    m_stopDeadline.OnStopOrResetRequest(
                        m_wirePhase, std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
                }
                m_desiredRunning = false;
                m_desiredPeriodFrames = 0U;
                m_readiness = SourceReadiness::Ready;
                m_ringPrimedReported = false;
            }
        }
        if (m_wakeEvent)
        {
            (void)SetEvent(m_wakeEvent.get());
        }
    }

    SourceReadiness CaptureReadiness() const noexcept
    {
        auto stateLock = m_stateLock.lock_shared();
        return m_terminalFailure ? SourceReadiness::Failed : m_readiness;
    }

    std::optional<std::uint32_t> AvailableFrames() const noexcept
    {
        auto stateLock = m_stateLock.lock_shared();
        if (m_terminalFailure || m_readiness != SourceReadiness::Ready || !m_desiredRunning)
        {
            return 0U;
        }
        const PcmRingSnapshot snapshot = m_ring.Snapshot();
        if (snapshot.m_generation != m_desiredGeneration)
        {
            return 0U;
        }
        const std::size_t maximumFrames = (std::numeric_limits<std::uint32_t>::max)();
        return static_cast<std::uint32_t>((std::min)(snapshot.m_queuedFrames, maximumFrames));
    }

    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept
    {
        auto stateLock = m_stateLock.lock_exclusive();
        if (m_terminalFailure || m_readiness != SourceReadiness::Ready || !m_desiredRunning)
        {
            detail::TraceAudioSource(
                L"ReadFramesStateRejected",
                frames,
                (m_terminalFailure ? 1U : 0U) | (!m_desiredRunning ? 2U : 0U) | (static_cast<std::uint64_t>(m_readiness) << 8U));
            return false;
        }
        const bool read = m_ring.ReadFrames(frames, destination, byteCount);
        if (!read)
        {
            detail::TraceAudioSource(L"RingReadFailed", frames, m_ring.Snapshot().m_queuedFrames);
        }
        return read;
    }

    void Reset() noexcept
    {
        {
            auto stateLock = m_stateLock.lock_exclusive();
            if (m_terminalFailure)
            {
                return;
            }
            const auto previousResetFailures = m_ring.Snapshot().m_resetFailures;
            m_ring.Reset();
            if (m_ring.Snapshot().m_resetFailures != previousResetFailures)
            {
                SetFailedLocked();
            }
            else
            {
                if (m_desiredRunning || m_wirePhase != WirePhase::Idle)
                {
                    m_stopDeadline.OnStopOrResetRequest(
                        m_wirePhase, std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
                }
                m_desiredRunning = false;
                m_desiredPeriodFrames = 0U;
                m_readiness = SourceReadiness::Ready;
                m_ringPrimedReported = false;
            }
        }
        if (m_wakeEvent)
        {
            (void)SetEvent(m_wakeEvent.get());
        }
    }

    void Shutdown() noexcept
    {
        auto shutdownLock = m_shutdownLock.lock_exclusive();
        if (m_shutdownComplete)
        {
            return;
        }
        {
            auto stateLock = m_stateLock.lock_exclusive();
            m_shutdownRequested = true;
            m_desiredRunning = false;
            m_desiredPeriodFrames = 0U;
            if (!m_terminalFailure)
            {
                const auto previousResetFailures = m_ring.Snapshot().m_resetFailures;
                m_ring.Reset();
                if (m_ring.Snapshot().m_resetFailures != previousResetFailures)
                {
                    SetFailedLocked();
                }
                else
                {
                    m_readiness = SourceReadiness::Ready;
                    m_ringPrimedReported = false;
                }
            }
            if (m_wirePhase != WirePhase::Idle && m_wirePhase != WirePhase::Closed)
            {
                m_stopDeadline.ArmIfMissing(std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
            }
        }
        if (m_wakeEvent)
        {
            (void)SetEvent(m_wakeEvent.get());
        }
        if (m_controller.joinable())
        {
            m_controller.join();
        }

        if (m_process && WaitForSingleObject(m_process.get(), 0U) != WAIT_OBJECT_0)
        {
            ForceTerminateWorker();
        }
        m_pipe.reset();
        m_process.reset();
        m_job.reset();
        m_token.reset();
        m_shutdownComplete = true;
    }

private:
    using WirePhase = detail::CaptureStopDeadline::Phase;

    enum class PeekResult
    {
        NoMessage,
        CompleteMessage,
        InvalidFrame,
        Disconnected,
    };

    void SetFailedLocked() noexcept
    {
        m_terminalFailure = true;
        m_desiredRunning = false;
        m_desiredPeriodFrames = 0U;
        m_readiness = SourceReadiness::Failed;
        m_ringPrimedReported = false;
        const auto previousResetFailures = m_ring.Snapshot().m_resetFailures;
        m_ring.Reset();
        (void)previousResetFailures;
    }

    void MarkFailed(std::uint32_t location = 0) noexcept
    {
        detail::TraceAudioSource(L"ControllerFailed", location);
        {
            auto stateLock = m_stateLock.lock_exclusive();
            SetFailedLocked();
        }
        if (m_wakeEvent)
        {
            (void)SetEvent(m_wakeEvent.get());
        }
    }

    void CreateServerPipe()
    {
        SECURITY_DESCRIPTOR descriptor{};
        std::vector<BYTE> daclStorage;
        std::vector<BYTE> saclStorage;
        if (!detail::BuildWorkerPipeSecurityDescriptor(m_userSid.data(), static_cast<DWORD>(m_userSid.size()), daclStorage, saclStorage, descriptor))
        {
            throw std::runtime_error("Unable to create the protected worker pipe ACL");
        }
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = &descriptor;
        securityAttributes.bInheritHandle = FALSE;

        m_pipeName = CreatePipeName();
        m_pipe.reset(CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1U,
            static_cast<DWORD>(wsl::virtio_sound::audio_ipc::c_maxMessageBytes),
            static_cast<DWORD>(wsl::virtio_sound::audio_ipc::c_maxMessageBytes),
            0U,
            &securityAttributes));
        if (!m_pipe)
        {
            throw std::runtime_error("Unable to create the protected worker pipe");
        }
    }

    void CreateAndLaunchWorker()
    {
        m_job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!m_job)
        {
            throw std::runtime_error("Unable to create the worker process job");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInformation{};
        jobInformation.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &jobInformation, sizeof(jobInformation)) == FALSE)
        {
            throw std::runtime_error("Unable to configure bounded worker cleanup");
        }

        LPVOID environmentBlock = nullptr;
        if (CreateEnvironmentBlock(&environmentBlock, m_token.get(), FALSE) == FALSE)
        {
            throw std::runtime_error("Unable to create the restricted worker environment");
        }
        EnvironmentBlock environment{environmentBlock};

        const std::wstring command = QuoteWindowsArgument(m_workerImagePath) + L" --pipe " + QuoteWindowsArgument(m_pipeName) +
                                     L" --server-pid " + std::to_wstring(GetCurrentProcessId());
        std::vector<wchar_t> mutableCommand;
        mutableCommand.assign(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        detail::JobBoundProcessStartup jobStartup(m_job.get());
        auto& startup = jobStartup.Info().StartupInfo;
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION processInformation{};
        DWORD callerSession = MAXDWORD;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &callerSession))
        {
            const DWORD error = GetLastError();
            throw std::runtime_error("ProcessIdToSessionId(audio host) failed: win32=" + std::to_string(error));
        }
        // Job membership crosses process creation independently of handle
        // inheritance. A session-1 worker cannot inherit the session-0 host's
        // job. Its own kill-on-close job is assigned atomically at creation,
        // before any user code can run, including on host death during launch.
        const DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                    detail::JobBoundProcessStartup::RequiredCreationFlags() |
                                    detail::CrossSessionBreakawayFlags(callerSession, m_sessionId);
        if (CreateProcessAsUserW(
                m_token.get(),
                m_workerImagePath.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                creationFlags,
                environment.get(),
                DirectoryName(m_workerImagePath).c_str(),
                &startup,
                &processInformation) == FALSE)
        {
            // Preserve the actual launch failure before diagnostics can replace
            // the thread's last-error value. Session IDs contain no audio data.
            const DWORD launchError = GetLastError();
            DWORD workerSession = MAXDWORD;
            DWORD returned = 0U;
            (void)GetTokenInformation(m_token.get(), TokenSessionId, &workerSession, sizeof(workerSession), &returned);
            throw std::runtime_error(
                "CreateProcessAsUserW(audio worker) failed: win32=" + std::to_string(launchError) +
                "; callerSession=" + std::to_string(callerSession) + "; workerSession=" + std::to_string(workerSession));
        }

        wil::unique_handle newProcess(processInformation.hProcess);
        wil::unique_handle suspendedThread(processInformation.hThread);
        BOOL inWorkerJob = FALSE;
        if (!IsProcessInJob(newProcess.get(), m_job.get(), &inWorkerJob) || !inWorkerJob)
        {
            (void)TerminateProcess(newProcess.get(), ERROR_PROCESS_ABORTED);
            (void)WaitForSingleObject(newProcess.get(), 2000U);
            throw std::runtime_error("The audio worker was not atomically assigned to its job");
        }
        if (ResumeThread(suspendedThread.get()) == static_cast<DWORD>(-1))
        {
            (void)TerminateJobObject(m_job.get(), ERROR_PROCESS_ABORTED);
            (void)WaitForSingleObject(newProcess.get(), 2000U);
            throw std::runtime_error("Unable to start the audio worker process");
        }
        m_workerProcessId = processInformation.dwProcessId;
        m_process = std::move(newProcess);
        suspendedThread.reset();
    }

    void ConnectAndVerifyWorker()
    {
        wil::unique_event connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connectEvent)
        {
            throw std::runtime_error("Unable to initialize pipe connection");
        }
        OVERLAPPED operation{};
        operation.hEvent = connectEvent.get();
        BOOL connected = ConnectNamedPipe(m_pipe.get(), &operation);
        if (connected == FALSE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED)
            {
                connected = TRUE;
            }
            else if (error == ERROR_IO_PENDING)
            {
                const DWORD waitResult = WaitForSingleObject(connectEvent.get(), c_pipeConnectTimeoutMs);
                if (waitResult != WAIT_OBJECT_0)
                {
                    (void)CancelIoEx(m_pipe.get(), &operation);
                    (void)WaitForSingleObject(connectEvent.get(), INFINITE);
                    DWORD ignored = 0U;
                    (void)GetOverlappedResult(m_pipe.get(), &operation, &ignored, FALSE);
                    throw std::runtime_error("The audio worker pipe connection timed out");
                }
                DWORD ignored = 0U;
                connected = GetOverlappedResult(m_pipe.get(), &operation, &ignored, FALSE);
            }
            else
            {
                throw std::runtime_error("The audio worker pipe connection failed");
            }
        }
        if (connected == FALSE)
        {
            throw std::runtime_error("The audio worker pipe connection failed");
        }

        ULONG clientProcessId = 0U;
        if (GetNamedPipeClientProcessId(m_pipe.get(), &clientProcessId) == FALSE || clientProcessId != m_workerProcessId ||
            WaitForSingleObject(m_process.get(), 0U) == WAIT_OBJECT_0)
        {
            throw std::runtime_error("The named pipe client is not the launched worker");
        }

        wil::unique_handle clientProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientProcessId));
        if (!clientProcess)
        {
            throw std::runtime_error("Unable to validate the worker client");
        }
        FILETIME expectedCreation{}, expectedExit{}, expectedKernel{}, expectedUser{};
        FILETIME clientCreation{}, clientExit{}, clientKernel{}, clientUserTime{};
        if (GetProcessTimes(m_process.get(), &expectedCreation, &expectedExit, &expectedKernel, &expectedUser) == FALSE ||
            GetProcessTimes(clientProcess.get(), &clientCreation, &clientExit, &clientKernel, &clientUserTime) == FALSE ||
            expectedCreation.dwLowDateTime != clientCreation.dwLowDateTime || expectedCreation.dwHighDateTime != clientCreation.dwHighDateTime)
        {
            throw std::runtime_error("The named pipe worker identity changed");
        }

        HANDLE clientTokenRaw = nullptr;
        if (OpenProcessToken(clientProcess.get(), TOKEN_QUERY, &clientTokenRaw) == FALSE)
        {
            throw std::runtime_error("Unable to validate the worker token");
        }
        wil::unique_handle clientToken(clientTokenRaw);
        std::vector<std::uint8_t> clientUserBytes;
        DWORD clientSession = 0U;
        if (!QueryTokenUserAndSession(clientToken.get(), clientUserBytes, clientSession))
        {
            throw std::runtime_error("The named pipe worker token is invalid");
        }
        const auto* clientUser = reinterpret_cast<const TOKEN_USER*>(clientUserBytes.data());
        if (clientSession != m_sessionId || !IsValidSid(clientUser->User.Sid) ||
            EqualSid(clientUser->User.Sid, const_cast<std::uint8_t*>(m_userSid.data())) == FALSE || !IsSessionActive(m_sessionId))
        {
            throw std::runtime_error("The named pipe worker user session is invalid");
        }

        std::vector<wchar_t> imagePath(32768U, L'\0');
        DWORD imageCharacters = static_cast<DWORD>(imagePath.size());
        if (QueryFullProcessImageNameW(clientProcess.get(), 0U, imagePath.data(), &imageCharacters) == FALSE ||
            imageCharacters == 0U || imageCharacters >= imagePath.size())
        {
            throw std::runtime_error("Unable to validate the worker image identity");
        }
        const std::wstring processImage(imagePath.data(), imageCharacters);
        wil::unique_hfile imageFile(CreateFileW(
            processImage.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!imageFile || !OrdinalEqualsIgnoreCase(FinalPathFromHandle(imageFile.get()), m_workerImagePath))
        {
            throw std::runtime_error("The named pipe worker image is unexpected");
        }
    }

    void CompleteHandshake()
    {
        HANDLE duplicatedServerProcess = nullptr;
        constexpr DWORD c_serverProcessRights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), m_process.get(), &duplicatedServerProcess, c_serverProcessRights, FALSE, 0U) ||
            duplicatedServerProcess == nullptr || duplicatedServerProcess == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Unable to provide the worker a limited service handle");
        }

        Message serverIdentity{};
        serverIdentity.m_header.m_type = MessageType::ServerIdentity;
        serverIdentity.m_header.m_epoch = 0U;
        serverIdentity.m_header.m_sequence = 1U;
        wsl::virtio_sound::audio_ipc::Error error{};
        const auto remoteHandleValue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(duplicatedServerProcess));
        if (!wsl::virtio_sound::audio_ipc::BuildServerIdentityPayload(remoteHandleValue, serverIdentity.m_payload, error) ||
            serverIdentity.m_payload.size() > (std::numeric_limits<std::uint32_t>::max)())
        {
            throw std::runtime_error("Unable to encode the authenticated service handle");
        }
        serverIdentity.m_header.m_payloadBytes = static_cast<std::uint32_t>(serverIdentity.m_payload.size());
        if (!wsl::virtio_sound::audio_ipc::ValidateMessage(serverIdentity, error) ||
            !IsSuccessfulPipeResult(wsl::virtio_sound::audio_ipc::windows::WriteMessage(m_pipe.get(), nullptr, c_pipeMessageTimeoutMs, serverIdentity)))
        {
            // The numeric handle exists only in the child process. On this
            // failure path Shutdown tears down the worker job, which closes it.
            throw std::runtime_error("The authenticated service handle bootstrap failed");
        }

        Message hello{};
        hello.m_header.m_type = MessageType::Hello;
        hello.m_header.m_epoch = 0U;
        hello.m_header.m_sequence = 1U;
        hello.m_header.m_payloadBytes = 0U;
        wsl::virtio_sound::audio_ipc::Error helloError{};
        if (!wsl::virtio_sound::audio_ipc::ValidateMessage(hello, helloError) ||
            !IsSuccessfulPipeResult(wsl::virtio_sound::audio_ipc::windows::WriteMessage(m_pipe.get(), nullptr, c_pipeMessageTimeoutMs, hello)))
        {
            throw std::runtime_error("The audio worker Hello handshake failed");
        }
        Message response{};
        if (!IsSuccessfulPipeResult(wsl::virtio_sound::audio_ipc::windows::ReadMessage(m_pipe.get(), nullptr, c_pipeMessageTimeoutMs, response)) ||
            !IsExpectedMessage(response, MessageType::Ready, 0U, 1U))
        {
            throw std::runtime_error("The audio worker did not complete its Ready handshake");
        }
        m_lastInboundSequence = 1U;
        m_lastOutboundSequence = 1U;
    }

    bool NextSequence(std::uint64_t last, std::uint64_t& candidate) const noexcept
    {
        if (last == (std::numeric_limits<std::uint64_t>::max)())
        {
            return false;
        }
        candidate = last + 1U;
        return candidate != 0U;
    }

    bool SendControl(MessageType type, std::uint64_t epoch, std::uint64_t& lastOutboundSequence)
    {
        std::uint64_t sequence = 0U;
        if (!NextSequence(lastOutboundSequence, sequence))
        {
            return false;
        }
        Message message{};
        message.m_header.m_type = type;
        message.m_header.m_epoch = epoch;
        message.m_header.m_sequence = sequence;
        if (type == MessageType::Start)
        {
            message.m_payload = m_startPayload;
        }
        if (message.m_payload.size() > (std::numeric_limits<std::uint32_t>::max)())
        {
            return false;
        }
        message.m_header.m_payloadBytes = static_cast<std::uint32_t>(message.m_payload.size());
        wsl::virtio_sound::audio_ipc::Error error{};
        if (!wsl::virtio_sound::audio_ipc::ValidateMessage(message, error) ||
            !IsSuccessfulPipeResult(wsl::virtio_sound::audio_ipc::windows::WriteMessage(
                m_pipe.get(), m_transportCancelEvent.get(), c_pipeMessageTimeoutMs, message)))
        {
            return false;
        }
        lastOutboundSequence = sequence;
        return true;
    }

    PeekResult PeekCompleteMessage(Message& message, std::chrono::steady_clock::time_point& partialFrameStart)
    {
        std::array<std::uint8_t, wsl::virtio_sound::audio_ipc::c_headerBytes> bytes{};
        DWORD bytesRead = 0U;
        DWORD totalBytesAvailable = 0U;
        if (PeekNamedPipe(m_pipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, &totalBytesAvailable, nullptr) == FALSE)
        {
            return PeekResult::Disconnected;
        }
        if (totalBytesAvailable == 0U)
        {
            partialFrameStart = std::chrono::steady_clock::time_point{};
            return PeekResult::NoMessage;
        }
        if (bytesRead < bytes.size())
        {
            if (partialFrameStart == std::chrono::steady_clock::time_point{})
            {
                partialFrameStart = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() - partialFrameStart > std::chrono::milliseconds(c_partialFrameTimeoutMs))
            {
                return PeekResult::InvalidFrame;
            }
            return PeekResult::NoMessage;
        }
        Header header{};
        wsl::virtio_sound::audio_ipc::Error error{};
        if (!wsl::virtio_sound::audio_ipc::DecodeHeader(bytes.data(), bytes.size(), header, error))
        {
            return PeekResult::InvalidFrame;
        }
        const std::size_t frameBytes = wsl::virtio_sound::audio_ipc::c_headerBytes + static_cast<std::size_t>(header.m_payloadBytes);
        if (totalBytesAvailable < frameBytes)
        {
            if (partialFrameStart == std::chrono::steady_clock::time_point{})
            {
                partialFrameStart = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() - partialFrameStart > std::chrono::milliseconds(c_partialFrameTimeoutMs))
            {
                return PeekResult::InvalidFrame;
            }
            return PeekResult::NoMessage;
        }
        partialFrameStart = std::chrono::steady_clock::time_point{};
        const auto result =
            wsl::virtio_sound::audio_ipc::windows::ReadMessage(m_pipe.get(), m_transportCancelEvent.get(), c_pipeMessageTimeoutMs, message);
        if (!IsSuccessfulPipeResult(result))
        {
            return result == wsl::virtio_sound::audio_ipc::windows::PipeResult::Disconnected ? PeekResult::Disconnected
                                                                                             : PeekResult::InvalidFrame;
        }
        return PeekResult::CompleteMessage;
    }

    bool ValidateInboundSequence(const Message& message, std::uint64_t& lastInboundSequence) const noexcept
    {
        std::uint64_t expected = 0U;
        if (!NextSequence(lastInboundSequence, expected))
        {
            return false;
        }
        wsl::virtio_sound::audio_ipc::Error error{};
        if (!wsl::virtio_sound::audio_ipc::ValidateMessage(message, error) || message.m_header.m_sequence != expected)
        {
            return false;
        }
        lastInboundSequence = expected;
        return true;
    }

    void SetWirePhase(WirePhase phase, std::uint64_t epoch, std::uint64_t generation = 0U) noexcept
    {
        auto stateLock = m_stateLock.lock_exclusive();
        m_wirePhase = phase;
        m_wireEpoch = epoch;
        m_wireGeneration = generation;
        m_stopDeadline.OnWirePhaseChanged(phase);
    }

    bool IsSessionStillActive() const noexcept
    {
        return IsSessionActive(m_sessionId);
    }

    void ForceTerminateWorker() noexcept
    {
        if (m_transportCancelEvent)
        {
            (void)SetEvent(m_transportCancelEvent.get());
        }
        if (m_pipe)
        {
            (void)CancelIoEx(m_pipe.get(), nullptr);
        }
        if (m_job)
        {
            (void)TerminateJobObject(m_job.get(), ERROR_PROCESS_ABORTED);
        }
        if (m_process)
        {
            DWORD wait = WaitForSingleObject(m_process.get(), 2000U);
            if (wait != WAIT_OBJECT_0)
            {
                (void)TerminateProcess(m_process.get(), ERROR_PROCESS_ABORTED);
                (void)WaitForSingleObject(m_process.get(), 2000U);
            }
        }
    }

    bool StartWorkerEpoch(std::uint64_t epoch, std::uint64_t generation, std::uint64_t& lastOutboundSequence)
    {
        if (!SendControl(MessageType::Start, epoch, lastOutboundSequence))
        {
            return false;
        }
        SetWirePhase(WirePhase::StartSent, epoch, generation);
        {
            auto stateLock = m_stateLock.lock_exclusive();
            m_stopDeadline.AfterStartSent(
                m_desiredRunning && m_desiredEpoch == epoch, std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
        }
        return true;
    }

    bool StopWorkerEpoch(std::uint64_t epoch, std::uint64_t& lastOutboundSequence)
    {
        if (!SendControl(MessageType::Stop, epoch, lastOutboundSequence))
        {
            return false;
        }
        SetWirePhase(WirePhase::StopSent, epoch);
        {
            auto stateLock = m_stateLock.lock_exclusive();
            m_stopDeadline.AfterStopSent(std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
        }
        return true;
    }

    bool SendShutdown(std::uint64_t& lastOutboundSequence)
    {
        if (!SendControl(MessageType::Shutdown, 0U, lastOutboundSequence))
        {
            return false;
        }
        SetWirePhase(WirePhase::ShutdownSent, 0U);
        auto stateLock = m_stateLock.lock_exclusive();
        m_shutdownDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_workerCloseTimeoutMs);
        return true;
    }

    bool CheckDeadlines(WirePhase phase, std::chrono::steady_clock::time_point now) noexcept
    {
        auto stateLock = m_stateLock.lock_exclusive();
        if (m_terminalFailure)
        {
            return false;
        }
        if (phase == WirePhase::StartSent && now >= m_startDeadline)
        {
            return false;
        }
        if (phase == WirePhase::Capturing && m_desiredRunning && m_readiness == SourceReadiness::Pending && now >= m_startDeadline)
        {
            return false;
        }
        if (m_stopDeadline.Expired(phase, now))
        {
            return false;
        }
        if (phase == WirePhase::ShutdownSent && now >= m_shutdownDeadline)
        {
            return false;
        }
        if (m_desiredRunning && phase == WirePhase::Idle && now >= m_startDeadline)
        {
            return false;
        }
        return true;
    }

    void MarkReadyWhenPrimed(std::uint64_t epoch, std::uint64_t generation, std::uint32_t periodFrames) noexcept
    {
        const PcmRingSnapshot snapshot = m_ring.Snapshot();
        if (snapshot.m_generation != generation || snapshot.m_queuedFrames < periodFrames)
        {
            return;
        }
        auto stateLock = m_stateLock.lock_exclusive();
        if (!m_terminalFailure && m_desiredRunning && m_desiredEpoch == epoch && m_desiredGeneration == generation &&
            m_readiness == SourceReadiness::Ready && !m_ringPrimedReported)
        {
            m_ringPrimedReported = true;
            detail::TraceAudioSource(L"RingPrimed", snapshot.m_queuedFrames, periodFrames);
        }
    }

    bool HandleInbound(const Message& message, WirePhase& phase, std::uint64_t& activeEpoch, std::uint64_t& activeGeneration, std::uint64_t& lastInboundSequence)
    {
        if (!ValidateInboundSequence(message, lastInboundSequence))
        {
            return false;
        }
        switch (message.m_header.m_type)
        {
        case MessageType::Started:
        {
            detail::TraceAudioSource(L"WorkerStarted", message.m_header.m_epoch);
            if (phase != WirePhase::StartSent || message.m_header.m_epoch != activeEpoch || !message.m_payload.empty())
            {
                return false;
            }
            phase = WirePhase::Capturing;
            SetWirePhase(phase, activeEpoch, activeGeneration);
            bool becameReady = false;
            {
                auto stateLock = m_stateLock.lock_exclusive();
                if (!m_terminalFailure && m_desiredRunning && m_desiredEpoch == activeEpoch &&
                    m_desiredGeneration == activeGeneration && m_readiness == SourceReadiness::Pending)
                {
                    m_readiness = SourceReadiness::Ready;
                    becameReady = true;
                }
                if (!m_desiredRunning || m_desiredEpoch != activeEpoch || m_desiredGeneration != activeGeneration)
                {
                    m_stopDeadline.ArmIfMissing(std::chrono::steady_clock::now(), std::chrono::milliseconds(c_workerStopTimeoutMs));
                }
            }
            if (becameReady)
            {
                detail::TraceAudioSource(L"CaptureReady", activeEpoch, activeGeneration);
            }
            return true;
        }
        case MessageType::Stopped:
            if (phase != WirePhase::StopSent || message.m_header.m_epoch != activeEpoch || !message.m_payload.empty())
            {
                return false;
            }
            phase = WirePhase::Idle;
            activeEpoch = 0U;
            activeGeneration = 0U;
            SetWirePhase(phase, 0U);
            return true;
        case MessageType::Pcm:
        {
            if ((phase != WirePhase::Capturing && phase != WirePhase::StopSent) || message.m_header.m_epoch != activeEpoch ||
                message.m_payload.empty() || message.m_payload.size() % c_pcmBytesPerFrame != 0U ||
                message.m_payload.size() > wsl::virtio_sound::audio_ipc::c_maxPayloadBytes)
            {
                return false;
            }
            const std::size_t frames = message.m_payload.size() / c_pcmBytesPerFrame;
            if (frames == 0U || frames > c_maximumPcmFrames)
            {
                return false;
            }
            const PcmRingWriteResult writeResult =
                m_ring.TryWriteFrames(activeGeneration, message.m_payload.data(), frames, message.m_payload.size());
            if (writeResult == PcmRingWriteResult::StaleGeneration)
            {
                auto stateLock = m_stateLock.lock_exclusive();
                if (m_terminalFailure)
                {
                    return false;
                }
                if (!m_desiredRunning || m_desiredEpoch != activeEpoch || m_desiredGeneration != activeGeneration)
                {
                    return true;
                }
                return false;
            }
            if (writeResult != PcmRingWriteResult::Written)
            {
                return false;
            }
            std::uint32_t periodFrames = 0U;
            {
                auto stateLock = m_stateLock.lock_exclusive();
                if (m_desiredRunning && m_desiredEpoch == activeEpoch && m_desiredGeneration == activeGeneration)
                {
                    periodFrames = m_desiredPeriodFrames;
                }
            }
            if (periodFrames != 0U)
            {
                MarkReadyWhenPrimed(activeEpoch, activeGeneration, periodFrames);
            }
            return true;
        }
        case MessageType::Fault:
            if (message.m_payload.size() == 4U)
            {
                const auto* p = message.m_payload.data();
                const std::uint32_t code = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
                                           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
                detail::TraceAudioSource(L"WorkerFault", code, message.m_header.m_epoch);
            }
            else
            {
                detail::TraceAudioSource(L"MalformedWorkerFault", message.m_payload.size());
            }
            return false;
        case MessageType::Closed:
            if (phase != WirePhase::ShutdownSent || message.m_header.m_epoch != 0U || !message.m_payload.empty())
            {
                return false;
            }
            phase = WirePhase::Closed;
            SetWirePhase(phase, 0U);
            return true;
        case MessageType::Hello:
        case MessageType::Ready:
        case MessageType::Start:
        case MessageType::Stop:
        case MessageType::Shutdown:
        case MessageType::ServerIdentity:
        default:
            return false;
        }
    }

    void ControllerMain() noexcept
    {
        bool gracefulClose = false;
        try
        {
            gracefulClose = RunController();
        }
        catch (...)
        {
            MarkFailed(__LINE__);
            gracefulClose = false;
        }
        if (gracefulClose)
        {
            const DWORD waitResult = m_process ? WaitForSingleObject(m_process.get(), c_workerCloseTimeoutMs) : WAIT_OBJECT_0;
            if (waitResult != WAIT_OBJECT_0)
            {
                ForceTerminateWorker();
            }
        }
        else
        {
            ForceTerminateWorker();
        }
        m_pipe.reset();
        {
            auto stateLock = m_stateLock.lock_exclusive();
            m_controllerFinished = true;
        }
    }

    bool RunController()
    {
        WirePhase phase = WirePhase::Idle;
        std::uint64_t activeEpoch = 0U;
        std::uint64_t activeGeneration = 0U;
        std::uint64_t lastOutboundSequence = m_lastOutboundSequence;
        std::uint64_t lastInboundSequence = m_lastInboundSequence;
        std::chrono::steady_clock::time_point nextSessionCheck{};
        std::chrono::steady_clock::time_point partialFrameStart{};

        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            if (m_process && WaitForSingleObject(m_process.get(), 0U) == WAIT_OBJECT_0)
            {
                DWORD exitCode = MAXDWORD;
                (void)GetExitCodeProcess(m_process.get(), &exitCode);
                detail::TraceAudioSource(L"WorkerExited", exitCode);
                // A worker can emit Fault then exit between controller polls.
                // Preserve those already-buffered diagnostics before closing
                // the pipe. The worker is dead, so there is no producer; only
                // fully available, protocol-validated frames are consumed.
                for (unsigned remaining = 32; remaining != 0; --remaining)
                {
                    Message finalMessage{};
                    if (PeekCompleteMessage(finalMessage, partialFrameStart) != PeekResult::CompleteMessage)
                    {
                        break;
                    }
                    if (!HandleInbound(finalMessage, phase, activeEpoch, activeGeneration, lastInboundSequence))
                    {
                        break;
                    }
                }
                MarkFailed(__LINE__);
                return false;
            }

            bool shuttingDown = false;
            bool desiredRunning = false;
            std::uint64_t desiredEpoch = 0U;
            std::uint64_t desiredGeneration = 0U;
            {
                auto stateLock = m_stateLock.lock_exclusive();
                if (m_terminalFailure)
                {
                    return false;
                }
                shuttingDown = m_shutdownRequested;
                desiredRunning = m_desiredRunning;
                desiredEpoch = m_desiredEpoch;
                desiredGeneration = m_desiredGeneration;
            }

            if (!CheckDeadlines(phase, now))
            {
                MarkFailed(__LINE__);
                return false;
            }

            if (phase == WirePhase::StartSent || phase == WirePhase::Capturing || phase == WirePhase::StopSent)
            {
                if (nextSessionCheck == std::chrono::steady_clock::time_point{} || now >= nextSessionCheck)
                {
                    if (!IsSessionStillActive())
                    {
                        MarkFailed(__LINE__);
                        return false;
                    }
                    nextSessionCheck = now + std::chrono::milliseconds(c_sessionCheckCadenceMs);
                }
            }

            if (phase == WirePhase::StartSent && (!desiredRunning || desiredEpoch != activeEpoch))
            {
                auto stateLock = m_stateLock.lock_exclusive();
                m_stopDeadline.ArmIfMissing(now, std::chrono::milliseconds(c_workerStopTimeoutMs));
            }

            if (shuttingDown && phase == WirePhase::Idle)
            {
                if (!SendShutdown(lastOutboundSequence))
                {
                    MarkFailed(__LINE__);
                    return false;
                }
                phase = WirePhase::ShutdownSent;
                continue;
            }
            if (!shuttingDown && phase == WirePhase::Idle && desiredRunning)
            {
                if (!StartWorkerEpoch(desiredEpoch, desiredGeneration, lastOutboundSequence))
                {
                    MarkFailed(__LINE__);
                    return false;
                }
                phase = WirePhase::StartSent;
                activeEpoch = desiredEpoch;
                activeGeneration = desiredGeneration;
                continue;
            }
            if ((phase == WirePhase::Capturing || phase == WirePhase::StartSent) && (!desiredRunning || desiredEpoch != activeEpoch))
            {
                // A Start already accepted by the worker must be acknowledged
                // before its exact epoch can be stopped.
                if (phase == WirePhase::Capturing)
                {
                    if (!StopWorkerEpoch(activeEpoch, lastOutboundSequence))
                    {
                        MarkFailed(__LINE__);
                        return false;
                    }
                    phase = WirePhase::StopSent;
                    continue;
                }
            }
            if (shuttingDown && phase == WirePhase::Capturing)
            {
                if (!StopWorkerEpoch(activeEpoch, lastOutboundSequence))
                {
                    MarkFailed(__LINE__);
                    return false;
                }
                phase = WirePhase::StopSent;
                continue;
            }

            Message incoming{};
            const PeekResult peek = PeekCompleteMessage(incoming, partialFrameStart);
            if (peek == PeekResult::Disconnected || peek == PeekResult::InvalidFrame)
            {
                MarkFailed(__LINE__);
                return false;
            }
            if (peek == PeekResult::CompleteMessage)
            {
                if (!HandleInbound(incoming, phase, activeEpoch, activeGeneration, lastInboundSequence))
                {
                    MarkFailed(__LINE__);
                    return false;
                }
                if (phase == WirePhase::Closed)
                {
                    return true;
                }
                continue;
            }

            if (phase == WirePhase::ShutdownSent && shuttingDown)
            {
                // Shutdown/Closed has its own two-second deadline.
            }
            const DWORD waitResult = WaitForSingleObject(m_wakeEvent.get(), c_controllerCadenceMs);
            if (waitResult == WAIT_FAILED)
            {
                MarkFailed(__LINE__);
                return false;
            }
        }
    }

    mutable wil::srwlock m_stateLock;
    wil::srwlock m_shutdownLock;
    wsl::virtio_sound::native::S16LePcmRingSource m_ring;
    wil::unique_handle m_token;
    wil::unique_hfile m_pipe;
    wil::unique_handle m_job;
    wil::unique_handle m_process;
    wil::unique_event m_wakeEvent;
    wil::unique_event m_transportCancelEvent;
    std::thread m_controller;
    std::wstring m_workerImagePath;
    std::wstring m_pipeName;
    std::vector<std::uint8_t> m_startPayload;
    std::vector<std::uint8_t> m_userSid;
    DWORD m_sessionId{};
    DWORD m_workerProcessId{};
    _Guarded_by_(m_stateLock) SourceReadiness m_readiness { SourceReadiness::Ready };
    _Guarded_by_(m_stateLock) WirePhase m_wirePhase { WirePhase::Idle };
    _Guarded_by_(m_stateLock) bool m_terminalFailure {};
    _Guarded_by_(m_stateLock) bool m_shutdownRequested {};
    _Guarded_by_(m_shutdownLock) bool m_shutdownComplete {};
    _Guarded_by_(m_stateLock) bool m_controllerFinished {};
    _Guarded_by_(m_stateLock) bool m_desiredRunning {};
    _Guarded_by_(m_stateLock) bool m_ringPrimedReported {};
    _Guarded_by_(m_stateLock) std::uint64_t m_highestEpoch {};
    _Guarded_by_(m_stateLock) std::uint64_t m_desiredEpoch {};
    _Guarded_by_(m_stateLock) std::uint64_t m_desiredGeneration {};
    _Guarded_by_(m_stateLock) std::uint64_t m_wireEpoch {};
    _Guarded_by_(m_stateLock) std::uint64_t m_wireGeneration {};
    _Guarded_by_(m_stateLock) std::uint32_t m_desiredPeriodFrames {};
    _Guarded_by_(m_stateLock) detail::CaptureStopDeadline m_stopDeadline;
    _Guarded_by_(m_stateLock) std::chrono::steady_clock::time_point m_startDeadline {};
    _Guarded_by_(m_stateLock) std::chrono::steady_clock::time_point m_shutdownDeadline {};
    std::uint64_t m_lastInboundSequence{};
    std::uint64_t m_lastOutboundSequence{};
};

WindowsCaptureSource::WindowsCaptureSource(std::unique_ptr<Impl> implementation) noexcept : m_impl(std::move(implementation))
{
}

std::shared_ptr<WindowsCaptureSource> WindowsCaptureSource::Create(HANDLE restrictedUserToken, const std::wstring& workerPath, const std::wstring& endpointId)
{
    auto implementation = std::make_unique<Impl>(c_captureRingFrames);
    try
    {
        implementation->Initialize(restrictedUserToken, workerPath, endpointId);
        return std::shared_ptr<WindowsCaptureSource>(new WindowsCaptureSource(std::move(implementation)));
    }
    catch (...)
    {
        if (implementation)
        {
            implementation->Shutdown();
        }
        throw;
    }
}

WindowsCaptureSource::~WindowsCaptureSource()
{
    Shutdown();
}

bool WindowsCaptureSource::StartCapture(std::uint32_t periodFrames) noexcept
{
    return m_impl != nullptr && m_impl->StartCapture(periodFrames);
}

void WindowsCaptureSource::StopCapture() noexcept
{
    if (m_impl != nullptr)
    {
        m_impl->StopCapture();
    }
}

SourceReadiness WindowsCaptureSource::CaptureReadiness() const noexcept
{
    return m_impl == nullptr ? SourceReadiness::Failed : m_impl->CaptureReadiness();
}

std::optional<std::uint32_t> WindowsCaptureSource::AvailableFrames() const noexcept
{
    return m_impl == nullptr ? std::optional<std::uint32_t>{0U} : m_impl->AvailableFrames();
}

bool WindowsCaptureSource::ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept
{
    return m_impl != nullptr && m_impl->ReadFrames(frames, destination, byteCount);
}

void WindowsCaptureSource::Reset() noexcept
{
    if (m_impl != nullptr)
    {
        m_impl->Reset();
    }
}

void WindowsCaptureSource::Shutdown() noexcept
{
    if (m_impl != nullptr)
    {
        m_impl->Shutdown();
    }
}

} // namespace wsl::virtio_sound::native::windows
