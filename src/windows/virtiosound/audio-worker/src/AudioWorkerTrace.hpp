// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <Windows.h>
#include <evntprov.h>

#include <cstdint>
#include <cstdio>

namespace wsl::virtio_sound::audio_worker::detail {

// Metadata only. Keep labels fixed or operation constants; never pass endpoint,
// user, PCM, or signal values to this trace.
inline void TraceAudioWorker(const wchar_t* stage, std::uint64_t value = 0, std::uint64_t extra = 0) noexcept
{
    struct Registration final
    {
        REGHANDLE m_handle{};
        Registration() noexcept
        {
            constexpr GUID c_provider{0x4f761d1c, 0x00fd, 0x4a1a, {0x88, 0xa3, 0x6d, 0xb3, 0x85, 0xdf, 0x94, 0x13}};
            if (EventRegister(&c_provider, nullptr, nullptr, &m_handle) != ERROR_SUCCESS)
            {
                m_handle = 0;
            }
        }
        ~Registration()
        {
            if (m_handle)
            {
                EventUnregister(m_handle);
            }
        }
    };

    static Registration registration;
    if (!registration.m_handle)
    {
        return;
    }

    wchar_t text[256]{};
    if (swprintf_s(text, L"AudioWorker stage=%ls value=%llu extra=%llu", stage, static_cast<unsigned long long>(value), static_cast<unsigned long long>(extra)) >
        0)
    {
        (void)EventWriteString(registration.m_handle, 4, 1, text);
    }
}

} // namespace wsl::virtio_sound::audio_worker::detail
