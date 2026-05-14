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

Process::Process(WslcProcess process) : m_process(process)
{
    StartExitThread();
}

Process::Process(winrt::Microsoft::WSL::Containers::Container const& container, winrt::Microsoft::WSL::Containers::ProcessSettings const& settings) :
    m_container(container), m_settings(settings)
{
}

bool Process::ApplyCallbacksToSettings(WslcProcessSettings* settings)
{
    if (!m_outputReceivedEvent && !m_errorReceivedEvent)
    {
        return false;
    }

    WslcProcessCallbacks callbacks{};
    callbacks.onExit = ExitCallback;

    if (m_outputReceivedEvent)
    {
        callbacks.onStdOut = OutputCallback;
    }

    if (m_errorReceivedEvent)
    {
        callbacks.onStdErr = OutputCallback;
    }

    winrt::check_hresult(WslcSetProcessSettingsCallbacks(settings, &callbacks, this));

    // This object needs to stay alive for as long as the callbacks may be invoked even if all other references to it are dropped.
    // We increase its ref count here, and decrease it once the process exits.
    AddRef();
    m_hasExitCallback = true;

    return true;
}

void Process::AttachHandle(WslcProcess handle)
{
    if (m_process)
    {
        throw winrt::hresult_illegal_method_call(L"Process handle has already been attached");
    }

    m_process.reset(handle);

    // If no exit callback is registered (callback path), start the exit thread
    // so the Exited WinRT event still works.
    if (!m_hasExitCallback)
    {
        StartExitThread();
    }
}

void Process::StartExitThread()
{
    winrt::check_hresult(WslcGetProcessExitEvent(m_process.get(), m_exitEventHandle.put()));

    // Start a background thread that waits for exit and fires the WinRT event.
    m_destructedEvent.create(wil::EventOptions::ManualReset);
    m_exitThread = std::thread(
        [weak_this = get_weak(), exitEventHandle = m_exitEventHandle.get(), destructedEventHandle = m_destructedEvent.get()]() {
            try
            {
                HANDLE handles[] = {exitEventHandle, destructedEventHandle};
                auto waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (waitResult != WAIT_OBJECT_0)
                {
                    return;
                }

                // After this point, we ensure that the Process object is not destructed until we're done.
                if (auto strong_this = weak_this.get())
                {
                    int32_t exitCode = 0;
                    if (FAILED(WslcGetProcessExitCode(strong_this->ToHandle(), &exitCode)))
                    {
                        return;
                    }

                    strong_this->m_exitedEvent(*strong_this, exitCode);
                }
            }
            CATCH_LOG()
        });
}

void Process::Start()
{
    EnsureNotStarted();

    if (ApplyCallbacksToSettings(GetStructPointer(m_settings)))
    {
        auto releaseRef = wil::scope_exit([this] { Release(); });

        wil::unique_cotaskmem_string errorMessage;
        auto hr = WslcCreateContainerProcess(GetHandle(m_container), GetStructPointer(m_settings), m_process.put(), errorMessage.put());
        THROW_MSG_IF_FAILED(hr, errorMessage);

        releaseRef.release();
    }
    else
    {
        wil::unique_cotaskmem_string errorMessage;
        auto hr = WslcCreateContainerProcess(GetHandle(m_container), GetStructPointer(m_settings), m_process.put(), errorMessage.put());
        THROW_MSG_IF_FAILED(hr, errorMessage);
    }
    m_settings = nullptr;
}

Process::~Process()
{
    if (m_destructedEvent)
    {
        m_destructedEvent.SetEvent();
    }

    if (m_exitThread.joinable())
    {
        m_exitThread.join();
    }
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
    if ((outputHandle == ProcessOutputHandle::StandardOutput && m_outputReceivedEvent) ||
        (outputHandle == ProcessOutputHandle::StandardError && m_errorReceivedEvent))
    {
        // Using callbacks and using streams for output are mutually exclusive.
        throw winrt::hresult_illegal_method_call(L"Cannot get output stream when using output callbacks");
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
    // Callbacks can only be registered before the process is started.
    if (!m_outputReceivedEvent)
    {
        EnsureNotStarted();
        m_outputReceivedEvent.emplace();
    }

    return m_outputReceivedEvent->add(handler);
}

void Process::OutputReceived(winrt::event_token const& token) noexcept
{
    if (m_outputReceivedEvent)
    {
        m_outputReceivedEvent->remove(token);
    }
}

winrt::event_token Process::ErrorReceived(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& handler)
{
    // Callbacks can only be registered before the process is started.
    if (!m_errorReceivedEvent)
    {
        EnsureNotStarted();
        m_errorReceivedEvent.emplace();
    }

    return m_errorReceivedEvent->add(handler);
}

void Process::ErrorReceived(winrt::event_token const& token) noexcept
{
    if (m_errorReceivedEvent)
    {
        m_errorReceivedEvent->remove(token);
    }
}

winrt::event_token Process::Exited(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& handler)
{
    return m_exitedEvent.add(handler);
}

void Process::Exited(winrt::event_token const& token) noexcept
{
    m_exitedEvent.remove(token);
}

void CALLBACK Process::OutputCallback(WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context)
{
    auto* self = static_cast<Process*>(context);
    if (!self->HasExternalReferences())
    {
        return;
    }

    winrt::com_ptr<Process> process;
    process.copy_from(self);

    auto& outputEvent = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? process->m_outputReceivedEvent : process->m_errorReceivedEvent;
    if (outputEvent)
    {
        winrt::array_view<const uint8_t> buffer{data, dataBytes};
        (*outputEvent)(*process, buffer);
    }
}

void CALLBACK Process::ExitCallback(INT32 exitCode, _In_opt_ PVOID context)
{
    winrt::com_ptr<Process> process;

    // No other callback should be called after this event, so we no longer need to keep the object alive.
    // This takes ownership without increasing the ref count to account for the AddRef in ApplyCallbacksToSettings().
    process.attach(static_cast<Process*>(context));

    process->m_exitedEvent(*process, exitCode);
}

bool Process::HasExternalReferences()
{
    // Check whether any references exist beyond the one held by the callback mechanism.
    AddRef();
    auto count = Release();
    return count > 1;
}

WslcProcess Process::ToHandle()
{
    EnsureStarted();
    return m_process.get();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
