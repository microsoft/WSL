/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessCrashDumpInformation.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ProcessCrashDumpInformation class.

--*/

#include "precomp.h"
#include "ProcessCrashDumpInformation.h"
#include "Microsoft.WSL.Containers.ProcessCrashDumpInformation.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ProcessCrashDumpInformation::ProcessCrashDumpInformation(const WslcSessionCrashDumpInfo* info)
{
    m_dumpPath = info->dumpPath;
    m_processName = winrt::to_hstring(info->processName);
    m_pid = info->pid;
    m_signal = info->signal;
    m_timestamp = winrt::clock::from_time_t(static_cast<time_t>(info->timestamp));
}

hstring ProcessCrashDumpInformation::DumpPath() const
{
    return m_dumpPath;
}

hstring ProcessCrashDumpInformation::ProcessName() const
{
    return m_processName;
}

uint64_t ProcessCrashDumpInformation::Pid() const
{
    return m_pid;
}

uint32_t ProcessCrashDumpInformation::Signal() const
{
    return m_signal;
}

winrt::Windows::Foundation::DateTime ProcessCrashDumpInformation::Timestamp() const
{
    return m_timestamp;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
