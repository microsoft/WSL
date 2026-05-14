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
void Process::Start()
{
    throw hresult_not_implemented();
}
uint32_t Process::Pid()
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ProcessState Process::State()
{
    throw hresult_not_implemented();
}
int32_t Process::ExitCode()
{
    throw hresult_not_implemented();
}
void Process::Signal(winrt::Microsoft::WSL::Containers::Signal const& signal)
{
    throw hresult_not_implemented();
}
winrt::Windows::Storage::Streams::IInputStream Process::GetOutputStream(winrt::Microsoft::WSL::Containers::ProcessOutputHandle const& outputHandle)
{
    throw hresult_not_implemented();
}
winrt::Windows::Storage::Streams::IOutputStream Process::GetInputStream()
{
    throw hresult_not_implemented();
}
winrt::event_token Process::OutputReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler)
{
    throw hresult_not_implemented();
}
void Process::OutputReceived(winrt::event_token const& token) noexcept
{
    assert(false); // TODO: not implemented, but this can't throw
}
winrt::event_token Process::ErrorReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler)
{
    throw hresult_not_implemented();
}
void Process::ErrorReceived(winrt::event_token const& token) noexcept
{
    assert(false); // TODO: not implemented, but this can't throw
}
winrt::event_token Process::Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler)
{
    throw hresult_not_implemented();
}
void Process::Exited(winrt::event_token const& token) noexcept
{
    assert(false); // TODO: not implemented, but this can't throw
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
