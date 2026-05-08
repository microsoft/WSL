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
    if (options != InputStreamOptions::None)
    {
        winrt::throw_hresult(ERROR_NOT_SUPPORTED);
    }

    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    THROW_HR_IF(E_BOUNDS, count > buffer.Capacity());

    DWORD bytesRead = 0;
    if (!ReadFile(self->m_handle.get(), buffer.data(), count, &bytesRead, nullptr))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_BROKEN_PIPE);
    }

    buffer.Length(bytesRead);
    co_return buffer;
}

IOHandleOutputStream::IOHandleOutputStream(wil::unique_handle&& handle) : m_handle(std::move(handle))
{
}

IAsyncOperationWithProgress<uint32_t, uint32_t> IOHandleOutputStream::WriteAsync(IBuffer const& buffer)
{
    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    DWORD bytesWritten = 0;
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(self->m_handle.get(), buffer.data(), buffer.Length(), &bytesWritten, nullptr));

    co_return bytesWritten;
}

winrt::Windows::Foundation::IAsyncOperation<bool> IOHandleOutputStream::FlushAsync()
{
    // Move to a background thread, ensuring that this object stays alive until the async operation completes.
    auto self = get_strong();
    co_await winrt::resume_background();

    THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(self->m_handle.get()));
    co_return true;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
