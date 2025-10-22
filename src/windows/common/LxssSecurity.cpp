/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Security.cpp

Abstract:

    This file contains user security function definitions.

--*/

#include "precomp.h"
#include "LxssSecurity.h"

using namespace Microsoft::WRL;

void Security::InitializeInstanceJob(_In_ HANDLE jobHandle)
{
    // Set the job limit flags.
    //
    // N.B. The kill on close flag is required to convert the job to a silo.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {};
    limitInfo.BasicLimitInformation.LimitFlags = (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &limitInfo, sizeof(limitInfo)));

    // Turn on timer-virtualization for this job.
    BOOLEAN enableTimerVirtualization = TRUE;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(
        jobHandle, JobObjectTimerVirtualizationInformation, &enableTimerVirtualization, sizeof(enableTimerVirtualization)));

    // Convert the job to a silo. This allows processes from multiple sessions
    // in the same job object.
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(jobHandle, JobObjectCreateSilo, nullptr, 0));
}

bool Security::IsTokenLocalAdministrator(_In_ HANDLE token)
{
    auto [sid, buffer] =
        wsl::windows::common::security::CreateSid(SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    BOOL member{};
    THROW_IF_WIN32_BOOL_FALSE(::CheckTokenMembership(token, sid, &member));

    return member;
}
