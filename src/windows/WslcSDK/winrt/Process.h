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
    Process(WslcProcess process, winrt::Microsoft::WSL::Containers::ProcessOutputMode mode = winrt::Microsoft::WSL::Containers::ProcessOutputMode::Discard);
    Process(winrt::Microsoft::WSL::Containers::Container const& container, winrt::Microsoft::WSL::Containers::ProcessSettings const& settings);
    Process(winrt::Microsoft::WSL::Containers::ProcessOutputMode mode);
    ~Process();

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
    bool SetupCallbacksForStart(WslcProcessSettings* settings);
    void ActivateCallbackOwnership();
    bool ApplyCallbacksToSettings(WslcProcessSettings* settings);
    void AttachHandle(WslcProcess handle);

private:
    void EnsureStarted() const;
    void EnsureNotStarted() const;
    void StartExitThread();

    static void CALLBACK OutputCallback(
        WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context) noexcept;
    static void CALLBACK ExitCallback(INT32 exitCode, _In_opt_ PVOID context) noexcept;

    // Returns true if external references still exist (i.e. not only the callback AddRef).
    bool HasExternalReferences();

    // Only kept until Start() is called
    winrt::Microsoft::WSL::Containers::Container m_container{nullptr};
    winrt::Microsoft::WSL::Containers::ProcessSettings m_settings{nullptr};

    wil::unique_any<WslcProcess, decltype(&WslcReleaseProcess), &WslcReleaseProcess> m_process{nullptr};
    winrt::event<winrt::Microsoft::WSL::Containers::ProcessExitHandler> m_exitedEvent;
    winrt::Microsoft::WSL::Containers::ProcessOutputMode m_outputMode{winrt::Microsoft::WSL::Containers::ProcessOutputMode::Discard};

    // For processes created with Start() (callback path):
    std::optional<winrt::event<winrt::Microsoft::WSL::Containers::ProcessOutputHandler>> m_outputReceivedEvent{};
    std::optional<winrt::event<winrt::Microsoft::WSL::Containers::ProcessOutputHandler>> m_errorReceivedEvent{};
    bool m_hasExitCallback{false};

    // For processes created from a WslcProcess handle (exit thread path):
    wil::unique_handle m_exitEventHandle;
    wil::unique_event m_destructedEvent;
    std::thread m_exitThread;
};

} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(Process);
