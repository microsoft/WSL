/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionCrashDumpInformation.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK SessionCrashDumpInformation class.

--*/

#include "precomp.h"
#include "SessionCrashDumpInformation.h"
#include "Microsoft.WSL.Containers.SessionCrashDumpInformation.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
SessionCrashDumpInformation::SessionCrashDumpInformation(const WslcSessionCrashDumpInfo* info)
{
    m_dumpPath = info->dumpPath;
    m_processName = winrt::to_hstring(info->processName);
    m_pid = info->pid;
    m_signal = info->signal;
    m_timestamp = winrt::clock::from_time_t(static_cast<time_t>(info->timestamp));
}

hstring SessionCrashDumpInformation::DumpPath() const
{
    return m_dumpPath;
}

hstring SessionCrashDumpInformation::ProcessName() const
{
    return m_processName;
}

uint64_t SessionCrashDumpInformation::Pid() const
{
    return m_pid;
}

uint32_t SessionCrashDumpInformation::Signal() const
{
    return m_signal;
}

winrt::Windows::Foundation::DateTime SessionCrashDumpInformation::Timestamp() const
{
    return m_timestamp;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
