// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9scheduler.h"
#include "p9errors.h"

namespace p9fs {

// Provides a movable wrapper around std::coroutine_handle that
// destroys the coroutine when it goes out of scope.
template <class PromiseType>
class UniqueCoroutineHandle
{
public:
    UniqueCoroutineHandle() = default;

    ~UniqueCoroutineHandle()
    {
        Reset();
    }

    UniqueCoroutineHandle(std::coroutine_handle<PromiseType> handle) : m_Handle(handle)
    {
    }

    UniqueCoroutineHandle(UniqueCoroutineHandle&& handle) : m_Handle(handle.m_Handle)
    {
        handle.m_Handle = nullptr;
    }

    UniqueCoroutineHandle& operator=(UniqueCoroutineHandle&& handle)
    {
        if (this != &handle)
        {
            Reset();
            m_Handle = handle.m_Handle;
            handle.m_Handle = nullptr;
        }

        return *this;
    }

    UniqueCoroutineHandle(const UniqueCoroutineHandle&) = delete;
    UniqueCoroutineHandle& operator=(const UniqueCoroutineHandle&) = delete;

    void Reset()
    {
        if (m_Handle)
        {
            FAIL_FAST_IF(!m_Handle.done());
            m_Handle.destroy();
            m_Handle = nullptr;
        }
    }

    std::coroutine_handle<PromiseType> Get() const
    {
        return m_Handle;
    }

    explicit operator bool() const
    {
        return static_cast<bool>(m_Handle);
    }

private:
    std::coroutine_handle<PromiseType> m_Handle{};
};

// Async task is an awaitable task object whose resources are released
// asynchronously from being awaited on. This requires an extra heap allocation,
// so only use this when you need to have a task that you don't plan to
// immediately await.
class AsyncTask
{
private:
    struct Storage
    {
        std::exception_ptr Exception;
        std::mutex Mutex;
        std::condition_variable Condition;
        std::atomic<bool> Done;
        std::atomic<void*> Waiter;
    };

public:
    AsyncTask() = default;

    AsyncTask(const std::shared_ptr<Storage>& storage) : m_Storage(storage)
    {
    }

    bool await_ready() const
    {
        return m_Storage->Done;
    }

    // Wait for the promise to resume us, unless the coroutine has
    // already completed.
    bool await_suspend(std::coroutine_handle<> handle)
    {
        m_Storage->Waiter = handle.address();
        if ((m_Storage->Done || m_Storage->Exception) && m_Storage->Waiter.exchange(nullptr) != nullptr)
        {
            return false;
        }

        return true;
    }

    // Return the result from the promise object.
    void await_resume() const
    {
        if (m_Storage->Exception)
        {
            std::rethrow_exception(m_Storage->Exception);
        }
    }

    explicit operator bool()
    {
        return bool{m_Storage};
    }

    void Get()
    {
        std::unique_lock<std::mutex> lock{m_Storage->Mutex};
        m_Storage->Condition.wait(lock, [this]() -> bool { return m_Storage->Done; });

        await_resume();
    }

    class promise_type
    {
    public:
        std::suspend_never initial_suspend()
        {
            return {};
        }

        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void return_void()
        {
            Signal();
        }

        void unhandled_exception()
        {
            m_Storage->Exception = std::current_exception();
            Signal();
        }

        AsyncTask get_return_object() const
        {
            return AsyncTask{m_Storage};
        }

    private:
        void Signal()
        {
            {
                std::lock_guard<std::mutex> lock{m_Storage->Mutex};
                m_Storage->Done = true;
            }

            m_Storage->Condition.notify_all();
            auto address = m_Storage->Waiter.exchange(nullptr);
            if (address)
            {
                auto waiter = std::coroutine_handle<>::from_address(address);
                g_Scheduler.Schedule(waiter);
            }

            m_Storage.reset();
        }

        std::shared_ptr<Storage> m_Storage{std::make_shared<Storage>()};
    };

private:
    std::shared_ptr<Storage> m_Storage;
};

template <class T>
class PromiseBase : public T
{
public:
    auto initial_suspend()
    {
        return std::suspend_never{};
    }

