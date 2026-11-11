// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "AudioWorkerSession.hpp"
#include "AudioPipeTransport.hpp"
#include "windows/ServerIdentityValidation.hpp"

namespace {

using wsl::virtio_sound::audio_ipc::Message;
using wsl::virtio_sound::audio_ipc::windows::PipeResult;
using wsl::virtio_sound::audio_worker::AudioWorkerPollResult;
using wsl::virtio_sound::audio_worker::AudioWorkerSession;
using wsl::virtio_sound::audio_worker::AudioWorkerSessionState;
using wsl::virtio_sound::audio_worker::IAudioWorkerMessageEmitter;

constexpr DWORD c_pipeConnectTimeoutMs = 5000U;
constexpr DWORD c_messageTimeoutMs = 1000U;
constexpr DWORD c_idlePollIntervalMs = 5U;
constexpr std::size_t c_maximumPipePathCodeUnits = 256U;

bool IsTerminal(AudioWorkerSessionState state) noexcept
{
    return state == AudioWorkerSessionState::Closed || state == AudioWorkerSessionState::Faulted || state == AudioWorkerSessionState::Disconnected;
}

class PipeEmitter final : public IAudioWorkerMessageEmitter
{
public:
    PipeEmitter(HANDLE pipe, HANDLE stopEvent) noexcept : m_pipe(pipe), m_stopEvent(stopEvent)
    {
    }

    bool Emit(const Message& message) noexcept override
    {
        return wsl::virtio_sound::audio_ipc::windows::WriteMessage(m_pipe, m_stopEvent, c_messageTimeoutMs, message) == PipeResult::Success;
    }

private:
    HANDLE m_pipe;
    HANDLE m_stopEvent;
};

bool ValidatePipeArgument(const wchar_t* value, std::wstring& path)
{
    if (value == nullptr)
    {
        return false;
    }
    const std::wstring candidate(value);
    constexpr wchar_t c_pipePrefix[] = L"\\\\.\\pipe\\";
    constexpr std::size_t c_pipePrefixLength = (sizeof(c_pipePrefix) / sizeof(c_pipePrefix[0])) - 1U;
    if (candidate.size() <= c_pipePrefixLength || candidate.size() > c_maximumPipePathCodeUnits ||
        _wcsnicmp(candidate.c_str(), c_pipePrefix, c_pipePrefixLength) != 0)
    {
        return false;
    }
    for (const wchar_t character : candidate)
    {
        if (character < 0x20 || character == L'/')
        {
            return false;
        }
    }
    path = candidate;
    return true;
}

bool ParseServerPid(const wchar_t* value, DWORD& processId) noexcept
{
    if (value == nullptr)
    {
        return false;
    }
    std::uint64_t parsed = 0U;
    std::size_t digits = 0U;
    for (const wchar_t* cursor = value; *cursor != L'\0'; ++cursor)
    {
        if (*cursor < L'0' || *cursor > L'9' || digits >= 10U)
        {
            return false;
        }
        parsed = (parsed * 10U) + static_cast<std::uint64_t>(*cursor - L'0');
        if (parsed > (std::numeric_limits<DWORD>::max)())
        {
            return false;
        }
        ++digits;
    }
    if (digits == 0U || parsed == 0U)
    {
        return false;
    }
    processId = static_cast<DWORD>(parsed);
    return true;
}

bool GetCurrentExecutablePath(std::wstring& path)
{
    std::array<wchar_t, 32768U> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size())
    {
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

bool AdjacentServicePath(const std::wstring& executablePath, std::wstring& servicePath)
{
    const std::size_t separator = executablePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return false;
    }
    servicePath.assign(executablePath, 0U, separator + 1U);
    servicePath.append(L"wslservice.exe");
    return true;
}

bool IsNamedPipeServer(HANDLE pipe, DWORD expectedProcessId) noexcept
{
    ULONG serverProcessId = 0U;
    if (GetNamedPipeServerProcessId(pipe, &serverProcessId) == FALSE || serverProcessId != expectedProcessId)
    {
        return false;
    }
    return true;
}

bool ReceiveAuthenticatedServerIdentity(HANDLE pipe, DWORD expectedProcessId, HANDLE stopEvent, const std::wstring& expectedServicePath, HANDLE& authenticatedProcess)
{
    authenticatedProcess = nullptr;
    if (!IsNamedPipeServer(pipe, expectedProcessId))
    {
        return false;
    }

    Message bootstrap{};
    if (wsl::virtio_sound::audio_ipc::windows::ReadMessage(pipe, stopEvent, c_messageTimeoutMs, bootstrap) != PipeResult::Success)
    {
        return false;
    }

    std::uint64_t handleValue = 0U;
    if (!wsl::virtio_sound::audio_worker::windows::detail::ParseServerIdentityBootstrap(bootstrap, handleValue))
    {
        return false;
    }

    const HANDLE candidate = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handleValue));
    if (!wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(pipe, expectedProcessId, candidate, expectedServicePath) ||
        !IsNamedPipeServer(pipe, expectedProcessId))
    {
        // Do not CloseHandle an untrusted number. If it was a remote duplicate,
        // worker process exit closes it; if malformed, it may alias an owned
        // local handle. Process exit is the safe cleanup boundary.
        return false;
    }
    authenticatedProcess = candidate;
    return true;
}

