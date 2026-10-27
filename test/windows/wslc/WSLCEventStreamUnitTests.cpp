// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "EventStore.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

using namespace std::chrono_literals;

class EventStreamUnitTests
{
    WSLC_TEST_CLASS(EventStreamUnitTests)

    TEST_METHOD(QueuedReaderCancellationAbortsWithoutReleasingGate)
    {
        for (const bool cancelBeforeRead : {false, true})
        {
            EventStore store;
            wil::unique_event readerAvailable{wil::EventOptions::Signaled};
            auto stream =
                wil::MakeOrThrow<EventStream>(wil::unique_event{wsl::windows::common::wslutil::DuplicateHandle(readerAvailable.get())});
            VERIFY_SUCCEEDED(stream->RuntimeClassInitialize(nullptr, &store, 0, 1, {}));

            VERIFY_IS_TRUE(readerAvailable.wait(0));
            auto releaseGate = readerAvailable.SetEvent_scope_exit();
            wil::unique_event cancelEvent{wil::EventOptions::ManualReset};
            wil::unique_event readerStarted{wil::EventOptions::ManualReset};
            wil::unique_cotaskmem_ansistring eventJson;
            std::promise<HRESULT> result;
            auto future = result.get_future();
            if (cancelBeforeRead)
            {
                cancelEvent.SetEvent();
            }

            std::thread reader([&]() {
                readerStarted.SetEvent();
                result.set_value(stream->GetNext(cancelEvent.get(), &eventJson));
            });
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                cancelEvent.SetEvent();
                releaseGate.reset();
                FAIL_FAST_IF_MSG(
                    WaitForSingleObject(reader.native_handle(), 10 * 1000) != WAIT_OBJECT_0,
                    "event stream reader did not finish after gate release");
                reader.join();
            });

            VERIFY_IS_TRUE(readerStarted.wait(10 * 1000));
            if (!cancelBeforeRead)
            {
                VERIFY_IS_TRUE(future.wait_for(100ms) == std::future_status::timeout);
                cancelEvent.SetEvent();
            }

            const bool completed = future.wait_for(10s) == std::future_status::ready;
            VERIFY_IS_TRUE(completed);
            if (!completed)
            {
                return;
            }

            VERIFY_ARE_EQUAL(E_ABORT, future.get());
            VERIFY_IS_NULL(eventJson.get());
            VERIFY_IS_FALSE(readerAvailable.wait(0));
        }
    }

    TEST_METHOD(QueuedReaderWithoutCancellationWaitsForGate)
    {
        EventStore store;
        wil::unique_event readerAvailable{wil::EventOptions::Signaled};
        auto stream =
            wil::MakeOrThrow<EventStream>(wil::unique_event{wsl::windows::common::wslutil::DuplicateHandle(readerAvailable.get())});
        VERIFY_SUCCEEDED(stream->RuntimeClassInitialize(nullptr, &store, 0, 1, {}));

        VERIFY_IS_TRUE(readerAvailable.wait(0));
        auto releaseGate = readerAvailable.SetEvent_scope_exit();
        wil::unique_event readerStarted{wil::EventOptions::ManualReset};
        wil::unique_cotaskmem_ansistring eventJson;
        std::promise<HRESULT> result;
        auto future = result.get_future();
        std::thread reader([&]() {
            readerStarted.SetEvent();
            result.set_value(stream->GetNext(nullptr, &eventJson));
        });
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            releaseGate.reset();
            FAIL_FAST_IF_MSG(
                WaitForSingleObject(reader.native_handle(), 10 * 1000) != WAIT_OBJECT_0,
                "event stream reader did not finish after gate release");
            reader.join();
        });

        VERIFY_IS_TRUE(readerStarted.wait(10 * 1000));
        VERIFY_IS_TRUE(future.wait_for(100ms) == std::future_status::timeout);
        releaseGate.reset();

        const bool completed = future.wait_for(10s) == std::future_status::ready;
        VERIFY_IS_TRUE(completed);
        if (!completed)
        {
            return;
        }

        VERIFY_ARE_EQUAL(WSLC_E_EVENT_STREAM_FINISHED, future.get());
        VERIFY_IS_NULL(eventJson.get());
        VERIFY_IS_TRUE(readerAvailable.wait(0));
    }

    TEST_METHOD(AbortedReadReleasesGate)
    {
        EventStore store;
        wil::unique_event readerAvailable{wil::EventOptions::Signaled};
        auto stream =
            wil::MakeOrThrow<EventStream>(wil::unique_event{wsl::windows::common::wslutil::DuplicateHandle(readerAvailable.get())});
        VERIFY_SUCCEEDED(stream->RuntimeClassInitialize(nullptr, &store, 0, 1, {}));

        wil::unique_event cancelEvent{wil::EventOptions::ManualReset | wil::EventOptions::Signaled};
        wil::unique_cotaskmem_ansistring eventJson;
        VERIFY_ARE_EQUAL(E_ABORT, stream->GetNext(cancelEvent.get(), &eventJson));
        VERIFY_IS_NULL(eventJson.get());
        VERIFY_IS_TRUE(readerAvailable.wait(0));
    }
};

} // namespace wsl::windows::service::wslc
