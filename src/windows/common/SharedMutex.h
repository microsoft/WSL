#pragma once

#include <mutex>

namespace wsl::windows::common {

template <typename TMutex>
class SharedMutex
{
public:
    NON_COPYABLE(SharedMutex);

    SharedMutex() = default;

    class LockInstance
    {
    public:
        NON_COPYABLE(LockInstance);

        DEFAULT_MOVABLE(LockInstance);

        LockInstance(SharedMutex<TMutex>& mutex, std::unique_lock<TMutex>&& shared) : m_mutex(mutex), m_shared(std::move(shared))
        {
        }

        void UpgradeToExclusive()
        {
            m_exclusive = std::unique_lock<TMutex>{m_mutex.m_exclusive};
        }

        void ReleaseExclusive()
        {
            WI_ASSERT(m_exclusive.owns_lock());

            m_exclusive.unlock();
        }

    private:
        SharedMutex<TMutex>& m_mutex;
        std::unique_lock<TMutex> m_shared;
        std::unique_lock<TMutex> m_exclusive;
    };

    LockInstance LockShared()
    {
        return LockInstance{*this, std::unique_lock<TMutex>(m_shared)};
    }

    LockInstance LockExclusive()
    {
        auto lock = LockShared();
        lock.UpgradeToExclusive();

        return lock;
    }

private:
    void Unlock(bool exclusive)
    {
        if (exclusive)
        {
            m_exclusive.unlock();
        }

        m_shared.unlock();
    }

    TMutex m_shared;
    TMutex m_exclusive;
};

} // namespace wsl::windows::common