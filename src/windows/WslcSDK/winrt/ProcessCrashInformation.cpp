/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessCrashInformation.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ProcessCrashInformation class.

--*/

#include "precomp.h"
#include "ProcessCrashInformation.h"
#include "Microsoft.WSL.Containers.ProcessCrashInformation.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ProcessCrashInformation::ProcessCrashInformation(const WslcSessionCrashDumpInfo* info)
{
    m_dumpPath = info->dumpPath;
    m_processName = winrt::to_hstring(info->processName);
    m_pid = info->pid;
    m_signal = info->signal;
    m_timestamp = winrt::clock::from_time_t(static_cast<time_t>(info->timestamp));
}

hstring ProcessCrashInformation::DumpPath() const
{
    return m_dumpPath;
}

hstring ProcessCrashInformation::ProcessName() const
{
    return m_processName;
}

uint32_t ProcessCrashInformation::Pid() const
{
    return m_pid;
}

uint32_t ProcessCrashInformation::Signal() const
{
    return m_signal;
}

winrt::Windows::Foundation::DateTime ProcessCrashInformation::Timestamp() const
{
    return m_timestamp;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
