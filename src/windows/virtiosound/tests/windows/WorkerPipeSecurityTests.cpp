// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WorkerPipeSecurity.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using wsl::virtio_sound::native::windows::detail::BuildWorkerPipeSecurityDescriptor;

constexpr ACCESS_MASK c_pipeAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE;

bool MakeUserSid(std::array<BYTE, SECURITY_MAX_SID_SIZE>& storage, DWORD& sidBytes)
{
    sidBytes = static_cast<DWORD>(storage.size());
    return CreateWellKnownSid(WinBuiltinUsersSid, nullptr, storage.data(), &sidBytes) != FALSE;
}

bool TestRejectsInvalidUserSids(const BYTE* validSid, DWORD validSidBytes)
{
    std::vector<BYTE> daclStorage;
    std::vector<BYTE> saclStorage;
    SECURITY_DESCRIPTOR descriptor{};
    std::array<BYTE, SECURITY_MAX_SID_SIZE> invalidSid{};
    if (BuildWorkerPipeSecurityDescriptor(nullptr, validSidBytes, daclStorage, saclStorage, descriptor) ||
        BuildWorkerPipeSecurityDescriptor(validSid, 0U, daclStorage, saclStorage, descriptor) ||
        BuildWorkerPipeSecurityDescriptor(validSid, validSidBytes - 1U, daclStorage, saclStorage, descriptor) ||
        BuildWorkerPipeSecurityDescriptor(invalidSid.data(), static_cast<DWORD>(invalidSid.size()), daclStorage, saclStorage, descriptor))
    {
        std::cerr << "invalid user SID input was accepted\n";
        return false;
    }
    return true;
}

bool TestDaclContainsOnlyWorkerAndSystem(const BYTE* userSid, const BYTE* systemSid, PACL dacl)
{
    if (dacl == nullptr || dacl->AceCount != 2U)
    {
        return false;
    }

    bool sawUser = false;
    bool sawSystem = false;
    for (DWORD index = 0U; index < dacl->AceCount; ++index)
    {
        void* rawAce = nullptr;
        if (GetAce(dacl, index, &rawAce) == FALSE)
        {
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            return false;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        const BYTE* sid = reinterpret_cast<const BYTE*>(&ace->SidStart);
        if (ace->Mask != c_pipeAccess)
        {
            return false;
        }

        if (EqualSid(const_cast<BYTE*>(sid), const_cast<BYTE*>(userSid)) != FALSE)
        {
            if (sawUser)
            {
                return false;
            }
            sawUser = true;
        }
        else if (EqualSid(const_cast<BYTE*>(sid), const_cast<BYTE*>(systemSid)) != FALSE)
        {
            if (sawSystem)
            {
                return false;
            }
            sawSystem = true;
        }
        else
        {
            return false;
        }
    }
    return sawUser && sawSystem;
}

bool TestSaclHasMediumNoWriteUpLabel(const BYTE* mediumSid, PACL sacl)
{
    if (sacl == nullptr || sacl->AceCount != 1U)
    {
        return false;
    }

    void* rawAce = nullptr;
    if (GetAce(sacl, 0U, &rawAce) == FALSE)
    {
        return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(rawAce);
    if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
    {
        return false;
    }

    const auto* ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(rawAce);
    const BYTE* sid = reinterpret_cast<const BYTE*>(&ace->SidStart);
    return ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP && EqualSid(const_cast<BYTE*>(sid), const_cast<BYTE*>(mediumSid)) != FALSE;
}

bool TestBuildsRestrictedMediumPipeDescriptor(const BYTE* userSid)
{
    std::array<BYTE, SECURITY_MAX_SID_SIZE> systemSid{};
    DWORD systemSidBytes = static_cast<DWORD>(systemSid.size());
    std::array<BYTE, SECURITY_MAX_SID_SIZE> mediumSid{};
    DWORD mediumSidBytes = static_cast<DWORD>(mediumSid.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &systemSidBytes) == FALSE ||
        CreateWellKnownSid(WinMediumLabelSid, nullptr, mediumSid.data(), &mediumSidBytes) == FALSE)
    {
        std::cerr << "unable to create well-known test SIDs\n";
        return false;
    }

    std::vector<BYTE> daclStorage;
    std::vector<BYTE> saclStorage;
    SECURITY_DESCRIPTOR descriptor{};
    if (!BuildWorkerPipeSecurityDescriptor(userSid, GetLengthSid(const_cast<BYTE*>(userSid)), daclStorage, saclStorage, descriptor))
    {
        std::cerr << "worker pipe security descriptor construction failed\n";
        return false;
    }

    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    BOOL saclPresent = FALSE;
    BOOL saclDefaulted = FALSE;
    PACL sacl = nullptr;
    if (GetSecurityDescriptorDacl(&descriptor, &daclPresent, &dacl, &daclDefaulted) == FALSE ||
        GetSecurityDescriptorSacl(&descriptor, &saclPresent, &sacl, &saclDefaulted) == FALSE || daclPresent == FALSE ||
        saclPresent == FALSE || daclDefaulted != FALSE || saclDefaulted != FALSE ||
        !TestDaclContainsOnlyWorkerAndSystem(userSid, systemSid.data(), dacl) || !TestSaclHasMediumNoWriteUpLabel(mediumSid.data(), sacl))
    {
        std::cerr << "worker pipe descriptor ACLs do not match the expected policy\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    std::array<BYTE, SECURITY_MAX_SID_SIZE> userSid{};
    DWORD userSidBytes = 0U;
    if (!MakeUserSid(userSid, userSidBytes))
    {
        std::cerr << "unable to create the test user SID\n";
        return 1;
    }

    if (!TestRejectsInvalidUserSids(userSid.data(), userSidBytes) || !TestBuildsRestrictedMediumPipeDescriptor(userSid.data()))
    {
        return 1;
    }

    std::cout << "PASS: worker pipe DACL is limited to the user and LocalSystem; "
                 "SACL carries the medium no-write-up label; invalid SIDs are rejected.\n";
    return 0;
}