HANDLE ConnectPipe(const std::wstring& pipePath, DWORD expectedServerPid, HANDLE stopEvent)
{
    const ULONGLONG deadline = GetTickCount64() + c_pipeConnectTimeoutMs;
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = FALSE;

    for (;;)
    {
        if (WaitForSingleObject(stopEvent, 0U) == WAIT_OBJECT_0)
        {
            return INVALID_HANDLE_VALUE;
        }
        wil::unique_hfile pipe{CreateFileW(
            pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, &securityAttributes, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
        if (pipe)
        {
            DWORD mode = PIPE_READMODE_BYTE;
            if (GetTickCount64() >= deadline || SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr) == FALSE ||
                !IsNamedPipeServer(pipe.get(), expectedServerPid))
            {
                return INVALID_HANDLE_VALUE;
            }
            if (GetTickCount64() >= deadline)
            {
                return INVALID_HANDLE_VALUE;
            }
            return pipe.release();
        }

        const DWORD error = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return INVALID_HANDLE_VALUE;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        if (error == ERROR_PIPE_BUSY)
        {
            if (WaitNamedPipeW(pipePath.c_str(), remaining) == FALSE)
            {
                const DWORD waitError = GetLastError();
                if (waitError != ERROR_SEM_TIMEOUT && waitError != ERROR_FILE_NOT_FOUND)
                {
                    return INVALID_HANDLE_VALUE;
                }
            }
        }
        else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            const DWORD nap = (std::min)(remaining, static_cast<DWORD>(25U));
            if (WaitForSingleObject(stopEvent, nap) == WAIT_OBJECT_0)
            {
                return INVALID_HANDLE_VALUE;
            }
        }
        else
        {
            return INVALID_HANDLE_VALUE;
        }
    }
}

int RunSession(HANDLE pipe, HANDLE stopEvent, HANDLE authenticatedServerProcess)
{
    if (authenticatedServerProcess == nullptr || authenticatedServerProcess == INVALID_HANDLE_VALUE)
    {
        return 69;
    }
    AudioWorkerSession session;
    PipeEmitter emitter(pipe, stopEvent);

    while (WaitForSingleObject(stopEvent, 0U) != WAIT_OBJECT_0)
    {
        const DWORD serverWait = WaitForSingleObject(authenticatedServerProcess, 0U);
        if (serverWait != WAIT_TIMEOUT)
        {
            session.OnTransportDisconnected();
            return 2;
        }
        DWORD availableBytes = 0U;
        if (PeekNamedPipe(pipe, nullptr, 0U, nullptr, &availableBytes, nullptr) == FALSE)
        {
            session.OnTransportDisconnected();
            return 2;
        }

        if (availableBytes != 0U)
        {
            Message request{};
            const PipeResult readResult = wsl::virtio_sound::audio_ipc::windows::ReadMessage(pipe, stopEvent, c_messageTimeoutMs, request);
            if (readResult != PipeResult::Success)
            {
                session.OnTransportDisconnected();
                return 2;
            }

            const AudioWorkerSessionState state = session.HandleControl(request, emitter);
            if (state == AudioWorkerSessionState::Closed)
            {
                return 0;
            }
            if (state == AudioWorkerSessionState::Faulted || state == AudioWorkerSessionState::Disconnected)
            {
                return 2;
            }
            continue;
        }

        const AudioWorkerPollResult pollResult = session.Poll(emitter);
        if (IsTerminal(session.State()) || pollResult == AudioWorkerPollResult::Terminal)
        {
            return session.State() == AudioWorkerSessionState::Closed ? 0 : 2;
        }
        if (WaitForSingleObject(stopEvent, c_idlePollIntervalMs) == WAIT_OBJECT_0)
        {
            break;
        }
    }

    session.OnTransportDisconnected();
    return 0;
}

} // namespace

int WorkerMain(int argc, wchar_t** argv)
{
    if (argc != 5 || argv == nullptr || argv[1] == nullptr || argv[2] == nullptr || argv[3] == nullptr || argv[4] == nullptr ||
        std::wstring(argv[1]) != L"--pipe" || std::wstring(argv[3]) != L"--server-pid")
    {
        return 64;
    }

    std::wstring pipePath;
    DWORD serverPid = 0U;
    if (!ValidatePipeArgument(argv[2], pipePath) || !ParseServerPid(argv[4], serverPid))
    {
        return 64;
    }

    std::wstring executablePath;
    std::wstring expectedServicePath;
    if (!GetCurrentExecutablePath(executablePath) || !AdjacentServicePath(executablePath, expectedServicePath))
    {
        return 65;
    }

    wil::unique_event stopEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!stopEvent)
    {
        return 66;
    }

    const HANDLE connectedPipe = ConnectPipe(pipePath, serverPid, stopEvent.get());
    wil::unique_hfile pipe{connectedPipe};
    if (!pipe)
    {
        return 67;
    }

    HANDLE authenticatedServerRaw = nullptr;
    if (!ReceiveAuthenticatedServerIdentity(pipe.get(), serverPid, stopEvent.get(), expectedServicePath, authenticatedServerRaw))
    {
        return 68;
    }
    wil::unique_handle authenticatedServer{authenticatedServerRaw};

    return RunSession(pipe.get(), stopEvent.get(), authenticatedServer.get());
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return WorkerMain(argc, argv);
    }
    catch (...)
    {
        // Keep unexpected allocation and session-construction failures inside
        // the worker's process boundary. Stack RAII closes the pipe and event.
        return 70;
    }
}
