// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "VirtioSoundDevice.hpp"

namespace wsl::virtio_sound::native::windows {

// Real capture source backed by an adjacent, user-session worker process.
// Construction owns a duplicated restricted token and starts supervision, but
// does not open a capture endpoint. Guest stream transitions only reset the
// bounded PCM ring and update desired state; the controller thread performs all
// process and pipe operations.
class WindowsCaptureSource final : public PcmSource
{
public:
    static std::shared_ptr<WindowsCaptureSource> Create(HANDLE restrictedUserToken, const std::wstring& workerPath, const std::wstring& endpointId);

    ~WindowsCaptureSource() override;

    WindowsCaptureSource(const WindowsCaptureSource&) = delete;
    WindowsCaptureSource& operator=(const WindowsCaptureSource&) = delete;

    bool StartCapture(std::uint32_t periodFrames) noexcept override;
    void StopCapture() noexcept override;
    // Ready means the current worker epoch reported Started. Buffered PCM is
    // tracked separately by AvailableFrames().
    SourceReadiness CaptureReadiness() const noexcept override;
    std::optional<std::uint32_t> AvailableFrames() const noexcept override;
    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept override;
    void Reset() noexcept override;

    // Gracefully stops the active epoch and worker, then enforces bounded
    // process cleanup if the worker or its endpoint does not respond.
    void Shutdown() noexcept;

private:
    class Impl;
    explicit WindowsCaptureSource(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace wsl::virtio_sound::native::windows
