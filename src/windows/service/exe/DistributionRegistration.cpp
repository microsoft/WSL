/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DistributionRegistration.cpp

Abstract:

    This file contains the DistributionRegistration helper class implementation.

--*/

#include "precomp.h"

#include "DistributionRegistration.h"

using wsl::windows::service::DistributionRegistration;
using namespace wsl::windows::common;
using namespace registry;

constexpr auto DefaultDistro = L"DefaultDistribution";

namespace {

template <typename T>
T ApplyTransform(T&& value, T (*transform)(T))
{
    if (transform == nullptr)
    {
        return value;
    }

    return transform(std::move(value));
}
} // namespace

DistributionRegistration DistributionRegistration::Open(HKEY LxssKey, const GUID& Id)
{
    ExecutionContext context(Context::ReadDistroConfig);

    const auto distroGuidString = wsl::shared::string::GuidToString<wchar_t>(Id);

    wil::unique_hkey distroKey;
    try
    {
        distroKey = wsl::windows::common::registry::OpenKey(LxssKey, distroGuidString.c_str(), (KEY_READ | KEY_WRITE));
    }
    catch (...)
    {
        THROW_HR_IF(WSL_E_DISTRO_NOT_FOUND, wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        throw;
    }

    return DistributionRegistration(Id, std::move(distroKey));
}

DistributionRegistration DistributionRegistration::Create(
    HKEY LxssKey, const std::optional<GUID>& Id, LPCWSTR Name, ULONG Version, LPCWSTR BasePath, ULONG Flags, ULONG DefaultUID, LPCWSTR PackageFamilyName, LPCWSTR VhdFileName, bool EnableOobe)
{
    std::wstring distroGuidString;
    GUID distroId{};
    wil::unique_hkey distroKey{};
    if (Id.has_value())
    {
        distroId = Id.value();
        distroGuidString = wsl::shared::string::GuidToString<wchar_t>(distroId);
        distroKey = wsl::windows::common::registry::CreateKey(LxssKey, distroGuidString.c_str(), (KEY_READ | KEY_WRITE));
    }
    else
    {
        DWORD disposition = 0;
        do
        {
            THROW_IF_FAILED(CoCreateGuid(&distroId));

            distroGuidString = wsl::shared::string::GuidToString<wchar_t>(distroId);
            distroKey = wsl::windows::common::registry::CreateKey(LxssKey, distroGuidString.c_str(), (KEY_READ | KEY_WRITE), &disposition);
        } while (disposition != REG_CREATED_NEW_KEY);
    }

    WI_ASSERT(distroKey && !distroGuidString.empty());

    // Set up a scope exit member to delete the key if registration fails.
    auto cleanup = wil::scope_exit([&] { wsl::windows::common::registry::DeleteKey(LxssKey, distroGuidString.c_str()); });

    DistributionRegistration distribution(distroId, std::move(distroKey));

    distribution.Write(Property::State, LxssDistributionStateInstalling);

    if (Name != nullptr)
    {
        distribution.Write(Property::Name, Name);
    }
    distribution.Write(Property::Version, Version);
    distribution.Write(Property::BasePath, BasePath);
    distribution.Write(Property::Flags, Flags);
    distribution.Write(Property::Flags, Flags);
    distribution.Write(Property::DefaultUid, DefaultUID);
    distribution.Write(Property::RunOOBE, EnableOobe);

    if (ARGUMENT_PRESENT(PackageFamilyName))
    {
        WI_ASSERT(wcslen(PackageFamilyName) > 0);

        distribution.Write(Property::PackageFamilyName, PackageFamilyName);
    }

    if (ARGUMENT_PRESENT(VhdFileName))
    {
        distribution.Write(Property::VhdFileName, VhdFileName);
    }

    // Dismiss the scope exit member so the key is persisted.
    cleanup.release();
    return distribution;
}

std::optional<DistributionRegistration> DistributionRegistration::OpenDefault(HKEY LxssKey)
{
    const auto defaultId = wsl::windows::common::registry::ReadOptionalString(LxssKey, nullptr, DefaultDistro);
    if (!defaultId.has_value())
    {
        return {};
    }

    const auto distroGuid = wsl::shared::string::ToGuid(defaultId.value());
    if (!distroGuid.has_value())
    {
        return {};
    }

    try
    {
        return Open(LxssKey, distroGuid.value());
    }
    catch (...)
    {
        // If we hit this block, it means that the default distribution value point to a distribution that doesn't exist.
        // Handle gracefully so this doesn't prevent the user from installing new distros.

        LOG_CAUGHT_EXCEPTION_MSG("Broken default distro. ID: %ls", defaultId->c_str());
        return {};
    }
}

DistributionRegistration DistributionRegistration::OpenOrDefault(HKEY LxssKey, const GUID* Id)
{
    if (Id == nullptr)
    {
        auto defaultDistribution = OpenDefault(LxssKey);
        THROW_HR_IF(WSL_E_DEFAULT_DISTRO_NOT_FOUND, !defaultDistribution.has_value());

        return std::move(defaultDistribution.value());
    }
    else
    {
        return Open(LxssKey, *Id);
    }
}

void DistributionRegistration::SetDefault(HKEY LxssKey, const DistributionRegistration& Distro)
{
    wsl::windows::common::registry::WriteString(
        LxssKey, nullptr, DefaultDistro, wsl::shared::string::GuidToString<wchar_t>(Distro.Id()).c_str());
}

void DistributionRegistration::DeleteDefault(HKEY LxssKey)
{
    wsl::windows::common::registry::DeleteKeyValue(LxssKey, DefaultDistro);
}

DistributionRegistration::DistributionRegistration(const GUID& Id, wil::unique_hkey&& key) : m_id(Id), m_key{std::move(key)}
{
}

const GUID& DistributionRegistration::Id() const
{
    return m_id;
}

std::wstring DistributionRegistration::Read(const DistributionPropertyWithDefault<LPCWSTR>& property) const
{
    return ReadString(m_key.get(), nullptr, property.Name, property.DefaultValue);
}

DWORD DistributionRegistration::Read(const DistributionPropertyWithDefault<DWORD>& property) const
{
    return ApplyTransform(ReadDword(m_key.get(), nullptr, property.Name, property.DefaultValue), property.Transform);
}

std::optional<std::wstring> DistributionRegistration::Read(const DistributionProperty<LPCWSTR>& property) const
{
    return ReadOptionalString(m_key.get(), nullptr, property.Name);
}

std::vector<std::string> DistributionRegistration::Read(const DistributionPropertyWithDefault<std::vector<std::string>>& property) const
{
    return wsl::windows::common::registry::ReadStringSet(m_key.get(), nullptr, property.Name, property.DefaultValue);
}

std::wstring DistributionRegistration::Read(const ExpectedProperty<LPCWSTR>& property) const
{
    auto value = Read(static_cast<DistributionProperty<LPCWSTR>>(property));
    if (!value.has_value())
    {
        THROW_HR_WITH_USER_ERROR(
            E_UNEXPECTED,
            wsl::shared::Localization::MessageCorruptedDistroRegistration(
                property.Name, wsl::shared::string::GuidToString<wchar_t>(m_id).c_str()));
    }

    return value.value();
}

void DistributionRegistration::Write(const DistributionProperty<LPCWSTR>& property, LPCWSTR value) const
{
    return WriteString(m_key.get(), nullptr, property.Name, value);
}

void DistributionRegistration::Write(const DistributionProperty<DWORD>& property, DWORD value) const
{
    return WriteDword(m_key.get(), nullptr, property.Name, value);
}

std::filesystem::path DistributionRegistration::ReadVhdFilePath() const
{
    return std::filesystem::path(Read(Property::BasePath)) / Read(Property::VhdFileName);
}

DWORD
DistributionRegistration::ApplyGlobalFlagsOverride(DWORD Flags)
{
    WI_ASSERT(!WI_IsAnyFlagSet(Flags, ~LXSS_DISTRO_FLAGS_ALL));

    DWORD globalFlags =
        wsl::windows::common::registry::ReadDword(HKEY_LOCAL_MACHINE, LXSS_SERVICE_REGISTRY_PATH, L"DistributionFlags", LXSS_DISTRO_FLAGS_ALL);

    // The VM Mode flag cannot be overridden by global flags.
    WI_SetFlag(globalFlags, LXSS_DISTRO_FLAGS_VM_MODE);
    Flags &= (globalFlags & LXSS_DISTRO_FLAGS_ALL);
    return Flags;
}

void DistributionRegistration::Delete(HKEY LxssKey) const
{
    DeleteKey(LxssKey, wsl::shared::string::GuidToString<wchar_t>(m_id).c_str());
}