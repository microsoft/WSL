// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <optional>
#include <mutex>
#include <condition_variable>

/**
 * @brief Class that contains a value T that can contain a single value and
 * blocks until post or get can be satisfied.
 *
 * @tparam Value stored in this class.
 */
template <typename T>
struct WaitableValue
{
public:
    /**
     * @brief Store value the value. Blocks until the value can be stored.
     *
     * @param[in] value Value to be stored.
     */
    void post(T& value)
    {
        std::unique_lock lck(m_mtx);
        while (m_value.has_value())
        {
            m_cv.wait(lck);
        }
        m_value = value;
        m_cv.notify_all();
    }

    /**
     * @brief Retrieve the value. Blocks until a value is available.
     *
     * @return Value that was previously stored.
     */
    T get()
    {
        std::unique_lock lck(m_mtx);
        while (!m_value.has_value())
        {
            m_cv.wait(lck);
        }
        auto return_value = m_value.value();
        m_value.reset();
        m_cv.notify_all();
        return return_value;
    }

    /**
     * @brief Attempt to retrieve the value with timeout.
     *
     * @param[in] timeout Duration to wait before returning empty.
     * @return Either the value or a std::nullopt on timeout.
     */
    template <typename duration>
    std::optional<T> try_get(duration timeout)
    {
        std::unique_lock lck(m_mtx);
        while (!m_value.has_value())
        {
            if (m_cv.wait_for(lck, timeout) == std::cv_status::timeout)
            {
                return std::nullopt;
            }
        }
        auto return_value = m_value.value();
        m_value.reset();
        m_cv.notify_all();
        return {return_value};
    }

private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::optional<T> m_value;
};
