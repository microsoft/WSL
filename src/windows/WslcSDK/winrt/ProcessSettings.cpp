/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ProcessSettings class.

--*/

#include "precomp.h"
#include "ProcessSettings.h"
#include "Process.h"
#include "Microsoft.WSL.Containers.ProcessSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

StringArray::StringArray(size_t capacity)
{
    m_strings.reserve(capacity);
    m_rawStrings.reserve(capacity);
}

void StringArray::Add(std::string&& s)
{
    m_strings.push_back(std::move(s));
    m_rawStrings.push_back(m_strings.back().c_str());
}

PCSTR* StringArray::GetRawPointer()
{
    return m_rawStrings.data();
}

hstring ProcessSettings::WorkingDirectory()
{
    return winrt::to_hstring(m_workingDirectory);
}

void ProcessSettings::WorkingDirectory(hstring const& value)
{
    if (m_processSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_workingDirectory = winrt::to_string(value);
}

winrt::Windows::Foundation::Collections::IVector<hstring> ProcessSettings::CmdLine()
{
    return m_cmdLine;
}

void ProcessSettings::CmdLine(winrt::Windows::Foundation::Collections::IVector<hstring> const& value)
{
    if (m_processSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"CmdLine cannot be null");
    }

    m_cmdLine = value;
}

winrt::Windows::Foundation::Collections::IMap<hstring, hstring> ProcessSettings::EnvironmentVariables()
{
    return m_environmentVariables;
}

void ProcessSettings::EnvironmentVariables(winrt::Windows::Foundation::Collections::IMap<hstring, hstring> const& value)
{
    if (m_processSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"EnvironmentVariables cannot be null");
    }

    m_environmentVariables = value;
}

winrt::Microsoft::WSL::Containers::ProcessOutputMode ProcessSettings::OutputMode()
{
    return m_outputMode;
}

void ProcessSettings::OutputMode(winrt::Microsoft::WSL::Containers::ProcessOutputMode const& value)
{
    if (m_processSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_outputMode = value;
}

WslcProcessSettings* ProcessSettings::ToStructPointer()
{
    if (m_processSettings)
    {
        return m_processSettings.get();
    }

    m_processSettings = std::make_unique<WslcProcessSettings>();
    winrt::check_hresult(WslcInitProcessSettings(m_processSettings.get()));

    if (!m_workingDirectory.empty())
    {
        winrt::check_hresult(WslcSetProcessSettingsWorkingDirectory(m_processSettings.get(), m_workingDirectory.c_str()));
    }

    if (m_cmdLine && m_cmdLine.Size() > 0)
    {
        auto argc = m_cmdLine.Size();
        m_cmdLineStrings = StringArray{argc};
        for (auto const& arg : m_cmdLine)
        {
            m_cmdLineStrings.Add(winrt::to_string(arg));
        }

        winrt::check_hresult(WslcSetProcessSettingsCmdLine(m_processSettings.get(), m_cmdLineStrings.GetRawPointer(), argc));
    }

    if (m_environmentVariables.Size() > 0)
    {
        auto size = m_environmentVariables.Size();
        m_envStrings = StringArray{size};
        for (auto const& [key, value] : m_environmentVariables)
        {
            m_envStrings.Add(winrt::to_string(key) + "=" + winrt::to_string(value));
        }

        winrt::check_hresult(WslcSetProcessSettingsEnvVariables(m_processSettings.get(), m_envStrings.GetRawPointer(), size));
    }

    return m_processSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
