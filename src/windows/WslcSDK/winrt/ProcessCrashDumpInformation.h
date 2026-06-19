/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessCrashDumpInformation.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ProcessCrashDumpInformation class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ProcessCrashDumpInformation.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ProcessCrashDumpInformation : ProcessCrashDumpInformationT<ProcessCrashDumpInformation>
{
    ProcessCrashDumpInformation(const WslcSessionCrashDumpInfo* info);

    hstring DumpPath() const;
    hstring ProcessName() const;
    uint64_t Pid() const;
    uint32_t Signal() const;
    winrt::Windows::Foundation::DateTime Timestamp() const;

private:
    hstring m_dumpPath;
    hstring m_processName;
    uint64_t m_pid{};
    uint32_t m_signal{};
    winrt::Windows::Foundation::DateTime m_timestamp{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(ProcessCrashDumpInformation);
