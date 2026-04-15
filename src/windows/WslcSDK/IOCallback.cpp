/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IOCallback.cpp

Abstract:

    Holds IO callback objects.

--*/
#include "precomp.h"
#include "WslcsdkPrivate.h"

IOCallback::IOCallback(IWSLCProcess* process, const WslcContainerProcessIOCallbackOptions& options) :
    m_process(process), m_callbackOptions(std::make_unique<WslcContainerProcessIOCallbackOptions>(options))
{
    using namespace wsl::windows::common::relay;

    auto addIOCallback = [&](WslcProcessIOHandle ioHandle, WslcStdIOCallback callback, PVOID context) {
        std::function<void(const gsl::span<char>& Buffer)> function;
        if (callback)
        {
            function = [ioHandle, callback, context](const gsl::span<char>& buffer) {
                callback(ioHandle, reinterpret_cast<const BYTE*>(buffer.data()), static_cast<uint32_t>(buffer.size()), context);
            };
        }
        else
        {
            function = [](const gsl::span<char>&) {};
        }

        m_io.AddHandle(std::make_unique<ReadHandle>(GetIOHandle(process, ioHandle), std::move(function)));
    };

    addIOCallback(WSLC_PROCESS_IO_HANDLE_STDOUT, options.onStdOut, options.callbackContext);
    addIOCallback(WSLC_PROCESS_IO_HANDLE_STDERR, options.onStdErr, options.callbackContext);

    if (options.onExit)
    {
        wil::unique_handle processExitEvent;
        THROW_IF_FAILED(process->GetExitEvent(&processExitEvent));
        m_io.AddHandle(std::make_unique<EventHandle>(std::move(processExitEvent)));
    }

    m_io.AddHandle(std::make_unique<EventHandle>(m_cancelEvent.get()), MultiHandleWait::CancelOnCompleted | MultiHandleWait::NeedNotComplete);

    m_thread = std::thread([this]() {
        try
        {
            // Will be false when cancelled.
            bool runResult = m_io.Run({});

            if (runResult && m_process && m_callbackOptions && m_callbackOptions->onExit)
            {
                WSLCProcessState state{};
                int exitCode = -1;

                // Prefer to make the callback even if we don't properly retrieve the exit code.
                if (FAILED_LOG(m_process->GetState(&state, &exitCode)))
                {
                    // Reset to our known value in case GetState stomped it while failing.
                    exitCode = -1;
                }
                else
                {
                    WI_ASSERT(state == WslcProcessStateExited);
                }

                // Regardless of our ability to get the proper exit code, inform the caller that the process
                // has exited and they will not be getting any additional IO callbacks.
                m_callbackOptions->onExit(exitCode, m_callbackOptions->callbackContext);
            }
        }
        CATCH_LOG();
    });
}

IOCallback::~IOCallback()
{
    Cancel();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void IOCallback::Cancel()
{
    m_cancelEvent.SetEvent();
}

bool IOCallback::HasIOCallback(const WslcContainerProcessOptionsInternal* options)
{
    return options && HasIOCallback(options->ioCallbacks);
}

bool IOCallback::HasIOCallback(const WslcContainerProcessIOCallbackOptions& options)
{
    return options.onStdOut || options.onStdErr || options.onExit;
}

wil::unique_handle IOCallback::GetIOHandle(IWSLCProcess* process, WslcProcessIOHandle ioHandle)
{
    wsl::windows::common::wslutil::COMOutputHandle handle;

    THROW_IF_FAILED(process->GetStdHandle(static_cast<WSLCFD>(static_cast<std::underlying_type_t<WslcProcessIOHandle>>(ioHandle)), &handle));

    return handle.Release();
}
