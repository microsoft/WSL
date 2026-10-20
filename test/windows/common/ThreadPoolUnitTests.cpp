// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "AsyncExecution.h"
#include "ThreadPool.h"

using namespace wsl::windows::common;

namespace ThreadPoolUnitTests {
namespace {

    // Upper bound for waits that complete immediately unless the code under test is broken.
    constexpr DWORD c_failTimeoutMs = 60 * 1000;

    HRESULT ErrorCode(const std::exception_ptr& error)
    {
        try
        {
            std::rethrow_exception(error);
        }
        catch (...)
        {
            return wil::ResultFromCaughtException();
        }
    }

} // namespace

class ThreadPoolUnitTests
{
    WSL_TEST_CLASS(ThreadPoolUnitTests)

    TEST_METHOD(ThreadPool_InvalidArgumentsThrow)
    {
        VERIFY_THROWS_SPECIFIC((ThreadPool{0, 0}), wil::ResultException, [](const wil::ResultException& ex) {
            return ex.GetErrorCode() == E_INVALIDARG;
        });

        VERIFY_THROWS_SPECIFIC((ThreadPool{2, 1}), wil::ResultException, [](const wil::ResultException& ex) {
            return ex.GetErrorCode() == E_INVALIDARG;
        });
    }

    TEST_METHOD(ThreadPool_SubmitWorkReturnsResult)
    {
        ThreadPool pool{1, 2};

        auto item = pool.SubmitWork([]() { return std::wstring{L"result"}; });

        VERIFY_IS_TRUE(item->Wait(c_failTimeoutMs));
        VERIFY_IS_NULL(item->Error());
        VERIFY_ARE_EQUAL(std::wstring{L"result"}, item->Result());
    }

    TEST_METHOD(ThreadPool_SubmitWorkRunsVoidWork)
    {
        ThreadPool pool{1, 1};
        std::atomic<bool> ran{false};

        auto item = pool.SubmitWork([&ran]() { ran = true; });

        VERIFY_IS_TRUE(item->Wait(c_failTimeoutMs));
        VERIFY_IS_NULL(item->Error());
        item->Result();
        VERIFY_IS_TRUE(ran.load());
    }

    TEST_METHOD(ThreadPool_SubmitWorkAcceptsMoveOnlyCallableAndResult)
    {
        ThreadPool pool{1, 1};

        auto item = pool.SubmitWork([value = std::make_unique<int>(42)]() mutable { return std::move(value); });

        auto result = std::move(item->Result());
        VERIFY_IS_NOT_NULL(result.get());
        VERIFY_ARE_EQUAL(42, *result);
    }

    TEST_METHOD(ThreadPool_ErrorPreservesExceptionType)
    {
        ThreadPool pool{1, 1};

        auto item = pool.SubmitWork([]() -> int { throw std::runtime_error("work failed"); });

        VERIFY_IS_NOT_NULL(item->Error());
        VERIFY_THROWS_SPECIFIC(std::rethrow_exception(item->Error()), std::runtime_error, [](const std::runtime_error& ex) {
            return std::string{ex.what()} == "work failed";
        });
        VERIFY_THROWS_SPECIFIC(item->Result(), std::runtime_error, [](const std::runtime_error& ex) {
            return std::string{ex.what()} == "work failed";
        });
    }

    TEST_METHOD(ThreadPool_CancelPendingWork)
    {
        ThreadPool pool{1, 1};
        wil::unique_event started{wil::EventOptions::ManualReset};
        wil::unique_event release{wil::EventOptions::ManualReset};
        std::atomic<bool> pendingRan{false};

        auto blocking = pool.SubmitWork([&]() {
            started.SetEvent();
            return release.wait(c_failTimeoutMs);
        });

        VERIFY_IS_TRUE(started.wait(c_failTimeoutMs));

        // The pool has one thread, which is occupied, so this work cannot start until release is set.
        auto pending = pool.SubmitWork([&pendingRan]() { pendingRan = true; });

        VERIFY_IS_TRUE(pending->Cancel());
        VERIFY_IS_TRUE(pending->Wait(0));
        VERIFY_IS_FALSE(pending->Cancel());

        release.SetEvent();
        VERIFY_IS_TRUE(blocking->Result());

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), ErrorCode(pending->Error()));
        VERIFY_THROWS_SPECIFIC(pending->Result(), wil::ResultException, [](const wil::ResultException& ex) {
            return ex.GetErrorCode() == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        });

