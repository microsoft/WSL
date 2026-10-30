// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCRestartScheduler.h"

namespace WSLCRestartScheduling {

class WSLCRestartSchedulerUnitTests
{
    WSLC_TEST_CLASS(WSLCRestartSchedulerUnitTests)

    TEST_METHOD(RestartScheduler_StopDrainsCallbackReferences)
    {
        wil::unique_event callbackEntered{wil::EventOptions::ManualReset};
        wil::unique_event releaseCallback{wil::EventOptions::ManualReset};
        wil::unique_event referenceReleased{wil::EventOptions::ManualReset};
        wsl::windows::service::wslc::WSLCRestartScheduler scheduler;
        auto reference = std::shared_ptr<int>(new int{0}, [&](int* value) {
            delete value;
            referenceReleased.SetEvent();
        });

        scheduler.Schedule(0ms, [reference = std::move(reference), &callbackEntered, &releaseCallback]() {
            callbackEntered.SetEvent();
            releaseCallback.wait();
        });
        auto releaseOnFailure = wil::scope_exit([&]() { releaseCallback.SetEvent(); });
        VERIFY_IS_TRUE(callbackEntered.wait(30000));

        std::promise<bool> stopped;
        auto result = stopped.get_future();
        std::thread stopThread([&]() {
            scheduler.Stop();
            stopped.set_value(referenceReleased.is_signaled());
        });
        auto joinThread = wil::scope_exit([&]() {
            releaseCallback.SetEvent();
            stopThread.join();
        });

        VERIFY_IS_TRUE(result.wait_for(100ms) == std::future_status::timeout);
        releaseCallback.SetEvent();
        VERIFY_IS_TRUE(result.get());
    }
};

} // namespace WSLCRestartScheduling
