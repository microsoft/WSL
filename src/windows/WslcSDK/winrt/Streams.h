/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Streams.h

Abstract:

    This file contains the definition of WinRT stream wrappers for IO handles.
--*/

#pragma once
#include <winrt/Windows.Storage.Streams.h>

namespace winrt::Microsoft::WSL::Containers::implementation {

// WinRT IInputStream wrapper around a Windows HANDLE (read end).
struct IOHandleInputStream
    : winrt::implements<IOHandleInputStream, winrt::Windows::Storage::Streams::IInputStream, winrt::Windows::Foundation::IClosable>
{
    explicit IOHandleInputStream(wil::unique_handle&& handle);

    winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IBuffer, uint32_t> ReadAsync(
        winrt::Windows::Storage::Streams::IBuffer buffer, uint32_t count, winrt::Windows::Storage::Streams::InputStreamOptions options);

    void Close();

private:
    wil::unique_handle m_handle;
};

// WinRT IOutputStream wrapper around a Windows HANDLE (write end).
struct IOHandleOutputStream
    : winrt::implements<IOHandleOutputStream, winrt::Windows::Storage::Streams::IOutputStream, winrt::Windows::Foundation::IClosable>
{
    explicit IOHandleOutputStream(wil::unique_handle&& handle);

    winrt::Windows::Foundation::IAsyncOperationWithProgress<uint32_t, uint32_t> WriteAsync(winrt::Windows::Storage::Streams::IBuffer const& buffer);
    winrt::Windows::Foundation::IAsyncOperation<bool> FlushAsync();

    void Close();

private:
    wil::unique_handle m_handle;
};

} // namespace winrt::Microsoft::WSL::Containers::implementation
