/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IOCallback.h

Abstract:

    Holds IO callback objects.

--*/
#pragma once
#include "wslc.h"
#include "relay.hpp"
#include <thread>

struct WslcContainerProcessIOCallbackOptions;
struct WslcContainerProcessOptionsInternal;

struct IOCallback
{
    IOCallback(IWSLCProcess* process, const WslcContainerProcessIOCallbackOptions& options);
    ~IOCallback();

    void Cancel();

    static bool HasIOCallback(const WslcContainerProcessOptionsInternal* options);
    static bool HasIOCallback(const WslcContainerProcessIOCallbackOptions& options);

    static wil::unique_handle GetIOHandle(IWSLCProcess* process, WslcProcessIOHandle ioHandle);

private:
    wil::com_ptr<IWSLCProcess> m_process;
    std::unique_ptr<WslcContainerProcessIOCallbackOptions> m_callbackOptions;
    std::thread m_thread;
    wsl::windows::common::relay::MultiHandleWait m_io;
    wil::unique_event m_cancelEvent{wil::EventOptions::ManualReset};
};
