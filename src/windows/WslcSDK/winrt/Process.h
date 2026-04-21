/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Process.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Process class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Process.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Process : ProcessT<Process>
{
    Process() = default;
    Process(WslcProcess process);

    uint32_t Pid();
    winrt::Microsoft::WSL::Containers::ProcessState State();
    int32_t ExitCode();
    void Signal(winrt::Microsoft::WSL::Containers::Signal const& signal);
    winrt::Windows::Storage::Streams::IInputStream GetOutputStream(winrt::Microsoft::WSL::Containers::ProcessOutputHandle const& ioHandle);
    winrt::Windows::Storage::Streams::IOutputStream GetInputStream();
    winrt::event_token Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler);
    void Exited(winrt::event_token const& token) noexcept;

    WslcProcess ToHandle();
    WslcProcess* ToHandlePointer();

private:
    wil::unique_any<WslcProcess, decltype(&WslcReleaseProcess), &WslcReleaseProcess> m_process;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(Process);