    // When this coroutine function exits, signal the waiter and then suspend so
    // that the promise stays alive long enough to get the result out of it.
    auto final_suspend() noexcept
    {
        return FinalAwaiter{};
    }

    // Stores an exception object for rethrowing when awaiting on
    // the associated task.
    void unhandled_exception()
    {
        m_Exception = std::current_exception();
    }

protected:
    // Rethrows the coroutine's exception if there is one.
    void CheckException()
    {
        if (m_Exception)
        {
            std::rethrow_exception(m_Exception);
        }
    }

private:
    struct FinalAwaiter
    {
        static bool await_ready() noexcept
        {
            return false;
        }

        template <class U>
        void await_suspend(std::coroutine_handle<U> handle) noexcept
        {
            handle.promise().Resume();
        }

        static void await_resume() noexcept
        {
        }
    };

    std::exception_ptr m_Exception;
};

// Promise type for coroutines that return values.
template <class T, class Task, class Base>
class Promise : public PromiseBase<Base>
{
public:
    Task get_return_object()
    {
        return {*this};
    }

    template <class U>
    void return_value(U&& value)
    {
        m_Value = std::forward<U>(value);
    }

    T GetResult()
    {
        PromiseBase<Base>::CheckException();
        return std::move(m_Value);
    }

private:
    T m_Value;
};

// Promise type for coroutines that do not return values.
template <class Task, class Base>
class Promise<void, Task, Base> : public PromiseBase<Base>
{
public:
    Task get_return_object()
    {
        return {*this};
    }

    void return_void()
    {
    }

    void GetResult()
    {
        PromiseBase<Base>::CheckException();
    }
};

// Promise type for Task objects. Represents a coroutine that will eventually
// return or throw an exception.
class TaskPromise
{
public:
    // Sets the unique waiting task. Returns false if the promise already
    // completed.
    bool SetWaiter(std::coroutine_handle<> handle)
    {
        m_Waiter = handle.address();
        return !m_WaiterOrDone.exchange(true);
    }

    // Resumes the waiting task, if there is one.
    void Resume()
    {
        // Set the value to true to indicate we're done. If it was already true,
        // it indicates a waiter was registered before completion so resume it.
        if (m_WaiterOrDone.exchange(true))
        {
            auto waiter = std::coroutine_handle<>::from_address(m_Waiter);
            m_Waiter = nullptr;
            g_Scheduler.Schedule(waiter);
        }
    }

    bool Done() const
    {
        return m_WaiterOrDone.load(std::memory_order_acquire);
    }

private:
    void* m_Waiter{};
    std::atomic<bool> m_WaiterOrDone{false};
};

// Task is an awaitable task that is designed to be co_awaited immediately. It
// will not release the underlying coroutine's resources until it goes out of
// scope.
template <class T>
class Task
{
public:
    using promise_type = Promise<T, Task<T>, TaskPromise>;

    bool await_ready()
    {
        return Promise().Done();
    }

    // Start the coroutine and wait for the promise to resume us.
    bool await_suspend(std::coroutine_handle<> handle)
    {
        // Try to set the waiter. If the coroutine has already completed, abort
        // the suspend.
        return Promise().SetWaiter(handle);
    }

    // Return the result from the promise object.
    auto await_resume()
    {
        return Promise().GetResult();
    }

    Task() = default;

    Task(promise_type& promise) : m_Coroutine(std::coroutine_handle<promise_type>::from_promise(promise))
    {
    }

private:
    promise_type& Promise()
    {
        return m_Coroutine.Get().promise();
    }

    UniqueCoroutineHandle<promise_type> m_Coroutine;
};

class ScheduledTask
{
public:
    struct promise_type
    {
        ScheduledTask get_return_object()
        {
            return {*this};
        }

        std::suspend_always initial_suspend()
        {
            return {};
        }

        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void unhandled_exception()
        {
            try
            {
                std::rethrow_exception(std::current_exception());
            }
            catch (...)
            {
                FAIL_FAST_CAUGHT_EXCEPTION();
            }
        }

        static void return_void()
        {
        }
    };

