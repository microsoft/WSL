// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <functional>
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/registry.h>

namespace wsl::windows::common {

constexpr DWORD registry_notify_filter = REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC;

class slim_registry_watcher
{
public:
    slim_registry_watcher() noexcept = default;

    // Pass a root key, sub key pair or use an empty string to use rootKey as the key to watch.
    HRESULT create(HKEY rootKey, _In_ PCWSTR subKey, bool isRecursive, std::function<void(::wil::RegistryChangeKind)>&& callback) noexcept
    {
        ::wil::unique_hkey keyToWatch;
        HRESULT hr = HRESULT_FROM_WIN32(::RegCreateKeyExW(rootKey, subKey, 0, nullptr, 0, KEY_NOTIFY, nullptr, &keyToWatch, nullptr));
        if (FAILED(hr))
        {
            return hr;
        }
        return create_common(std::move(keyToWatch), isRecursive, std::move(callback));
    }

    HRESULT create(::wil::unique_hkey&& keyToWatch, bool isRecursive, std::function<void(::wil::RegistryChangeKind)>&& callback) noexcept
    {
        return create_common(std::move(keyToWatch), isRecursive, std::move(callback));
    }

private:
    // using the default d'tor, destruction must occur in this order
    std::function<void(::wil::RegistryChangeKind)> m_callback;
    ::wil::unique_hkey m_keyToWatch;
    ::wil::unique_event_nothrow m_eventHandle;
    ::wil::unique_threadpool_wait m_threadPoolWait;
    bool m_isRecursive;

    static void __stdcall callback(PTP_CALLBACK_INSTANCE, void* context, TP_WAIT*, TP_WAIT_RESULT) noexcept
    {
        const auto this_ptr = static_cast<slim_registry_watcher*>(context);

        const LSTATUS error = ::RegNotifyChangeKeyValue(
            this_ptr->m_keyToWatch.get(), this_ptr->m_isRecursive, registry_notify_filter, this_ptr->m_eventHandle.get(), TRUE);

        // Call the client before re-arming to ensure that multiple callbacks don't
        // run concurrently.
        switch (error)
        {
        case ERROR_SUCCESS:
        case ERROR_ACCESS_DENIED:
            // Normal modification: send RegistryChangeKind::Modify and re-arm.
            this_ptr->m_callback(::wil::RegistryChangeKind::Modify);
            ::SetThreadpoolWait(this_ptr->m_threadPoolWait.get(), this_ptr->m_eventHandle.get(), nullptr);
            break;

        case ERROR_KEY_DELETED:
            // Key deleted: send RegistryChangeKind::Delete but do not re-arm.
            this_ptr->m_callback(::wil::RegistryChangeKind::Delete);
            break;

        case ERROR_HANDLE_REVOKED:
            // Handle revoked.  This can occur if the user session ends before the watcher shuts-down.
            // Do not re-arm since there is generally no way to respond.
            break;

        default:
            // failure here is a programming error.
            FAIL_FAST_HR(HRESULT_FROM_WIN32(error));
        }
    }

    HRESULT create_common(::wil::unique_hkey&& keyToWatch, bool isRecursive, std::function<void(::wil::RegistryChangeKind)>&& callback) noexcept
    {
        RETURN_IF_FAILED(m_eventHandle.create());

        m_threadPoolWait.reset(CreateThreadpoolWait(&slim_registry_watcher::callback, this, nullptr));
        RETURN_LAST_ERROR_IF(!m_threadPoolWait);

        // associate the notification handle with the threadpool before passing it to RegNotifyChangeKeyValue so we get immediate callbacks in the tp
        SetThreadpoolWait(m_threadPoolWait.get(), m_eventHandle.get(), nullptr);

        // 'this' object must be fully created before calling RegNotifyChangeKeyValue, as callbacks can start immediately
        m_keyToWatch = std::move(keyToWatch);
        m_isRecursive = isRecursive;
        m_callback = std::move(callback);

        // no failures after RegNotifyChangeKeyValue succeeds,
        RETURN_IF_WIN32_ERROR(RegNotifyChangeKeyValue(m_keyToWatch.get(), m_isRecursive, registry_notify_filter, m_eventHandle.get(), TRUE));
        return S_OK;
    }
};

} // namespace wsl::windows::common