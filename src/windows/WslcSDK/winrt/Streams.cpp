/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Streams.cpp

Abstract:

    This file contains the implementation of WinRT wrappers for streams.

--*/

#include "precomp.h"
#include "Streams.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

namespace winrt::Microsoft::WSL::Containers::implementation {

IOHandleInputStream::IOHandleInputStream(wil::unique_handle&& handle) : m_handle(std::move(handle))
{
}

IAsyncOperationWithProgress<IBuffer, uint32_t> IOHandleInputStream::ReadAsync(IBuffer buffer, uint32_t count, InputStreamOptions options)
{
    if (!m_handle)
    {
        throw winrt::hresult_illegal_method_call(L"Stream is closed");
    }

    if (options != InputStreamOptions::None)
    {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), L"Only InputStreamOptions::None is supported");
    }

    if (buffer == nullptr)
    {
        throw winrt::hresult_error(E_POINTER, L"Buffer cannot be null");
    }

    if (count > buffer.Capacity())
    {
        throw winrt::hresult_error(E_BOUNDS, L"Count cannot be greater than the buffer capacity");
    }

    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    DWORD bytesRead = 0;
    if (!ReadFile(self->m_handle.get(), buffer.data(), count, &bytesRead, nullptr))
    {
        const auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE)
        {
            bytesRead = 0;
        }
        else
        {
            THROW_WIN32(error);
        }
    }

    buffer.Length(bytesRead);
    co_return buffer;
}

void IOHandleInputStream::Close()
{
    m_handle.reset();
}

IOHandleOutputStream::IOHandleOutputStream(wil::unique_handle&& handle) : m_handle(std::move(handle))
{
}

IAsyncOperationWithProgress<uint32_t, uint32_t> IOHandleOutputStream::WriteAsync(IBuffer const& buffer)
{
    if (!m_handle)
    {
        throw winrt::hresult_illegal_method_call(L"Stream is closed");
    }

    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    DWORD bytesWritten = 0;
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(self->m_handle.get(), buffer.data(), buffer.Length(), &bytesWritten, nullptr));

    co_return bytesWritten;
}

winrt::Windows::Foundation::IAsyncOperation<bool> IOHandleOutputStream::FlushAsync()
{
    if (!m_handle)
    {
        throw winrt::hresult_illegal_method_call(L"Stream is closed");
    }

    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(self->m_handle.get()));
    co_return true;
}

void IOHandleOutputStream::Close()
{
    m_handle.reset();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
