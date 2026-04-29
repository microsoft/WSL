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
    return winrt::to_hstring(m_workingDirectory);
}

void ProcessSettings::WorkingDirectory(hstring const& value)
{
    if (m_processSettings)
    {
        throw hresult_illegal_state_change();
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
        throw hresult_illegal_state_change();
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
        throw hresult_illegal_state_change();
    }

    m_environmentVariables = value;
}

winrt::Microsoft::WSL::Containers::ProcessOutputHandler ProcessSettings::OnStdOut()
{
    throw hresult_not_implemented();
}

void ProcessSettings::OnStdOut(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& value)
{
    throw hresult_not_implemented();
}

winrt::Microsoft::WSL::Containers::ProcessOutputHandler ProcessSettings::OnStdErr()
{
    throw hresult_not_implemented();
}

void ProcessSettings::OnStdErr(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& value)
{
    throw hresult_not_implemented();
}

winrt::Microsoft::WSL::Containers::ProcessExitHandler ProcessSettings::OnExit()
{
    throw hresult_not_implemented();
}

void ProcessSettings::OnExit(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& value)
{
    throw hresult_not_implemented();
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

    if (m_cmdLine.Size() > 0)
    {
        m_cmdLineStrings.clear();
        m_cmdLineStrings.reserve(m_cmdLine.Size());
        std::vector<PCSTR> argv;
        argv.reserve(m_cmdLine.Size());
        for (auto const& arg : m_cmdLine)
        {
            m_cmdLineStrings.push_back(winrt::to_string(arg));
            argv.push_back(m_cmdLineStrings.back().c_str());
        }

        winrt::check_hresult(WslcSetProcessSettingsCmdLine(m_processSettings.get(), argv.data(), argv.size()));
    }

    if (m_environmentVariables.Size() > 0)
    {
        m_envStrings.clear();
        m_envStrings.reserve(m_environmentVariables.Size());
        std::vector<PCSTR> keyValues;
        keyValues.reserve(m_environmentVariables.Size());
        for (auto const& [key, value] : m_environmentVariables)
        {
            m_envStrings.push_back(winrt::to_string(key) + "=" + winrt::to_string(value));
            keyValues.push_back(m_envStrings.back().c_str());
        }

        winrt::check_hresult(WslcSetProcessSettingsEnvVariables(m_processSettings.get(), keyValues.data(), keyValues.size()));
    }

    return m_processSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
