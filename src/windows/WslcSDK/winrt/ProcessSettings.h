/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ProcessSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ProcessSettings.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ProcessSettings : ProcessSettingsT<ProcessSettings>
{
    ProcessSettings() = default;

    hstring WorkingDirectory();
    void WorkingDirectory(hstring const& value);
    winrt::Windows::Foundation::Collections::IVector<hstring> CmdLine();
    void CmdLine(winrt::Windows::Foundation::Collections::IVector<hstring> const& value);
    winrt::Windows::Foundation::Collections::IMap<hstring, hstring> EnvironmentVariables();
    void EnvironmentVariables(winrt::Windows::Foundation::Collections::IMap<hstring, hstring> const& value);
    winrt::Microsoft::WSL::Containers::ProcessOutputHandler OnStdOut();
    void OnStdOut(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& value);
    winrt::Microsoft::WSL::Containers::ProcessOutputHandler OnStdErr();
    void OnStdErr(winrt::Microsoft::WSL::Containers::ProcessOutputHandler const& value);
    winrt::Microsoft::WSL::Containers::ProcessExitHandler OnExit();
    void OnExit(winrt::Microsoft::WSL::Containers::ProcessExitHandler const& value);

    WslcProcessSettings* ToStructPointer();

private:
    std::string m_workingDirectory;
    winrt::Windows::Foundation::Collections::IVector<hstring> m_cmdLine{
        winrt::single_threaded_vector<hstring>()
    };
    winrt::Windows::Foundation::Collections::IMap<hstring, hstring> m_environmentVariables{
        winrt::single_threaded_map<hstring, hstring>()
    };

    std::unique_ptr<WslcProcessSettings> m_processSettings;
    std::vector<std::string> m_cmdLineStrings;
    std::vector<std::string> m_envStrings;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ProcessSettings : ProcessSettingsT<ProcessSettings, implementation::ProcessSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(ProcessSettings);
