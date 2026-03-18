/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IOCallback.cpp

Abstract:

    Holds IO callback objects.

--*/
#include "precomp.h"
#include "WslcsdkPrivate.h"

IOCallback::IOCallback(IWSLAProcess* process, const WslcContainerProcessIOCallbackOptions& options)
{
    using namespace wsl::windows::common::relay;

    auto addIOCallback = [&](WslcProcessIOHandle ioHandle, WslcStdIOCallback callback, PVOID context)
    {
        std::function<void(const gsl::span<char>& Buffer)> function;
        if (callback)
        {
            function = [callback, context](const gsl::span<char>& buffer) {
                callback(reinterpret_cast<const BYTE*>(buffer.data()), static_cast<uint32_t>(buffer.size()), context);
                };
        }
        else
        {
            function = [](const gsl::span<char>&) {};
        }

        m_io.AddHandle(std::make_unique<ReadHandle>(GetIOHandle(process, ioHandle), std::move(function)));
    };

    addIOCallback(WSLC_PROCESS_IO_HANDLE_STDOUT, options.stdOutCallback, options.stdOutCallbackContext);
    addIOCallback(WSLC_PROCESS_IO_HANDLE_STDERR, options.stdErrCallback, options.stdErrCallbackContext);

    m_io.AddHandle(std::make_unique<EventHandle>(m_cancelEvent.get()), MultiHandleWait::CancelOnCompleted);

    m_thread = std::thread([this]() {
        try
        {
            m_io.Run({});
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
    return options.stdOutCallback || options.stdErrCallback;
}

wil::unique_handle IOCallback::GetIOHandle(IWSLAProcess* process, WslcProcessIOHandle ioHandle)
{
    ULONG ulongHandle = 0;

    THROW_IF_FAILED(process->GetStdHandle(static_cast<ULONG>(static_cast<std::underlying_type_t<WslcProcessIOHandle>>(ioHandle)), &ulongHandle));

    return wil::unique_handle{ULongToHandle(ulongHandle)};
}
