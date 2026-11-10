// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeCancellation.h"
#include "wslutil.h"

namespace wsl::windows::service::wslc {

ComposeCancellation::ComposeCancellation(HANDLE sessionTerminatingEvent, HANDLE cancelEvent) :
    m_callerProcess(wsl::windows::common::wslutil::OpenCallingProcess(SYNCHRONIZE)), m_handles{sessionTerminatingEvent}
{
    for (const auto handle : {cancelEvent, m_callerProcess.get()})
    {
        if (handle != nullptr)
        {
            m_handles.push_back(handle);
        }
    }
}

void ComposeCancellation::Check() const
{
    const auto result = WaitForMultipleObjects(gsl::narrow<DWORD>(m_handles.size()), m_handles.data(), FALSE, 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), result < WAIT_OBJECT_0 + m_handles.size());
    THROW_HR_IF(E_UNEXPECTED, result != WAIT_TIMEOUT);
}

wil::mutex_release_scope_exit ComposeCancellation::AcquireMutex(HANDLE mutex) const
{
    auto handles = m_handles;
    handles.push_back(mutex);
    const auto index = gsl::narrow<DWORD>(handles.size() - 1);
    const auto result = WaitForMultipleObjects(gsl::narrow<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), result < WAIT_OBJECT_0 + index);
    THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 + index && result != WAIT_ABANDONED_0 + index);

    auto lock = wil::ReleaseMutex_scope_exit(mutex);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ABANDONED_WAIT_0), result == WAIT_ABANDONED_0 + index);
    Check();
    return lock;
}

} // namespace wsl::windows::service::wslc
