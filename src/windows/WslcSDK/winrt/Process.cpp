/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Process.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Process class.

--*/

#include "precomp.h"
#include "Process.h"
#include "Streams.h"
#include "Microsoft.WSL.Containers.Process.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

Process::Process(winrt::Microsoft::WSL::Containers::Container const& container, winrt::Microsoft::WSL::Containers::ProcessSettings const& settings) :
    Process(settings)
{
    m_container = container;
}

Process::Process(winrt::Microsoft::WSL::Containers::ProcessSettings const& settings) : m_settings(settings)
{
    if (m_settings)
    {
        m_outputMode = GetImplementation(m_settings)->OutputMode();
        ApplyCallbacksToSettings();
    }
}

void Process::ApplyCallbacksToSettings()
{
    // Callbacks are only used with OutputMode::Event.
    // Stream and Discard modes use the exit event path (StartWaitingForExitAsync).
    if (m_outputMode != ProcessOutputMode::Event)
    {
        return;
    }

    auto settingsPtr = GetStructPointer(m_settings);

    WslcProcessCallbacks callbacks{};
    callbacks.onExit = ExitCallback;
    callbacks.onStdOut = OutputCallback;
    callbacks.onStdErr = OutputCallback;

    winrt::check_hresult(WslcSetProcessSettingsCallbacks(settingsPtr, &callbacks, this));
}

void Process::StartWaitingForExit()
{
    m_waitForExitAction = StartWaitingForExitAsync();
}

winrt::Windows::Foundation::IAsyncAction Process::StartWaitingForExitAsync()
{
    // Event mode uses the exit callback set in ApplyCallbacksToSettings; no need to wait here.
    if (m_outputMode == ProcessOutputMode::Event)
    {
        co_return;
    }

    wil::unique_handle exitEventHandle;
    winrt::check_hresult(WslcGetProcessExitEvent(ToHandle(), exitEventHandle.put()));

    // Allow the wait to be cancelled even if suspended for resume_on_signal.
    auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.enable_propagation();

    auto weak_this = get_weak();
    co_await winrt::resume_on_signal(exitEventHandle.get());

    try
    {
        if (auto strong_this = weak_this.get())
        {
            strong_this->m_exitedEvent(strong_this->ExitCode());
        }
    }
    CATCH_LOG();
}

void Process::AttachHandle(WslcProcess handle)
{
    if (m_process)
    {
        throw winrt::hresult_illegal_method_call(L"Process handle has already been attached");
    }

    m_process.reset(handle);
    StartWaitingForExit();
}

ProcessOutputMode Process::OutputMode()
{
    return m_outputMode;
}

void Process::Start()
{
    EnsureCanStart();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateContainerProcess(GetHandle(m_container), GetStructPointer(m_settings), m_process.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);

    m_container = nullptr;
    m_settings = nullptr;

    StartWaitingForExit();
}

void Process::EnsureStarted() const
{
    if (!m_process)
    {
        throw winrt::hresult_illegal_method_call(L"Process has not been started");
    }
}

void Process::EnsureNotStarted() const
{
    if (m_process)
    {
        throw winrt::hresult_illegal_method_call(L"Process has already been started");
    }
}

void Process::EnsureCanStart() const
{
    EnsureNotStarted();

    if (!m_container)
    {
        throw winrt::hresult_illegal_method_call(L"Start() cannot be called on the init process, it is started by the container");
    }

    auto cmdLine = GetImplementation(m_settings)->CommandLine();
    if (!cmdLine || cmdLine.Size() == 0)
    {
        throw winrt::hresult_invalid_argument(L"Process requires a non-empty CommandLine to start");
    }
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

winrt::Windows::Storage::Streams::IInputStream Process::GetOutputStream(winrt::Microsoft::WSL::Containers::ProcessOutputHandle const& outputHandle)
{
    if (m_outputMode != ProcessOutputMode::Stream)
    {
        throw winrt::hresult_illegal_method_call(L"GetOutputStream requires OutputMode::Stream");
    }

    wil::unique_handle handle;
    winrt::check_hresult(WslcGetProcessIOHandle(ToHandle(), static_cast<WslcProcessIOHandle>(outputHandle), handle.put()));
    return winrt::make<IOHandleInputStream>(std::move(handle));
}

winrt::Windows::Storage::Streams::IOutputStream Process::GetInputStream()
{
    wil::unique_handle handle;
    winrt::check_hresult(WslcGetProcessIOHandle(ToHandle(), WSLC_PROCESS_IO_HANDLE_STDIN, handle.put()));
    return winrt::make<IOHandleOutputStream>(std::move(handle));
}

winrt::event_token Process::OutputReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler)
{
    if (m_outputMode != ProcessOutputMode::Event)
    {
        throw winrt::hresult_illegal_method_call(L"OutputReceived requires OutputMode::Event");
    }

    return m_outputReceivedEvent.add(handler);
}

void Process::OutputReceived(winrt::event_token const& token) noexcept
{
    m_outputReceivedEvent.remove(token);
}

winrt::event_token Process::ErrorReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler)
{
    if (m_outputMode != ProcessOutputMode::Event)
    {
        throw winrt::hresult_illegal_method_call(L"ErrorReceived requires OutputMode::Event");
    }

    return m_errorReceivedEvent.add(handler);
}

void Process::ErrorReceived(winrt::event_token const& token) noexcept
{
    m_errorReceivedEvent.remove(token);
}

winrt::event_token Process::Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler)
{
    return m_exitedEvent.add(handler);
}

void Process::Exited(winrt::event_token const& token) noexcept
{
    m_exitedEvent.remove(token);
}

void CALLBACK Process::OutputCallback(WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context) noexcept
{
    try
    {
        auto process = static_cast<Process*>(context);

        auto& outputEvent = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? process->m_outputReceivedEvent : process->m_errorReceivedEvent;
        winrt::array_view<const uint8_t> buffer{data, dataBytes};
        outputEvent(buffer);
    }
    CATCH_LOG();
}

void CALLBACK Process::ExitCallback(INT32 exitCode, _In_opt_ PVOID context) noexcept
{
    try
    {
        auto process = static_cast<Process*>(context);
        process->m_exitedEvent(exitCode);
    }
    CATCH_LOG();
}

WslcProcess Process::ToHandle()
{
    EnsureStarted();
    return m_process.get();
}

void Process::Close()
{
    if (m_waitForExitAction)
    {
        m_waitForExitAction.Cancel();
        m_waitForExitAction = nullptr;
    }

    // Methods called after Close() will fail due to EnsureStarted().
    m_process.reset();
}

void Process::final_release(std::unique_ptr<Process> self)
{
    // Ensure cleanup when refcount drops to zero even if Close() was not called explicitly.
    self->Close();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