    ScheduledTask(promise_type& promise)
    {
        g_Scheduler.Schedule(std::coroutine_handle<promise_type>::from_promise(promise));
    }
};

/// Non-awaitable wrapper to schedule a coroutine to run on another thread.
template <class T>
void RunScheduledTask(T&& awaitable)
{
    [](T func) -> ScheduledTask { co_await std::move(func)(); }(std::forward<T>(awaitable));
}

template <typename T>
void RunAsyncTask(T&& awaitable)
{
    [](T func) -> AsyncTask { co_await std::move(func)(); }(std::forward<T>(awaitable));
}

/// Awaitable wrapper to run synchronous blocking code without blocking
/// outstanding coroutines.
template <class T>
auto BlockingCode(T func) -> Task<decltype(func())>
{
    const bool unblock = g_Scheduler.Block();
    auto result = func();
    if (unblock)
    {
        co_await g_Scheduler.Unblock();
    }

    co_return result;
}

// Awaitable semaphore.
class AsyncSemaphore
{
public:
    class AsyncSemaphoreTask
    {
    public:
        AsyncSemaphoreTask(AsyncSemaphore& evt, uint64_t count) : m_Semaphore{evt}, m_Count{count}
        {
        }

        bool await_ready() const noexcept
        {
            return m_Count == 0;
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept
        {
            m_Awaiter = awaiter;
            return m_Semaphore.Enqueue(this, m_Count);
        }

        static void await_resume() noexcept
        {
        }

    private:
        friend class AsyncSemaphore;
        AsyncSemaphore& m_Semaphore;
        AsyncSemaphoreTask* m_Next;
        std::coroutine_handle<> m_Awaiter;
        uint64_t m_Count;
    };

    AsyncSemaphore(uint64_t initialCount) : m_Count(initialCount)
    {
    }

    AsyncSemaphore(AsyncSemaphore&) = delete;

    AsyncSemaphoreTask Acquire(uint64_t count) noexcept
    {
        if (TryAcquire(count))
        {
            count = 0;
        }

        return AsyncSemaphoreTask(*this, count);
    }

    bool TryAcquire(uint64_t count) noexcept
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        if (m_Count >= count)
        {
            m_Count -= count;
            return true;
        }

        return false;
    }

    void Release(uint64_t count) noexcept
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        m_Count += count;

        // Wake all possible waiters.
        AsyncSemaphoreTask** head = &m_Waiter;
        if (*head != nullptr)
        {
            const auto waiter = *head;
            if (m_Count >= waiter->m_Count)
            {
                m_Count -= waiter->m_Count;
                *head = waiter->m_Next;
                g_Scheduler.Schedule(waiter->m_Awaiter);
            }
            else
            {
                head = &waiter->m_Next;
            }
        }
    }

private:
    bool Enqueue(AsyncSemaphoreTask* task, uint64_t count)
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        if (m_Count >= count)
        {
            m_Count -= count;
            return false;
        }

        task->m_Next = m_Waiter;
        m_Waiter = task;
        return true;
    }

    std::mutex m_Lock;
    uint64_t m_Count;
    AsyncSemaphoreTask* m_Waiter{};
};

class AsyncEvent
{
public:
    class AsyncEventTask
    {
    public:
        AsyncEventTask(AsyncEvent& evt) : m_Event{evt}
        {
        }

        bool await_ready() const noexcept
        {
            return m_Event.IsSet();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept
        {
            m_Awaiter = awaiter;
            ULONG_PTR state = m_Event.m_State;
            while (true)
            {
                if (state == m_SetState)
                {
                    return false;
                }

                m_Next = reinterpret_cast<AsyncEventTask*>(state);
                if (m_Event.m_State.compare_exchange_weak(state, reinterpret_cast<ULONG_PTR>(this)))
                {
                    return true;
                }
            }
        }

        static void await_resume() noexcept
        {
        }

    private:
        friend class AsyncEvent;
        AsyncEvent& m_Event;
        AsyncEventTask* m_Next;
        std::coroutine_handle<> m_Awaiter;
    };

