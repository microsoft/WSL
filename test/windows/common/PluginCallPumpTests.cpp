// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "PluginCallPump.h"
#include <thread>

using wsl::windows::service::PluginCallPump;

class PluginCallPumpTests
{
    WSL_TEST_CLASS(PluginCallPumpTests)

    TEST_METHOD(CallbackRunsOnNotifyingThread)
    {
        PluginCallPump pump;
        const auto notifyingThread = GetCurrentThreadId();
        DWORD callbackThread = 0;
        HRESULT callbackResult = S_OK;
        bool invoked = false;
        const auto result = pump.Run([&] {
            invoked = pump.Invoke(
                [&] {
                    callbackThread = GetCurrentThreadId();
                    return E_ACCESSDENIED;
                },
                callbackResult);
            return S_FALSE;
        });
        VERIFY_ARE_EQUAL(S_FALSE, result);
        VERIFY_IS_TRUE(invoked);
        VERIFY_ARE_EQUAL(E_ACCESSDENIED, callbackResult);
        VERIFY_ARE_EQUAL(notifyingThread, callbackThread);
    }

    TEST_METHOD(CallbackExceptionUnblocksWorker)
    {
        PluginCallPump pump;
        HRESULT callbackResult = S_OK;
        const auto result = pump.Run([&] {
            pump.Invoke([]() -> HRESULT { THROW_HR(E_ABORT); }, callbackResult);
            return S_OK;
        });
        VERIFY_ARE_EQUAL(S_OK, result);
        VERIFY_ARE_EQUAL(E_ABORT, callbackResult);
    }

    TEST_METHOD(NotificationExceptionStopsPump)
    {
        PluginCallPump pump;
        VERIFY_ARE_EQUAL(E_ABORT, pump.Run([]() -> HRESULT { THROW_HR(E_ABORT); }));
        HRESULT callbackResult = E_PENDING;
        bool ran = false;
        VERIFY_IS_FALSE(pump.Invoke(
            [&] {
                ran = true;
                return S_OK;
            },
            callbackResult));
        VERIFY_IS_FALSE(ran);
        VERIFY_ARE_EQUAL(E_PENDING, callbackResult);
    }

    TEST_METHOD(CallbacksQueuedBeforeRunAreServiced)
    {
        PluginCallPump pump;
        wil::unique_event started;
        started.create();
        wil::unique_event done;
        done.create(wil::EventOptions::ManualReset);
        HRESULT callbackResult = E_PENDING;
        bool invoked = false;
        std::thread callback([&] {
            started.SetEvent();
            invoked = pump.Invoke([] { return S_FALSE; }, callbackResult);
            done.SetEvent();
        });
        auto join = wil::scope_exit([&] { callback.join(); });
        started.wait();
        VERIFY_ARE_EQUAL(S_OK, pump.Run([&] { return done.wait(10000) ? S_OK : HRESULT_FROM_WIN32(WAIT_TIMEOUT); }));
        callback.join();
        join.release();
        VERIFY_IS_TRUE(invoked);
        VERIFY_ARE_EQUAL(S_FALSE, callbackResult);
    }

    TEST_METHOD(ConcurrentCallbacksRunExactlyOnce)
    {
        PluginCallPump pump;
        constexpr size_t c_callers = 8;
        std::array<HRESULT, c_callers> results{};
        std::array<bool, c_callers> invoked{};
        size_t calls = 0;
        const auto result = pump.Run([&] {
            std::array<std::thread, c_callers> threads;
            auto join = wil::scope_exit([&] {
                for (auto& thread : threads)
                {
                    if (thread.joinable())
                    {
                        thread.join();
                    }
                }
            });
            for (size_t index = 0; index < threads.size(); ++index)
            {
                threads[index] = std::thread([&, index] {
                    invoked[index] = pump.Invoke(
                        [&] {
                            ++calls;
                            return S_OK;
                        },
                        results[index]);
                });
            }
            return S_OK;
        });
        VERIFY_ARE_EQUAL(S_OK, result);
        VERIFY_ARE_EQUAL(c_callers, calls);
        for (size_t index = 0; index < c_callers; ++index)
        {
            VERIFY_IS_TRUE(invoked[index]);
            VERIFY_ARE_EQUAL(S_OK, results[index]);
        }
    }

    TEST_METHOD(CancelledBeforeRunDoesNotStrandCallbacks)
    {
        PluginCallPump pump;
        wil::unique_event started;
        started.create();
        HRESULT callbackResult = E_PENDING;
        bool invoked = false;
        std::thread callback([&] {
            started.SetEvent();
            invoked = pump.Invoke([] { return S_OK; }, callbackResult);
        });
        started.wait();
        pump.Stop();
        callback.join();
        VERIFY_IS_TRUE(!invoked || callbackResult == RPC_E_DISCONNECTED);
    }
};
