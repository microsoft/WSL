/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IOCallback.h

Abstract:

    Holds IO callback objects.

--*/
#pragma once
#include "wslaservice.h"
#include "wslrelay.h"
#include <thread>

struct WslcContainerProcessIOCallbackOptions;
struct WslcContainerProcessOptionsInternal;

struct IOCallback
{
    IOCallback(IWSLAProcess* process, const WslcContainerProcessIOCallbackOptions& options);
    ~IOCallback();

    void Cancel();

    static bool HasIOCallback(const WslcContainerProcessOptionsInternal* options);
    static bool HasIOCallback(const WslcContainerProcessIOCallbackOptions& options);

    static wil::unique_handle GetIOHandle(IWSLAProcess* process, WslcProcessIOHandle ioHandle);

private:
    std::thread m_thread;
    wsl::windows::common::relay::MultiHandleWait m_io;
};
