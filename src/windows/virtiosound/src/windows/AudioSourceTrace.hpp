// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <Windows.h>
#include <evntprov.h>
#include <cstdint>
#include <cstdio>

namespace wsl::virtio_sound::native::windows::detail {

// Metadata only: no endpoint names, user identities, PCM, or signal amplitudes.
// Enabled only by an ETW consumer for this dedicated provider GUID.
inline void TraceAudioSource(const wchar_t* stage, std::uint64_t value = 0, std::uint64_t extra = 0) noexcept
{
    struct Registration final
    {
        REGHANDLE handle{};
        Registration() noexcept
        {
            constexpr GUID provider{0x4f761d1c, 0x00fd, 0x4a1a, {0x88, 0xa3, 0x6d, 0xb3, 0x85, 0xdf, 0x94, 0x13}};
            if (EventRegister(&provider, nullptr, nullptr, &handle) != ERROR_SUCCESS)
            {
                handle = 0;
            }
        }
        ~Registration()
        {
            if (handle)
            {
                EventUnregister(handle);
            }
        }
    };
    static Registration registration;
    if (!registration.handle)
    {
        return;
    }
    wchar_t text[256]{};
    if (swprintf_s(text, L"AudioSource stage=%ls value=%llu extra=%llu", stage, static_cast<unsigned long long>(value), static_cast<unsigned long long>(extra)) >
        0)
    {
        (void)EventWriteString(registration.handle, 4, 1, text);
    }
}
} // namespace wsl::virtio_sound::native::windows::detail
