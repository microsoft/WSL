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
    if (options.stdOutCallback)
    {
        auto localCallback = options.stdOutCallback;
        auto localContext = options.stdOutCallbackContext;
        m_io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
            GetIOHandle(process, WSLC_PROCESS_IO_HANDLE_STDOUT),
            [localCallback, localContext](const auto& buffer) { localCallback(reinterpret_cast<const BYTE*>(buffer.data()), static_cast<uint32_t>(buffer.size()), localContext); }));
    }

    if (options.stdErrCallback)
    {
        auto localCallback = options.stdErrCallback;
        auto localContext = options.stdErrCallbackContext;
        m_io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
            GetIOHandle(process, WSLC_PROCESS_IO_HANDLE_STDERR), [localCallback, localContext](const auto& buffer) {
                localCallback(reinterpret_cast<const BYTE*>(buffer.data()), static_cast<uint32_t>(buffer.size()), localContext);
            }));
    }

    m_thread = std::thread([this]() {
        try
        {
            m_io.Run({});
        }
        catch (...){}
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
    m_io.Cancel();
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

    THROW_IF_FAILED(process->GetStdHandle(
        static_cast<ULONG>(static_cast<std::underlying_type_t<WslcProcessIOHandle>>(ioHandle)), &ulongHandle));

    return wil::unique_handle{ULongToHandle(ulongHandle)};
}
