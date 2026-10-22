// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <wil/resource.h>

namespace wsl::windows::common {

// A capability for one VM incarnation, not an owning reference or an idle lease.
// Capture under the lifecycle lock; use only after releasing that lock.
class NativeAuditLaunch
{
public:
    using Connector = std::function<wil::unique_socket(const GUID&, ULONG, HANDLE, DWORD)>;

    NativeAuditLaunch(const GUID& runtimeId, HANDLE terminatingEvent);
    wil::unique_socket CreateRootNamespaceProcess(const std::vector<std::string>& arguments, const Connector& connector = {}) const;

private:
    GUID m_runtimeId{};
    wil::shared_handle m_terminatingEvent;
};

} // namespace wsl::windows::common
