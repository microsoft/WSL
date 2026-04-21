/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Process.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Process class.

--*/

#include "precomp.h"
#include "Process.h"
#include "Microsoft.WSL.Containers.Process.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
Process::Process(WslcProcess process) : m_process(process)
{
}

uint32_t Process::Pid()
{
    uint32_t pid;
    winrt::check_hresult(WslcGetProcessPid(ToHandle(), &pid));
    return pid;
}

winrt::Microsoft::WSL::Containers::ProcessState Process::State()
{
    WslcProcessState state;
    winrt::check_hresult(WslcGetProcessState(ToHandle(), &state));
    return static_cast<winrt::Microsoft::WSL::Containers::ProcessState>(state);
}

int32_t Process::ExitCode()
{
    int32_t exitCode;
    winrt::check_hresult(WslcGetProcessExitCode(ToHandle(), &exitCode));
    return exitCode;
}

void Process::Signal(winrt::Microsoft::WSL::Containers::Signal const& signal)
{
    winrt::check_hresult(WslcSignalProcess(ToHandle(), static_cast<WslcSignal>(signal)));
}

winrt::Windows::Storage::Streams::IInputStream Process::GetOutputStream(winrt::Microsoft::WSL::Containers::ProcessOutputHandle const& ioHandle)
{
    throw hresult_not_implemented();
}

winrt::Windows::Storage::Streams::IOutputStream Process::GetInputStream()
{
    throw hresult_not_implemented();
}

winrt::event_token Process::Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler)
{
    throw hresult_not_implemented();
}

void Process::Exited(winrt::event_token const& token) noexcept
{
}

WslcProcess Process::ToHandle()
{
    return m_process.get();
}

WslcProcess* Process::ToHandlePointer()
{
    return m_process.addressof();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
