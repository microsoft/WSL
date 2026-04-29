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
        auto _msg = (msg).get(); \
        if (_msg) \
        { \
            THROW_HR_IF_MSG(_hr, FAILED(_hr), "%ls", _msg); \
        } \
        else \
        { \
            THROW_IF_FAILED(_hr); \
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
auto* GetStructPointer(const T& obj)
{
    return obj ? winrt::get_self<typename implementation_type<T>::type>(obj)->ToStructPointer() : nullptr;
}

template <typename T>
auto GetStruct(const T& obj)
{
    return winrt::get_self<typename implementation_type<T>::type>(obj)->ToStruct();
}

template <typename T>
auto* GetHandlePointer(const T& obj)
{
    return obj ? winrt::get_self<typename implementation_type<T>::type>(obj)->ToHandlePointer() : nullptr;
}

template <typename T>
auto GetHandle(const T& obj)
{
    return obj ? winrt::get_self<typename implementation_type<T>::type>(obj)->ToHandle() : nullptr;
}

template <typename T>
auto* GetStructPointer(const winrt::com_ptr<T>& obj)
{
    return obj ? obj->ToStructPointer() : nullptr;
}

template <typename T>
auto GetStruct(const winrt::com_ptr<T>& obj)
{
    return obj->ToStruct();
}

template <typename T>
auto* GetHandlePointer(const winrt::com_ptr<T>& obj)
{
    return obj ? obj->ToHandlePointer() : nullptr;
}

template <typename T>
auto GetHandle(const winrt::com_ptr<T>& obj)
{
    return obj ? obj->ToHandle() : nullptr;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation