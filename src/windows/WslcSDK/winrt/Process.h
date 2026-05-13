/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Process.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Process class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Process.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Process : ProcessT<Process>
{
    Process() = default;

    void Start();
    uint32_t Pid();
    winrt::Microsoft::WSL::Containers::ProcessState State();
    int32_t ExitCode();
    void Signal(winrt::Microsoft::WSL::Containers::Signal const& signal);
    winrt::Windows::Storage::Streams::IInputStream GetOutputStream(winrt::Microsoft::WSL::Containers::ProcessOutputHandle const& outputHandle);
    winrt::Windows::Storage::Streams::IOutputStream GetInputStream();
    winrt::event_token OutputReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler);
    void OutputReceived(winrt::event_token const& token) noexcept;
    winrt::event_token ErrorReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler);
    void ErrorReceived(winrt::event_token const& token) noexcept;
    winrt::event_token Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler);
    void Exited(winrt::event_token const& token) noexcept;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
