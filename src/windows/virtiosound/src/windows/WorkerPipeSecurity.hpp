// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <array>
#include <cstddef>
#include <vector>

namespace wsl::virtio_sound::native::windows::detail {

// The worker runs at medium integrity while the capture source may now be
// hosted by a high-integrity child. Keep the DACL limited to the worker user
// and LocalSystem, and explicitly label the pipe medium so MIC permits the
// medium worker to exchange capture-control messages with its server.
inline bool BuildWorkerPipeSecurityDescriptor(
    _In_reads_bytes_(userSidBytes) const BYTE* userSid,
    DWORD userSidBytes,
    _Out_ std::vector<BYTE>& daclStorage,
    _Out_ std::vector<BYTE>& saclStorage,
    _Out_ SECURITY_DESCRIPTOR& descriptor) noexcept
{
    if (userSid == nullptr || userSidBytes == 0U || IsValidSid(const_cast<BYTE*>(userSid)) == FALSE ||
        GetLengthSid(const_cast<BYTE*>(userSid)) != userSidBytes)
    {
        return false;
    }

    std::array<BYTE, SECURITY_MAX_SID_SIZE> systemSid{};
    DWORD systemSidBytes = static_cast<DWORD>(systemSid.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &systemSidBytes) == FALSE)
    {
        return false;
    }

    std::array<BYTE, SECURITY_MAX_SID_SIZE> mediumLabelSid{};
    DWORD mediumLabelSidBytes = static_cast<DWORD>(mediumLabelSid.size());
    if (CreateWellKnownSid(WinMediumLabelSid, nullptr, mediumLabelSid.data(), &mediumLabelSidBytes) == FALSE)
    {
        return false;
    }

    constexpr std::size_t c_aceOverhead = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD);
    const std::size_t daclBytes = sizeof(ACL) + c_aceOverhead + userSidBytes + c_aceOverhead + systemSidBytes;
    const std::size_t labelAceBytes = sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + mediumLabelSidBytes;
    const std::size_t saclBytes = sizeof(ACL) + labelAceBytes;
    if (daclBytes > MAXDWORD || saclBytes > MAXDWORD)
    {
        return false;
    }

    try
    {
        daclStorage.assign(daclBytes, 0U);
        saclStorage.assign(saclBytes, 0U);
    }
    catch (...)
    {
        return false;
    }

    auto* dacl = reinterpret_cast<ACL*>(daclStorage.data());
    auto* sacl = reinterpret_cast<ACL*>(saclStorage.data());
    if (InitializeAcl(dacl, static_cast<DWORD>(daclStorage.size()), ACL_REVISION) == FALSE ||
        AddAccessAllowedAceEx(dacl, ACL_REVISION, 0U, FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE, const_cast<BYTE*>(userSid)) == FALSE ||
        AddAccessAllowedAceEx(dacl, ACL_REVISION, 0U, FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE, systemSid.data()) == FALSE ||
        InitializeAcl(sacl, static_cast<DWORD>(saclStorage.size()), ACL_REVISION) == FALSE ||
        AddMandatoryAce(sacl, ACL_REVISION, 0U, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, mediumLabelSid.data()) == FALSE ||
        InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
        SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) == FALSE ||
        SetSecurityDescriptorSacl(&descriptor, TRUE, sacl, FALSE) == FALSE)
    {
        return false;
    }
    return true;
}

} // namespace wsl::virtio_sound::native::windows::detail