        // Run a follow-up item on the single thread to make sure the canceled item's callback has been consumed.
        VERIFY_IS_TRUE(pool.SubmitWork([]() { return true; })->Result());
        VERIFY_IS_FALSE(pendingRan.load());
    }

    TEST_METHOD(ThreadPool_CancelRunningOrCompletedWorkFails)
    {
        ThreadPool pool{1, 1};
        wil::unique_event started{wil::EventOptions::ManualReset};
        wil::unique_event release{wil::EventOptions::ManualReset};

        auto item = pool.SubmitWork([&]() {
            started.SetEvent();
            return release.wait(c_failTimeoutMs);
        });

        VERIFY_IS_TRUE(started.wait(c_failTimeoutMs));
        VERIFY_IS_FALSE(item->Cancel());

        release.SetEvent();
        VERIFY_IS_TRUE(item->Result());
        VERIFY_IS_FALSE(item->Cancel());
        VERIFY_IS_NULL(item->Error());
    }

    TEST_METHOD(ThreadPool_DestructorCancelsPendingAndWaitsForRunningWork)
    {
        auto pool = std::make_unique<ThreadPool>(1, 1);
        wil::unique_event started{wil::EventOptions::ManualReset};
        wil::unique_event pendingSubmitted{wil::EventOptions::ManualReset};
        std::shared_ptr<WorkItem<void>> pending;
        std::atomic<bool> pendingRan{false};
        std::atomic<bool> runningFinished{false};

        // The running item returns only after the destructor has canceled the pending item, so the
        // destructor must wait for running work and must not start pending work.
        auto running = pool->SubmitWork([&]() {
            started.SetEvent();
            const bool observedCancel = pendingSubmitted.wait(c_failTimeoutMs) && pending->Wait(c_failTimeoutMs);
            runningFinished = true;
            return observedCancel;
        });

        VERIFY_IS_TRUE(started.wait(c_failTimeoutMs));
        pending = pool->SubmitWork([&pendingRan]() { pendingRan = true; });
        pendingSubmitted.SetEvent();

        pool.reset();

        VERIFY_IS_TRUE(runningFinished.load());
        VERIFY_IS_TRUE(running->Wait(0));
        VERIFY_IS_TRUE(running->Result());
        VERIFY_IS_TRUE(pending->Wait(0));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), ErrorCode(pending->Error()));
        VERIFY_IS_FALSE(pendingRan.load());
    }

    TEST_METHOD(ThreadPool_RunsUpToMaxThreadsConcurrently)
    {
        constexpr DWORD c_maxThreads = 4;
        constexpr int c_itemCount = 12;

        ThreadPool pool{c_maxThreads, c_maxThreads};
        wil::unique_event limitReached{wil::EventOptions::ManualReset};
        std::atomic<DWORD> active{0};
        std::atomic<DWORD> peak{0};

        std::vector<std::shared_ptr<WorkItem<bool>>> items;
        for (int i = 0; i < c_itemCount; ++i)
        {
            items.push_back(pool.SubmitWork([&]() {
                const auto current = ++active;
                auto previousPeak = peak.load();
                while (current > previousPeak && !peak.compare_exchange_weak(previousPeak, current))
                {
                }

                if (current == c_maxThreads)
                {
                    limitReached.SetEvent();
                }

                const bool reached = limitReached.wait(c_failTimeoutMs);
                --active;
                return reached;
            }));
        }

        for (const auto& item : items)
        {
            VERIFY_IS_TRUE(item->Result());
        }

        VERIFY_ARE_EQUAL(c_maxThreads, peak.load());
    }
};

class ForEachAsyncUnitTests
{
    WSL_TEST_CLASS(ForEachAsyncUnitTests)

    TEST_METHOD(ForEachAsync_SuccessCallbackReceivesResultsInInputOrder)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [](const int& item) { return item * 2; },
            [&](int result) { results.push_back(result); },
            [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            3);

