#pragma once

#include "defs.h"
#include <unordered_set>
#include <wil/com.h>
#include <wil/cppwinrt.h>

namespace wsl::windows::service::wsla {

template <typename T>
class WeakRefContainer
{
public:
    struct LockedElements
    {
        std::lock_guard<std::mutex> lock;
        std::unordered_set<T*>& elements;
    };

    WeakRefContainer(const WeakRefContainer&) = delete;
    WeakRefContainer(WeakRefContainer&&) = delete;

    WeakRefContainer& operator=(const WeakRefContainer&);
    WeakRefContainer& operator=(WeakRefContainer&&);

    WeakRefContainer() = default;
    ~WeakRefContainer()
    {
        std::lock_guard<std::mutex> guard(m_lock);

        for (const auto& e : m_elements)
        {
            e->SetContainer(nullptr);
        }
    }

    void Add(T* element)
    {
        std::lock_guard<std::mutex> guard(m_lock);

        element->SetContainer(this);
        m_elements.insert(element);
    }

    void Remove(T* element)
    {
        element->SetContainer(nullptr);

        std::lock_guard<std::mutex> guard(m_lock);
        m_elements.erase(element);
    }

    LockedElements Get()
    {
        return {std::lock_guard<std::mutex>(m_lock), m_elements};
    }

private:
    std::unordered_set<T*> m_elements;
    std::mutex m_lock;
};

template <typename T>
class WeakReference
{
public:
    NON_COPYABLE(WeakReference);
    WeakReference() = default;

    void SetContainer(WeakRefContainer<T>* container) noexcept
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_container = container;
    }

protected:
    WeakRefContainer<T>* m_container = nullptr;

    void OnDestroy()
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_container != nullptr)
        {
            m_container->Remove(static_cast<T*>(this));
            m_container = nullptr;
        }
    }

private:
    std::mutex m_lock;
};

} // namespace wsl::windows::service::wsla