/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DistributionRegistration.h

Abstract:

    This file contains the DistributionRegistration helper class definition.

--*/

#pragma once

namespace wsl::windows::service {

template <typename T>
struct DistributionProperty
{
    LPCWSTR Name;
    T (*Transform)(T) = nullptr;
};

template <typename T>
struct DistributionPropertyWithDefault : public DistributionProperty<T>
{
    DistributionPropertyWithDefault(LPCWSTR Name, T DefaultValue, T (*Transform)(T) = nullptr) :
        DistributionProperty<T>(Name, Transform), DefaultValue(DefaultValue)
    {
    }

    T DefaultValue;
};

template <typename T>
struct ExpectedProperty : public DistributionProperty<T>
{
    ExpectedProperty(LPCWSTR Name, T (*Transform)(T) = nullptr) : DistributionProperty<T>(Name, Transform)
    {
    }
};

class DistributionRegistration
{
public:
    static DistributionRegistration Create(
        HKEY LxssKey,
        const std::optional<GUID>& Id,
        LPCWSTR Name,
        ULONG Version,
        LPCWSTR BasePath,
        ULONG Flags,
        ULONG DefaultUID,
        LPCWSTR PackageFamilyName,
        LPCWSTR VhdFileName,
        bool EnableOobe);

    static DistributionRegistration Open(HKEY LxssKey, const GUID& Id);
    static DistributionRegistration OpenOrDefault(HKEY LxssKey, const GUID* Id);
    static std::optional<DistributionRegistration> OpenDefault(HKEY LxssKey);
    static void SetDefault(HKEY LxssKey, const DistributionRegistration& Distro);
    static void DeleteDefault(HKEY LxssKey);

    DistributionRegistration() = default;
    const GUID& Id() const;

    std::wstring Read(const DistributionPropertyWithDefault<LPCWSTR>& property) const;
    std::optional<std::wstring> Read(const DistributionProperty<LPCWSTR>& property) const;
    DWORD Read(const DistributionPropertyWithDefault<DWORD>& property) const;
    std::vector<std::string> Read(const DistributionPropertyWithDefault<std::vector<std::string>>& Property) const;
    std::wstring Read(const ExpectedProperty<LPCWSTR>& property) const;

    void Write(const DistributionProperty<LPCWSTR>& property, LPCWSTR Value) const;
    void Write(const DistributionProperty<DWORD>& property, DWORD Value) const;
    void Delete(HKEY LxssKey) const;

    std::filesystem::path ReadVhdFilePath() const;

    static DWORD ApplyGlobalFlagsOverride(DWORD Flags);

private:
    DistributionRegistration(const GUID& Id, wil::unique_hkey&& key);

    GUID m_id{};
    wil::unique_hkey m_key;
};

namespace Property {
    inline DistributionPropertyWithDefault<LPCWSTR> PackageFamilyName{L"PackageFamilyName", L""};
    inline DistributionPropertyWithDefault<LPCWSTR> KernelCommandLine{L"KernelCommandLine", L""};
    inline DistributionPropertyWithDefault<LPCWSTR> VhdFileName{L"VhdFileName", LXSS_VM_MODE_VHD_NAME};
    inline ExpectedProperty<LPCWSTR> Name{L"DistributionName"};
    inline ExpectedProperty<LPCWSTR> BasePath{L"BasePath"};
    inline DistributionProperty<LPCWSTR> Flavor{L"Flavor"};
    inline DistributionProperty<LPCWSTR> OsVersion{L"OsVersion"};
    inline DistributionProperty<LPCWSTR> ShortcutPath{L"ShortcutPath"};
    inline DistributionProperty<LPCWSTR> TerminalProfilePath{L"TerminalProfilePath"};

    inline DistributionPropertyWithDefault<DWORD> Version{L"Version", LXSS_DISTRO_VERSION_CURRENT};
    inline DistributionPropertyWithDefault<DWORD> Flags{L"Flags", LXSS_DISTRO_FLAGS_DEFAULT, &DistributionRegistration::ApplyGlobalFlagsOverride};
    inline DistributionPropertyWithDefault<DWORD> DefaultUid{L"DefaultUid", LX_UID_ROOT};
    inline DistributionPropertyWithDefault<DWORD> State{L"State", LxssDistributionStateInvalid};
    inline DistributionPropertyWithDefault<DWORD> RunOOBE{L"RunOOBE", 0};
    inline DistributionPropertyWithDefault<DWORD> Modern{L"Modern", 0};

    inline DistributionPropertyWithDefault<std::vector<std::string>> DefaultEnvironment{
        L"DefaultEnvironment",
        {"HOSTTYPE=x86_64",
         "LANG=en_US.UTF-8",
         "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games",
         "TERM=xterm-256color"}};

} // namespace Property

} // namespace wsl::windows::service
