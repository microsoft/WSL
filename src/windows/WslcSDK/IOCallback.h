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
    // No callbacks are delivered until Activate() is called, so the owner can publish this object first.
    IOCallback(IWSLCCompatProcess* process, const WslcContainerProcessIOCallbackOptions& options);
    ~IOCallback();

    void Activate();
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
    wil::unique_event m_activateEvent{wil::EventOptions::ManualReset};
    std::once_flag m_join;
};