    AsyncEvent() : m_State{m_UnsetState}
    {
    }

    AsyncEvent(AsyncEvent&) = delete;

    AsyncEventTask operator co_await() noexcept
    {
        return AsyncEventTask(*this);
    }

    bool IsSet() const noexcept
    {
        return m_State == m_SetState;
    }

    void Set() noexcept
    {
        const ULONG_PTR state = m_State.exchange(m_SetState);
        if (state != m_SetState)
        {
            // Resume all waiters.
            auto task = reinterpret_cast<AsyncEventTask*>(state);
            while (task != nullptr)
            {
                const auto next = task->m_Next;
                g_Scheduler.Schedule(task->m_Awaiter);
                task = next;
            }
        }
    }

    void Reset()
    {
        ULONG_PTR state = m_SetState;
        m_State.compare_exchange_strong(state, m_UnsetState);
    }

private:
    static constexpr ULONG_PTR m_SetState = 1;
    static constexpr ULONG_PTR m_UnsetState = 0;

    // State is NULL if the event is not set with no waiters, 1 if it's set,
    // or a pointer to the first waiter.
    std::atomic<ULONG_PTR> m_State;
};

// Mutex lock that can be waited on with co_await.
// N.B. Any waiters will be resumed on the thread that calls Unlock()
class AsyncLock
{
public:
    class AsyncLockTask;

    // RAII class that releases the lock on scope exit.
    // N.B. Unlike std::lock_guard, it does not acquire the lock; it must be
    //      created when the lock is already owned.
    class AsyncLockGuard
    {
    public:
        AsyncLockGuard(AsyncLockGuard&& lock) : m_Lock(lock.m_Lock)
        {
            lock.m_Lock = nullptr;
        }

        AsyncLockGuard(const AsyncLockGuard&) = delete;

        ~AsyncLockGuard()
        {
            if (m_Lock != nullptr)
            {
                m_Lock->Unlock();
            }
        }

    private:
        friend class AsyncLockTask;

        AsyncLockGuard(AsyncLock& lock) : m_Lock{&lock}
        {
        }

        AsyncLock* m_Lock;
    };

    class AsyncLockTask
    {
    public:
        AsyncLockTask(AsyncLock& lock) : m_Lock{lock}, m_Next{nullptr}
        {
        }

        static bool await_ready() noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> awaiter)
        {
            m_Awaiter = awaiter;
            ULONG_PTR state = m_Lock.m_State;
            while (true)
            {
                if (state == m_UnlockedState)
                {
                    // Acquire the lock and complete synchronously if it is
                    // not currently held.
                    if (m_Lock.m_State.compare_exchange_weak(state, m_LockedState))
                    {
                        return false;
                    }
                }
                else
                {
                    // Add this instance to the list of new waiters.
                    if (state != m_LockedState)
                    {
                        m_Next = reinterpret_cast<AsyncLockTask*>(state);
                    }
                    else
                    {
                        // Must reset in case the loop ran more than once.
                        m_Next = nullptr;
                    }

                    if (m_Lock.m_State.compare_exchange_weak(state, reinterpret_cast<ULONG_PTR>(this)))
                    {
                        return true;
                    }
                }
            }
        }

        AsyncLockGuard await_resume() const noexcept
        {
            return AsyncLockGuard(m_Lock);
        }

    private:
        friend class AsyncLock;
        AsyncLock& m_Lock;
        AsyncLockTask* m_Next;
        std::coroutine_handle<> m_Awaiter;
    };

    AsyncLock() : m_State{m_UnlockedState}, m_WaitList{nullptr}
    {
    }

    AsyncLock(const AsyncLock&) = delete;

    AsyncLockTask Lock() noexcept
    {
        return AsyncLockTask(*this);
    }

    bool TryLock() noexcept
    {
        ULONG_PTR unlockedState = m_UnlockedState;
        return m_State.compare_exchange_strong(unlockedState, m_LockedState);
    }

