/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Helpers.h

Abstract:

    This file contains helpers for the WinRT wrapper of WSLC SDK.

--*/

#pragma once

#define THROW_MSG_IF_FAILED(hr, msg) \
    do \
    { \
        const auto _hr = (hr); \
        if (FAILED(_hr)) \
        { \
            auto _msg = (msg).get(); \
            if (_msg) \
            { \
                throw winrt::hresult_error(_hr, winrt::to_hstring(_msg)); \
            } \
            else \
            { \
                winrt::throw_hresult(_hr); \
            } \
        } \
    } while (0)

template <typename T>
struct implementation_type;

#define DEFINE_TYPE_HELPERS(__type__) \
    template <> \
    struct implementation_type<winrt::Microsoft::WSL::Containers::__type__> \
    { \
        using type = winrt::Microsoft::WSL::Containers::implementation::__type__; \
    };

namespace winrt::Microsoft::WSL::Containers::implementation {
template <typename T>
auto GetImplementation(const T& obj)
{
    return winrt::get_self<typename implementation_type<T>::type>(obj);
}

template <typename T>
auto* GetStructPointer(const T& obj)
{
    return obj ? GetImplementation(obj)->ToStructPointer() : nullptr;
}

template <typename T>
auto GetStruct(const T& obj)
{
    return GetImplementation(obj)->ToStruct();
}

template <typename T>
auto GetHandle(const T& obj)
{
    return obj ? GetImplementation(obj)->ToHandle() : nullptr;
}

template <typename T>
auto* GetStructPointer(const winrt::com_ptr<T>& obj)
{
    return obj ? obj->ToStructPointer() : nullptr;
}

// Helper for forwarding C progress callbacks to a WinRT progress token.
// An instance of this struct is used as the context for the C callback.
// The callback is invoked synchronously during the blocking C call, so the context
// lives on the coroutine stack and is always valid for the duration of the call.
template <typename T>
struct ProgressCallbackHelper
{
    template <typename ProgressTokenT>
    ProgressCallbackHelper(ProgressTokenT progressToken) :
        m_reportProgress([progressToken](T progress) { progressToken(progress); })
    {
    }

    static void ReportProgress(PVOID context, T progress)
    {
        auto callbackContext = static_cast<ProgressCallbackHelper*>(context);
        callbackContext->m_reportProgress(progress);
    }

    const std::function<void(T)> m_reportProgress;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
