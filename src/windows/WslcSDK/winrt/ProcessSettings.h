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
#include "Process.h"

namespace winrt::Microsoft::WSL::Containers::implementation {

struct StringArray
{
    StringArray() = default;
    StringArray(size_t capacity);
    void Add(std::string&& s);
    PCSTR* GetRawPointer();

private:
    std::vector<std::string> m_strings;
    std::vector<PCSTR> m_rawStrings;
};

struct ProcessSettings : ProcessSettingsT<ProcessSettings>
{
    ProcessSettings() = default;

    hstring WorkingDirectory();
    void WorkingDirectory(hstring const& value);
    winrt::Windows::Foundation::Collections::IVector<hstring> CommandLine();
    void CommandLine(winrt::Windows::Foundation::Collections::IVector<hstring> const& value);
    winrt::Windows::Foundation::Collections::IMap<hstring, hstring> EnvironmentVariables();
    void EnvironmentVariables(winrt::Windows::Foundation::Collections::IMap<hstring, hstring> const& value);
    winrt::Microsoft::WSL::Containers::ProcessOutputMode OutputMode();
    void OutputMode(winrt::Microsoft::WSL::Containers::ProcessOutputMode const& value);

    WslcProcessSettings* ToStructPointer();

private:
    std::string m_workingDirectory;
    winrt::Windows::Foundation::Collections::IVector<hstring> m_commandLine{winrt::single_threaded_vector<hstring>()};
    winrt::Windows::Foundation::Collections::IMap<hstring, hstring> m_environmentVariables{winrt::single_threaded_map<hstring, hstring>()};
    winrt::Microsoft::WSL::Containers::ProcessOutputMode m_outputMode{winrt::Microsoft::WSL::Containers::ProcessOutputMode::Discard};

    std::unique_ptr<WslcProcessSettings> m_processSettings;
    StringArray m_commandLineStrings;
    StringArray m_envStrings;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ProcessSettings : ProcessSettingsT<ProcessSettings, implementation::ProcessSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(ProcessSettings);