    void Unlock() noexcept
    {
        // If there are no existing waiters and no new waiters, unlock and
        // return.
        // N.B. Since m_WaitList is only accessed from the unlock method,
        //      which should only be called with the lock held, it needs no
        //      synchronization.
        ULONG_PTR state = m_LockedState;
        if ((m_WaitList == nullptr) && (m_State.compare_exchange_strong(state, m_UnlockedState)))
        {
            return;
        }

        // If there are no existing waiters, transfer the list of new waiters.
        if (m_WaitList == nullptr)
        {
            // Take ownership of the list of new waiters, leaving the state
            // locked.
            state = m_State.exchange(m_LockedState);
            FAIL_FAST_IF(state == m_LockedState || state == m_UnlockedState);

            // Reverse the list and transfer it. This ensures waiters are
            // awoken in FIFO order.
            auto* current = reinterpret_cast<AsyncLockTask*>(state);
            AsyncLockTask* previous = nullptr;
            AsyncLockTask* next = nullptr;
            while (current != nullptr)
            {
                next = current->m_Next;
                current->m_Next = previous;
                previous = current;
                current = next;
            }

            m_WaitList = previous;
        }

        // Awake the first waiter; this transfers lock ownership to them.
        AsyncLockTask* head = m_WaitList;
        m_WaitList = head->m_Next;
        g_Scheduler.Schedule(head->m_Awaiter);
    }

private:
    static constexpr ULONG_PTR m_LockedState = 1;
    static constexpr ULONG_PTR m_UnlockedState = 0;

    // State is NULL if the lock is not held, 1 if it's held and there are no
    // new waiters, or a pointer to the first new waiter in LIFO order.
    std::atomic<ULONG_PTR> m_State;
    // List of existing waiters in FIFO order.
    AsyncLockTask* m_WaitList;
};

class ICancellable
{
public:
    virtual ~ICancellable() = default;

    virtual void Cancel() = 0;
};

// Token used to cancel an outstanding IO operation.
class CancelToken
{
public:
    CancelToken()
    {
        InitializeListHead(&m_Children);
    }

    CancelToken(CancelToken& parent) : CancelToken()
    {
        if (parent.AddChild(*this))
        {
            m_Parent = &parent;
        }
        else
        {
            m_Cancelled = true;
        }
    }

    ~CancelToken()
    {
        if (m_Parent)
        {
            m_Parent->RemoveChild(*this);
        }
    }

    // Register a running IO as cancellable.
    bool Register(ICancellable& operation)
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        if (m_Cancelled)
        {
            return false;
        }

        m_Operation = &operation;
        return true;
    }

    // Unregister the currently registered overlapped structure.
    void Unregister()
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        m_Operation = nullptr;
    }

    // Cancels the token, cancelling any associated outstanding IO.
    void Cancel()
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        const bool wasCancelled = m_Cancelled;
        m_Cancelled = true;
        if (!wasCancelled)
        {
            if (m_Operation != nullptr)
            {
                m_Operation->Cancel();
            }

            for (auto entry = m_Children.Flink; entry != &m_Children; entry = entry->Flink)
            {
                const auto child = CONTAINING_RECORD(entry, CancelToken, m_Link);
                child->Cancel();
            }
        }

        m_Cancelled = true;
    }

    // Returns whether the token has already been cancelled.
    bool Cancelled()
    {
        return m_Cancelled;
    }

    // Resets the state of the token. This should only be used when
    // the token is no longer in use by any IOs.
    void Reset()
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        FAIL_FAST_IF(m_Operation != nullptr || !IsListEmpty(&m_Children));
        m_Cancelled = false;
    }

private:
    bool AddChild(CancelToken& child)
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        if (m_Cancelled)
        {
            return false;
        }

        InsertTailList(&m_Children, &child.m_Link);
        return true;
    }

    void RemoveChild(CancelToken& child)
    {
        std::lock_guard<std::mutex> lock{m_Lock};
        RemoveEntryList(&child.m_Link);
    }

    std::mutex m_Lock{};
    ICancellable* m_Operation{};
    std::atomic<bool> m_Cancelled{};
    CancelToken* m_Parent{};
    LIST_ENTRY m_Children{};
    LIST_ENTRY m_Link{};
};

} // namespace p9fs
