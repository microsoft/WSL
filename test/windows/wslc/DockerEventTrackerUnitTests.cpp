// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/DockerEventTracker.h"

namespace DockerEventTracking {

using wsl::windows::service::wslc::DockerEventTracker;

constexpr auto c_execEvent =
    R"({"Type":"container","Action":"exec_die","Actor":{"ID":"container","Attributes":{"execID":"exec","exitCode":"0"}},"timeNano":1})";

class DockerEventTrackerUnitTests
{
    WSL_TEST_CLASS(DockerEventTrackerUnitTests)

    TEST_METHOD(ReferenceCanOutliveTrackerAndAddressReuse)
    {
        std::optional<DockerEventTracker> tracker{std::in_place, 0};
        auto captured = std::make_shared<int>(0);
        std::weak_ptr<int> released = captured;
        auto reference =
            tracker->RegisterExecStateUpdates("container", "exec", [captured = std::move(captured)](auto, auto, const auto&, auto) {});

        tracker.reset();
        tracker.emplace(0);
        int calls = 0;
        auto replacement = tracker->RegisterExecStateUpdates("container", "exec", [&](auto, auto, const auto&, auto) { ++calls; });

        reference.Reset();
        reference.Reset();
        VERIFY_IS_TRUE(released.expired());
        tracker->OnEvent(c_execEvent);
        VERIFY_ARE_EQUAL(1, calls);
    }

    TEST_METHOD(ResetDrainsRunningCallbackAndReleasesRegistration)
    {
        DockerEventTracker tracker{0};
        wil::unique_event entered{wil::EventOptions::ManualReset};
        wil::unique_event release{wil::EventOptions::ManualReset};
        wil::unique_event unregistered{wil::EventOptions::ManualReset};
        auto captured = std::make_shared<int>(0);
        std::weak_ptr<int> released = captured;
        int calls = 0;
        bool probing = false;
        bool stillRegistered = false;
        auto reference =
            tracker.RegisterExecStateUpdates("container", "exec", [&, captured = std::move(captured)](auto, auto, const auto&, auto) {
                if (probing)
                {
                    stillRegistered = true;
                    return;
                }

                ++calls;
                entered.SetEvent();
                probing = true;
                while (!release.wait(1))
                {
                    stillRegistered = false;
                    tracker.OnEvent(c_execEvent);
                    if (!stillRegistered)
                    {
                        // Recursive dispatch stops finding this entry once Reset removes it, before draining this invocation.
                        unregistered.SetEvent();
                        release.wait();
                        break;
                    }
                }
                probing = false;
            });

        std::thread dispatch([&]() { tracker.OnEvent(c_execEvent); });
        auto joinDispatch = wil::scope_exit([&]() {
            release.SetEvent();
            dispatch.join();
        });
        VERIFY_IS_TRUE(entered.wait(30000));

        std::promise<void> resetComplete;
        auto resetResult = resetComplete.get_future();
        std::thread reset([&]() {
            reference.Reset();
            resetComplete.set_value();
        });
        auto joinReset = wil::scope_exit([&]() {
            release.SetEvent();
            reset.join();
        });

        VERIFY_IS_TRUE(unregistered.wait(30000));
        VERIFY_IS_TRUE(resetResult.wait_for(100ms) == std::future_status::timeout);
        release.SetEvent();
        VERIFY_IS_TRUE(resetResult.wait_for(30s) == std::future_status::ready);
        resetResult.get();
        dispatch.join();
        joinDispatch.release();

        VERIFY_IS_TRUE(released.expired());
        tracker.OnEvent(c_execEvent);
        VERIFY_ARE_EQUAL(1, calls);
    }

    TEST_METHOD(MovesAndSelfUnregisterSkipSnapshottedCallbacks)
    {
        DockerEventTracker tracker{0};
        DockerEventTracker::EventTrackingReference first;
        DockerEventTracker::EventTrackingReference second;
        int calls = 0;
        first = tracker.RegisterExecStateUpdates("container", "exec", [&](auto, auto, const auto&, auto) {
            ++calls;
            first.Reset();
            second.Reset();
        });
        auto captured = std::make_shared<int>(0);
        std::weak_ptr<int> released = captured;
        auto source = tracker.RegisterExecStateUpdates(
            "container", "exec", [&, captured = std::move(captured)](auto, auto, const auto&, auto) { ++calls; });
        auto moved = std::move(source);
        source.Reset();
        auto replacedCapture = std::make_shared<int>(0);
        std::weak_ptr<int> replacedReleased = replacedCapture;
        second = tracker.RegisterVolumeUpdates([captured = std::move(replacedCapture)](const auto&, const auto&, const auto&, auto) {});
        second = std::move(moved);
        VERIFY_IS_TRUE(replacedReleased.expired());
        moved.Reset();
        second = std::move(second);
        VERIFY_IS_FALSE(released.expired());

        tracker.OnEvent(c_execEvent);
        VERIFY_ARE_EQUAL(1, calls);
        VERIFY_IS_TRUE(released.expired());
        tracker.OnEvent(c_execEvent);
        VERIFY_ARE_EQUAL(1, calls);
    }
};

} // namespace DockerEventTracking
