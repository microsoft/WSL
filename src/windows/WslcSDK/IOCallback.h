/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IOCallback.h

Abstract:

    Holds IO callback objects.

--*/
#pragma once
#include "WSLCCompat.h"
#include "relay.hpp"
#include <thread>

struct WslcContainerProcessIOCallbackOptions;
struct WslcContainerProcessOptionsInternal;

struct IOCallback
{
    IOCallback(IWSLCCompatProcess* process, const WslcContainerProcessIOCallbackOptions& options);
    ~IOCallback();

    void Cancel();
    void Complete();

    bool IsOnIOCallbackThread() const noexcept;

    static bool HasIOCallback(const WslcContainerProcessOptionsInternal* options);
    static bool HasIOCallback(const WslcContainerProcessIOCallbackOptions& options);

    static wil::unique_handle GetIOHandle(IWSLCCompatProcess* process, WslcProcessIOHandle ioHandle);

private:
    wil::com_ptr<IWSLCCompatProcess> m_process;
    std::unique_ptr<WslcContainerProcessIOCallbackOptions> m_callbackOptions;
    std::thread m_thread;
    wsl::windows::common::io::MultiHandleWait m_io;
    wil::unique_event m_cancelEvent{wil::EventOptions::ManualReset};
    std::once_flag m_join;
};
