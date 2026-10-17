// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <wil/resource.h>

#include "AudioIpcCodec.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <limits>
#include <string>

namespace wsl::virtio_sound::audio_worker::windows::detail {

// A broker-supplied numeric value is not trusted as a handle until it has
// passed these checks. Reject both native-width and zero-extended 32-bit
// pseudo-handle encodings (current process/thread and token pseudo handles).
inline bool IsRealProcessHandleValue(std::uint64_t value) noexcept
{
    if (value == 0U || value > static_cast<std::uint64_t>((std::numeric_limits<std::uintptr_t>::max)()))
    {
        return false;
    }

    constexpr std::uint64_t c_pseudoHandles[] = {
        UINT64_C(0xffffffffffffffff),
        UINT64_C(0xfffffffffffffffe),
        UINT64_C(0xfffffffffffffffd),
        UINT64_C(0xfffffffffffffffc),
        UINT64_C(0xfffffffffffffffb),
        UINT64_C(0xfffffffffffffffa),
        UINT64_C(0x00000000ffffffff),
        UINT64_C(0x00000000fffffffe),
        UINT64_C(0x00000000fffffffd),
        UINT64_C(0x00000000fffffffc),
        UINT64_C(0x00000000fffffffb),
        UINT64_C(0x00000000fffffffa),
    };
    for (const std::uint64_t pseudo : c_pseudoHandles)
    {
        if (value == pseudo)
        {
            return false;
        }
    }
    return true;
}

inline bool ParseServerIdentityBootstrap(const wsl::virtio_sound::audio_ipc::Message& message, std::uint64_t& processHandleValue) noexcept
{
    wsl::virtio_sound::audio_ipc::Error error{};
    if (!wsl::virtio_sound::audio_ipc::ValidateMessage(message, error) ||
        message.m_header.m_type != wsl::virtio_sound::audio_ipc::MessageType::ServerIdentity || message.m_header.m_epoch != 0U ||
        message.m_header.m_sequence != 1U)
    {
        return false;
    }

    std::uint64_t parsedValue = 0U;
    if (!wsl::virtio_sound::audio_ipc::ParseServerIdentityPayload(message.m_payload.data(), message.m_payload.size(), parsedValue, error) ||
        !IsRealProcessHandleValue(parsedValue))
    {
        return false;
    }
    processHandleValue = parsedValue;
    return true;
}

inline bool GetFinalPath(const std::wstring& inputPath, std::wstring& finalPath)
{
    wil::unique_hfile file{CreateFileW(
        inputPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (!file)
    {
        return false;
    }

    std::array<wchar_t, 32768U> buffer{};
    const DWORD length =
        GetFinalPathNameByHandleW(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0U || length >= buffer.size())
    {
        return false;
    }
    finalPath.assign(buffer.data(), length);
    return true;
}

// The service first authenticates the connected worker and duplicates a
// limited-rights handle to itself into that worker. The worker validates that
// received handle against the kernel-reported pipe server PID and the exact
// adjacent service image before creating AudioWorkerSession.
inline bool ValidateServerProcessHandle(HANDLE pipe, DWORD expectedProcessId, HANDLE process, const std::wstring& expectedServicePath)
{
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || expectedProcessId == 0U ||
        !IsRealProcessHandleValue(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(process))))
    {
        return false;
    }

    ULONG serverProcessId = 0U;
    if (GetNamedPipeServerProcessId(pipe, &serverProcessId) == FALSE || serverProcessId != expectedProcessId ||
        GetProcessId(process) != expectedProcessId || WaitForSingleObject(process, 0U) != WAIT_TIMEOUT)
    {
        return false;
    }

    std::array<wchar_t, 32768U> imageBuffer{};
    DWORD imageLength = static_cast<DWORD>(imageBuffer.size());
    if (QueryFullProcessImageNameW(process, 0U, imageBuffer.data(), &imageLength) == FALSE || imageLength == 0U ||
        imageLength >= imageBuffer.size())
    {
        return false;
    }

    std::wstring actualImagePath;
    std::wstring expectedFinalPath;
    if (!GetFinalPath(std::wstring(imageBuffer.data(), imageLength), actualImagePath) || !GetFinalPath(expectedServicePath, expectedFinalPath) ||
        actualImagePath.size() > static_cast<std::size_t>(INT_MAX) || expectedFinalPath.size() > static_cast<std::size_t>(INT_MAX) ||
        CompareStringOrdinal(
            actualImagePath.data(),
            static_cast<int>(actualImagePath.size()),
            expectedFinalPath.data(),
            static_cast<int>(expectedFinalPath.size()),
            TRUE) != CSTR_EQUAL)
    {
        return false;
    }

    ULONG verifiedServerProcessId = 0U;
    return WaitForSingleObject(process, 0U) == WAIT_TIMEOUT && GetProcessId(process) == expectedProcessId &&
           GetNamedPipeServerProcessId(pipe, &verifiedServerProcessId) != FALSE && verifiedServerProcessId == expectedProcessId;
}

} // namespace wsl::virtio_sound::audio_worker::windows::detail
