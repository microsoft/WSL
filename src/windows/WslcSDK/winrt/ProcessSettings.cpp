/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ProcessSettings class.

--*/

#include "precomp.h"
#include "ProcessSettings.h"
#include "Microsoft.WSL.Containers.ProcessSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
hstring ProcessSettings::WorkingDirectory()
{
    throw hresult_not_implemented();
}
void ProcessSettings::WorkingDirectory(hstring const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IVector<hstring> ProcessSettings::CmdLine()
{
    throw hresult_not_implemented();
}
void ProcessSettings::CmdLine(winrt::Windows::Foundation::Collections::IVector<hstring> const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IMap<hstring, hstring> ProcessSettings::EnvironmentVariables()
{
    throw hresult_not_implemented();
}
void ProcessSettings::EnvironmentVariables(winrt::Windows::Foundation::Collections::IMap<hstring, hstring> const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