        const std::vector<int> expected = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
        VERIFY_IS_TRUE(expected == results);
    }

    TEST_METHOD(ForEachAsync_ErrorCallbackReceivesErrorsInInputOrder)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5};
        std::vector<int> succeededItems;
        std::vector<std::pair<int, HRESULT>> failedItems;

        ForEachAsync<int>(
            items,
            [](const int& item) -> int {
                if (item % 2 == 0)
                {
                    THROW_HR(item == 2 ? E_ACCESSDENIED : E_NOTIMPL);
                }

                return item;
            },
            [&](int result) { succeededItems.push_back(result); },
            [&](const int& item, const std::exception_ptr& error) { failedItems.emplace_back(item, ErrorCode(error)); });

        const std::vector<int> expectedSucceeded = {1, 3, 5};
        const std::vector<std::pair<int, HRESULT>> expectedFailed = {{2, E_ACCESSDENIED}, {4, E_NOTIMPL}};
        VERIFY_IS_TRUE(expectedSucceeded == succeededItems);
        VERIFY_IS_TRUE(expectedFailed == failedItems);
    }

    TEST_METHOD(ForEachAsync_ErrorPreservesExceptionType)
    {
        const std::vector<int> items = {1};
        int errorCount = 0;

        ForEachAsync<int>(
            items,
            [](const int& /*item*/) -> int { throw std::runtime_error("work failed"); },
            [](int /*result*/) { VERIFY_FAIL(L"Unexpected success callback"); },
            [&](const int& /*item*/, const std::exception_ptr& error) {
                ++errorCount;
                VERIFY_THROWS_SPECIFIC(std::rethrow_exception(error), std::runtime_error, [](const std::runtime_error& ex) {
                    return std::string{ex.what()} == "work failed";
                });
            });

        VERIFY_ARE_EQUAL(1, errorCount);
    }

    TEST_METHOD(ForEachAsync_EmptyInputProducesNoCallbacks)
    {
        const std::vector<int> items;
        bool workCalled = false;
        bool successCalled = false;
        bool errorCalled = false;

        ForEachAsync<int>(
            items,
            [&](const int& item) {
                workCalled = true;
                return item;
            },
            [&](int /*result*/) { successCalled = true; },
            [&](const int& /*item*/, const std::exception_ptr& /*error*/) { errorCalled = true; });

        VERIFY_IS_FALSE(workCalled);
        VERIFY_IS_FALSE(successCalled);
        VERIFY_IS_FALSE(errorCalled);
    }

    TEST_METHOD(ForEachAsync_CallbacksRunOnCallingThread)
    {
        const std::vector<int> items = {1, 2, 3, 4};
        const auto callingThread = GetCurrentThreadId();
        std::vector<DWORD> callbackThreads;

        ForEachAsync<int>(
            items,
            [](const int& item) -> int {
                if (item > 2)
                {
                    THROW_HR(E_FAIL);
                }

                return item;
            },
            [&](int /*result*/) { callbackThreads.push_back(GetCurrentThreadId()); },
            [&](const int& /*item*/, const std::exception_ptr& /*error*/) { callbackThreads.push_back(GetCurrentThreadId()); });

        VERIFY_ARE_EQUAL(items.size(), callbackThreads.size());
        for (const auto threadId : callbackThreads)
        {
            VERIFY_ARE_EQUAL(callingThread, threadId);
        }
    }

    TEST_METHOD(ForEachAsync_MoveOnlyResultIsDelivered)
    {
        const std::vector<int> items = {7};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [](const int& item) { return std::make_unique<int>(item); },
            [&](std::unique_ptr<int> result) { results.push_back(*result); },
            [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); });

        VERIFY_ARE_EQUAL(1u, results.size());
        VERIFY_ARE_EQUAL(7, results[0]);
    }

    TEST_METHOD(ForEachAsync_BoolItemsAreReadWhenWorkRuns)
    {
        const std::vector<bool> items = {true, false, false, true, true, false, true, false};
        std::vector<bool> results;

        ForEachAsync<bool>(
            items,
            [](const bool& item) {
                // Delays the work so that queued items typically run after submission has finished
                // iterating the input vector. The result must match the input regardless of timing.
                Sleep(10);
                return item;
            },
            [&](bool result) { results.push_back(result); },
            [](const bool& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            1);

        VERIFY_IS_TRUE(items == results);
    }

    TEST_METHOD(ForEachAsync_ConcurrencyOfOneRunsSerially)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5};
        std::atomic<int> active{0};
        std::atomic<int> peak{0};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [&](const int& item) {
                const auto current = ++active;
                auto previousPeak = peak.load();
                while (current > previousPeak && !peak.compare_exchange_weak(previousPeak, current))
                {
                }

                --active;
                return item;
            },
            [&](int result) { results.push_back(result); },
            [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            1);

        VERIFY_IS_TRUE(items == results);
        VERIFY_ARE_EQUAL(1, peak.load());
    }

    TEST_METHOD(ForEachAsync_ConcurrencyReachesButDoesNotExceedLimit)
    {
        constexpr DWORD c_maxConcurrency = 3;
        const std::vector<int> items = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        wil::unique_event limitReached{wil::EventOptions::ManualReset};
        std::atomic<DWORD> active{0};
        std::atomic<DWORD> peak{0};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [&](const int& item) -> int {
                const auto current = ++active;
                auto previousPeak = peak.load();
                while (current > previousPeak && !peak.compare_exchange_weak(previousPeak, current))
                {
                }

                if (current == c_maxConcurrency)
                {
                    limitReached.SetEvent();
                }

                const bool reached = limitReached.wait(c_failTimeoutMs);
                --active;
                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !reached);
                return item;
            },
            [&](int result) { results.push_back(result); },
            [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            c_maxConcurrency);

        VERIFY_IS_TRUE(items == results);
        VERIFY_ARE_EQUAL(c_maxConcurrency, peak.load());
    }

    TEST_METHOD(ForEachAsync_ConcurrencyLargerThanItemCountIsHandled)
    {
        const std::vector<int> items = {1, 2};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [](const int& item) { return item; },
            [&](int result) { results.push_back(result); },
            [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            64);

        VERIFY_IS_TRUE(items == results);
    }

    TEST_METHOD(ForEachAsync_ZeroConcurrencyThrows)
    {
        const std::vector<int> items = {1};
        bool workCalled = false;

        VERIFY_THROWS_SPECIFIC(
            ForEachAsync<int>(
                items,
                [&](const int& item) {
                    workCalled = true;
                    return item;
                },
                [](int /*result*/) {},
                [](const int& /*item*/, const std::exception_ptr& /*error*/) {},
                0),
            wil::ResultException,
            [](const wil::ResultException& ex) { return ex.GetErrorCode() == E_INVALIDARG; });

        VERIFY_IS_FALSE(workCalled);
    }

    TEST_METHOD(ForEachAsync_ThrowFromOnErrorPropagatesAfterWorkStops)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5, 6, 7, 8};
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        int errorCount = 0;

        VERIFY_THROWS_SPECIFIC(
            ForEachAsync<int>(
                items,
                [&](const int& item) -> int {
                    ++started;
                    auto markFinished = wil::scope_exit([&]() { ++finished; });
                    THROW_HR_IF(E_ACCESSDENIED, item == 1);
                    return item;
                },
                [](int /*result*/) { VERIFY_FAIL(L"Unexpected success callback"); },
                [&](const int& /*item*/, const std::exception_ptr& error) {
                    ++errorCount;
                    std::rethrow_exception(error);
                },
                2),
            wil::ResultException,
            [](const wil::ResultException& ex) { return ex.GetErrorCode() == E_ACCESSDENIED; });

        VERIFY_ARE_EQUAL(1, errorCount);
        VERIFY_ARE_EQUAL(started.load(), finished.load());
    }

    TEST_METHOD(ForEachAsync_ThrowFromOnSuccessPropagatesAfterWorkStops)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5, 6, 7, 8};
        std::atomic<bool> returned{false};
        std::atomic<bool> ranAfterReturn{false};
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        int successCount = 0;

        VERIFY_THROWS_SPECIFIC(
            ForEachAsync<int>(
                items,
                [&](const int& item) {
                    ++started;
                    auto markFinished = wil::scope_exit([&]() { ++finished; });
                    if (returned)
                    {
                        ranAfterReturn = true;
                    }

                    return item;
                },
                [&](int /*result*/) {
                    ++successCount;
                    THROW_HR(E_ABORT);
                },
                [](const int& /*item*/, const std::exception_ptr& /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
                2),
            wil::ResultException,
            [&](const wil::ResultException& ex) {
                returned = true;
                return ex.GetErrorCode() == E_ABORT;
            });

        VERIFY_ARE_EQUAL(1, successCount);
        VERIFY_ARE_EQUAL(started.load(), finished.load());
        VERIFY_IS_FALSE(ranAfterReturn.load());
    }
};

} // namespace ThreadPoolUnitTests
