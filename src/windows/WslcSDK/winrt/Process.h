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
    Process(winrt::Microsoft::WSL::Containers::ProcessSettings const& settings); // For the init process
    Process(winrt::Microsoft::WSL::Containers::Container const& container, winrt::Microsoft::WSL::Containers::ProcessSettings const& settings);

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

    WslcProcess ToHandle();
    ProcessOutputMode OutputMode();
    void AttachHandle(WslcProcess handle);

    static void final_release(std::unique_ptr<Process> self)
    {
        self->m_process.reset();
        if (self->m_waitForExitAction)
        {
            self->m_waitForExitAction.Cancel();
        }
    }

private:
    void EnsureStarted() const;
    void EnsureNotStarted() const;
    void EnsureCanStart() const;

    void ApplyCallbacksToSettings();
    void StartWaitingForExit();
    winrt::Windows::Foundation::IAsyncAction StartWaitingForExitAsync();

    static void CALLBACK OutputCallback(
        WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context) noexcept;
    static void CALLBACK ExitCallback(INT32 exitCode, _In_opt_ PVOID context) noexcept;

    // Only kept until Start() is called
    winrt::Microsoft::WSL::Containers::Container m_container{nullptr};
    winrt::Microsoft::WSL::Containers::ProcessSettings m_settings{nullptr};

    winrt::Microsoft::WSL::Containers::ProcessOutputMode m_outputMode{winrt::Microsoft::WSL::Containers::ProcessOutputMode::Discard};

    winrt::Windows::Foundation::IAsyncAction m_waitForExitAction{nullptr};

    // For output mode Event
    winrt::event<winrt::Microsoft::WSL::Containers::ProcessOutputHandler> m_outputReceivedEvent;
    winrt::event<winrt::Microsoft::WSL::Containers::ProcessOutputHandler> m_errorReceivedEvent;
    winrt::event<winrt::Microsoft::WSL::Containers::ProcessExitHandler> m_exitedEvent;

    // Releasing the process handle will disconnect the callbacks.
    // Keep this at the end so that it is released first, ensuring the events aren't destroyed while they may still be signaled.
    wil::unique_any<WslcProcess, decltype(&WslcReleaseProcess), &WslcReleaseProcess> m_process{nullptr};
};

} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(Process);
