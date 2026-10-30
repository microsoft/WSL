// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <cstddef>
#include <vector>

namespace wsl::virtio_sound::native::windows::detail {

// Break away from the caller's job only for a session-crossing launch. The
// caller's job must explicitly allow breakaway; the child is then assigned to
// its own kill-on-close job through PROC_THREAD_ATTRIBUTE_JOB_LIST at creation.
constexpr DWORD CrossSessionBreakawayFlags(DWORD callerSession, DWORD targetSession) noexcept
{
    return callerSession == targetSession ? 0U : CREATE_BREAKAWAY_FROM_JOB;
}

// Owns the STARTUPINFOEX attribute-list storage needed to assign a new process
// to an already-configured job atomically during process creation. The job
// handle remains owned by the caller and must stay open through CreateProcess.
class JobBoundProcessStartup final
{
public:
    explicit JobBoundProcessStartup(_In_ HANDLE job) : m_job(job)
    {
        if (m_job == nullptr || m_job == INVALID_HANDLE_VALUE)
        {
            THROW_HR_MSG(E_INVALIDARG, "A worker job handle is required.");
        }

        SIZE_T requiredBytes = 0U;
        SetLastError(ERROR_SUCCESS);
        const BOOL sizeQuerySucceeded = InitializeProcThreadAttributeList(nullptr, 1U, 0U, &requiredBytes);
        const DWORD sizeQueryError = GetLastError();
        if (sizeQuerySucceeded != FALSE || sizeQueryError != ERROR_INSUFFICIENT_BUFFER || requiredBytes == 0U)
        {
            if (sizeQueryError != ERROR_SUCCESS)
            {
                THROW_HR_MSG(HRESULT_FROM_WIN32(sizeQueryError), "InitializeProcThreadAttributeList(size) failed");
            }
            THROW_HR_MSG(E_UNEXPECTED, "InitializeProcThreadAttributeList(size) returned an invalid size");
        }

        m_storage.resize(requiredBytes);
        auto* attributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
        if (InitializeProcThreadAttributeList(attributeList, 1U, 0U, &requiredBytes) == FALSE)
        {
            ThrowLastError("InitializeProcThreadAttributeList");
        }
        m_attributes.reset(attributeList);

        if (UpdateProcThreadAttribute(m_attributes.get(), 0U, PROC_THREAD_ATTRIBUTE_JOB_LIST, &m_job, sizeof(m_job), nullptr, nullptr) == FALSE)
        {
            ThrowLastError("UpdateProcThreadAttribute(JOB_LIST)");
        }

        m_startup.StartupInfo.cb = sizeof(m_startup);
        m_startup.lpAttributeList = m_attributes.get();
    }

    JobBoundProcessStartup(const JobBoundProcessStartup&) = delete;
    JobBoundProcessStartup& operator=(const JobBoundProcessStartup&) = delete;
    JobBoundProcessStartup(JobBoundProcessStartup&&) = delete;
    JobBoundProcessStartup& operator=(JobBoundProcessStartup&&) = delete;

    STARTUPINFOEXW& Info() noexcept
    {
        return m_startup;
    }

    static constexpr DWORD RequiredCreationFlags() noexcept
    {
        return EXTENDED_STARTUPINFO_PRESENT;
    }

private:
    [[noreturn]] static void ThrowLastError(const char* operation)
    {
        const DWORD error = GetLastError();
        const HRESULT result = error == ERROR_SUCCESS ? E_UNEXPECTED : HRESULT_FROM_WIN32(error);
        THROW_HR_MSG(result, "%hs failed", operation);
    }

    HANDLE m_job{};
    std::vector<std::byte> m_storage;
    wil::unique_any<PPROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList), DeleteProcThreadAttributeList> m_attributes;
    STARTUPINFOEXW m_startup{};
};

} // namespace wsl::virtio_sound::native::windows::detail
