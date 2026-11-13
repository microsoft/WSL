/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIMountParserUnitTests.cpp

Abstract:

    Unit tests for Docker-compatible --mount parsing.

--*/

#include "precomp.h"
#include "Common.h"
#include "WSLCCLITestHelpers.h"
#include "ContainerModel.h"
#include "MountSpecParsing.h"

using namespace wsl::windows::common;
using namespace wsl::windows::wslc::models;
using namespace wsl::shared;
using namespace WEX::Logging;
using namespace WEX::Common;

namespace WSLCCLIMountParserUnitTests {

namespace {

    mount::Spec ParseAndValidate(const std::wstring& value)
    {
        auto mountSpec = mount::ParseDockerMountString(value);
        mount::ValidateMountSpec(mountSpec);
        return mountSpec;
    }

    struct ValidMountCase
    {
        const wchar_t* Input;
        mount::Type Type;
        const wchar_t* Source;
        const char* Target;
        bool ReadOnly;
        std::optional<int64_t> TmpfsSizeBytes;
        std::optional<uint32_t> TmpfsMode;
        const char* TmpfsOptions;
    };

    enum class ExpectedException
    {
        Parse,
        Unsupported,
        Validation,
    };

    struct InvalidMountCase
    {
        const wchar_t* Input;
        std::wstring ExpectedReason;
        ExpectedException Exception = ExpectedException::Parse;
    };

    constexpr ValidMountCase c_validMountCases[] = {
        {L"type=volume,target=/data", WSLCMountTypeVolume, L"", "/data", false, {}, {}, ""},
        {L"source=data-volume,target=/data", WSLCMountTypeVolume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/path:voldir",
         WSLCMountTypeVolume,
         L"data-volume",
         "/path:voldir",
         false,
         {},
         {},
         ""},
        {L"TYPE=VOLUME,SOURCE=data-volume,TARGET=/data", WSLCMountTypeVolume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=VoLuMe,source=data-volume,target=/data", WSLCMountTypeVolume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,src=data-volume,dst=/data", WSLCMountTypeVolume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,src=data-volume,destination=/data", WSLCMountTypeVolume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,source=first,source=second,target=/data", WSLCMountTypeVolume, L"second", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/first,target=/second",
         WSLCMountTypeVolume,
         L"data-volume",
         "/second",
         false,
         {},
         {},
         ""},
        {L"type=volume,type=bind,source=C:\\data,target=/data", WSLCMountTypeBind, L"C:\\data", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,readonly", WSLCMountTypeVolume, L"data-volume", "/data", true, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,ro", WSLCMountTypeVolume, L"data-volume", "/data", true, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,readonly=1",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=t",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=T",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=TRUE",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=true",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=True",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=0",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=f",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=F",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=FALSE",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=false",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=False",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=true,readonly=false",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,bind-recursive=enabled",
         WSLCMountTypeVolume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=bind,\"source=C:\\mount,a\",target=/data", WSLCMountTypeBind, L"C:\\mount,a", "/data", false, {}, {}, ""},
        {L"type=bind,source=C:\\mount with spaces,target=/data",
         WSLCMountTypeBind,
         L"C:\\mount with spaces",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=bind,source=C:\\mount,target=/path:mntdir", WSLCMountTypeBind, L"C:\\mount", "/path:mntdir", false, {}, {}, ""},
        {L"type=bind,source=C:\\,target=/data", WSLCMountTypeBind, L"C:\\", "/data", false, {}, {}, ""},
        {L"type=bind,source=\\\\server\\share,target=/data", WSLCMountTypeBind, L"\\\\server\\share", "/data", false, {}, {}, ""},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=enabled",
         WSLCMountTypeBind,
         L"C:\\mount",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=A_,target=/data", WSLCMountTypeVolume, L"A_", "/data", false, {}, {}, ""},
        {L"type=volume,source=data.volume-1,target=/data", WSLCMountTypeVolume, L"data.volume-1", "/data", false, {}, {}, ""},
        {L"type=tmpfs,target=/tmp", WSLCMountTypeTmpfs, L"", "/tmp", false, {}, {}, ""},
        {L"type=tmpfs,target=/path:tmpfs", WSLCMountTypeTmpfs, L"", "/path:tmpfs", false, {}, {}, ""},
        {L"type=tmpfs,target=/tmp,readonly", WSLCMountTypeTmpfs, L"", "/tmp", true, {}, {}, "ro"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=0", WSLCMountTypeTmpfs, L"", "/tmp", false, 0, {}, ""},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1", WSLCMountTypeTmpfs, L"", "/tmp", false, 1, {}, "size=1"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1024", WSLCMountTypeTmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1536", WSLCMountTypeTmpfs, L"", "/tmp", false, 1536, {}, "size=1536"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1k", WSLCMountTypeTmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1KB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1KiB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MiB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1GB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1LL << 30, {}, "size=1g"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1.5MB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1536LL << 10, {}, "size=1536k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=+1MB", WSLCMountTypeTmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1e3", WSLCMountTypeTmpfs, L"", "/tmp", false, 1000, {}, "size=1000"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0000", WSLCMountTypeTmpfs, L"", "/tmp", false, {}, 0, ""},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0700", WSLCMountTypeTmpfs, L"", "/tmp", false, {}, 0700, "mode=700"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MB,tmpfs-mode=0700,readonly",
         WSLCMountTypeTmpfs,
         L"",
         "/tmp",
         true,
         1LL << 20,
         0700,
         "ro,mode=700,size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=0,tmpfs-mode=0000,readonly=false", WSLCMountTypeTmpfs, L"", "/tmp", false, 0, 0, ""},
    };

    const InvalidMountCase c_invalidMountCases[] = {
        {L"", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"")},
        {L",", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"")},
        {L"type=volume,source=data-volume,target=/data,", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"")},
        {L",type=volume,source=data-volume,target=/data", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"")},
        {L"type=bind,\"source=C:\\mount,target=/data", Localization::WSLCCLI_MountMalformedCsvError()},
        {L"type=volume,bogus", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"bogus")},
        {L"type=volume,bogus=value", Localization::WSLCCLI_MountUnexpectedKeyError(L"bogus", L"bogus=value")},
        {L"type", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"type")},
        {L"source", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"source")},
        {L"target", Localization::WSLCCLI_MountFieldKeyValueRequiredError(L"target")},
        {L"type=,source=data-volume,target=/data", Localization::WSLCCLI_MountTypeRequiredError()},
        {L"type=volume,source=data-volume", Localization::WSLCCLI_MountTargetRequiredError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,target=", Localization::WSLCCLI_MountTargetRequiredError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,dst=", Localization::WSLCCLI_MountTargetRequiredError(), ExpectedException::Validation},
        {L"type=cluster,source=data-volume,target=/data", Localization::WSLCCLI_MountTypeUnsupportedError(L"cluster"), ExpectedException::Unsupported},
        {L"type=npipe,source=data-volume,target=/data", Localization::WSLCCLI_MountTypeUnsupportedError(L"npipe"), ExpectedException::Unsupported},
        {L"type=bogus,source=data-volume,target=/data", Localization::WSLCCLI_MountTypeUnsupportedError(L"bogus"), ExpectedException::Unsupported},
        {L"type=CLUSTER,source=data-volume,target=/data", Localization::WSLCCLI_MountTypeUnsupportedError(L"cluster"), ExpectedException::Unsupported},
        {L"type=bind,target=/data", Localization::WSLCCLI_MountSourceRequiredError(), ExpectedException::Validation},
        {L"type=bind,source=relative,target=/data", Localization::WSLCCLI_MountBindSourceAbsoluteError(), ExpectedException::Validation},
        {L"type=volume,source=a,target=/data", Localization::WSLCCLI_MountVolumeSourceInvalidError(), ExpectedException::Validation},
        {L"type=volume,source=data/volume,target=/data", Localization::WSLCCLI_MountVolumeSourceInvalidError(), ExpectedException::Validation},
        {L"type=volume,source=C:\\mount,target=/data", Localization::WSLCCLI_MountVolumeSourceInvalidError(), ExpectedException::Validation},
        {L"type=tmpfs,source=data-volume,target=/data", Localization::WSLCCLI_MountTmpfsSourceUnsupportedError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,target=data", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,dst=.", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,destination=\\data", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"type=bind,source=C:\\mount,target=data", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"type=tmpfs,target=data", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"type=volume,source=data-volume,target=/data,readonly=no",
         Localization::WSLCCLI_MountInvalidValueError(L"readonly", L"no")},
        {L"type=volume,source=data-volume,target=/data,readonly=yes",
         Localization::WSLCCLI_MountInvalidValueError(L"readonly", L"yes")},
        {L"type=volume,source=data-volume,target=/data,readonly=", Localization::WSLCCLI_MountInvalidValueError(L"readonly", L"")},
        {L"type=volume,source=data-volume,target=/data,readonly=2",
         Localization::WSLCCLI_MountInvalidValueError(L"readonly", L"2")},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=no",
         Localization::WSLCCLI_MountInvalidValueError(L"volume-nocopy", L"no")},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=",
         Localization::WSLCCLI_MountInvalidValueError(L"volume-nocopy", L"")},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive=no",
         Localization::WSLCCLI_MountInvalidValueError(L"bind-nonrecursive", L"no")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=",
         Localization::WSLCCLI_MountInvalidBindRecursiveValueError(L"bind-recursive", L"")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=Enabled",
         Localization::WSLCCLI_MountInvalidBindRecursiveValueError(L"bind-recursive", L"Enabled")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=bogus",
         Localization::WSLCCLI_MountInvalidBindRecursiveValueError(L"bind-recursive", L"bogus")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=writable",
         Localization::WSLCCLI_MountOptionRequiresReadonlyError(L"bind-recursive=writable")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly",
         Localization::WSLCCLI_MountOptionRequiresReadonlyError(L"bind-recursive=readonly")},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly,readonly",
         Localization::WSLCCLI_MountBindRecursiveReadonlyRequiresPropagationError()},
        {L"type=bind,source=C:\\mount,target=/data,consistency=cached",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"consistency"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-propagation=rprivate",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-propagation"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-nonrecursive"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive=true",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-nonrecursive"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=disabled",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-recursive"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=writable,readonly",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-recursive"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly,readonly,bind-propagation=rprivate",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"bind-recursive"),
         ExpectedException::Unsupported},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"volume-nocopy"),
         ExpectedException::Unsupported},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=true",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"volume-nocopy"),
         ExpectedException::Unsupported},
        {L"type=volume,source=data-volume,target=/data,volume-label=a=b",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"volume-label"),
         ExpectedException::Unsupported},
        {L"type=volume,source=data-volume,target=/data,volume-driver=local",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"volume-driver"),
         ExpectedException::Unsupported},
        {L"type=volume,source=data-volume,target=/data,volume-opt=a=b",
         Localization::WSLCCLI_MountOptionUnsupportedError(L"volume-opt"),
         ExpectedException::Unsupported},
        {L"type=bind,source=C:\\mount,target=/data,volume-nocopy=true",
         Localization::WSLCCLI_MountOptionFamilyMismatchError(L"volume-*", L"bind")},
        {L"type=volume,source=data-volume,target=/data,bind-propagation=rprivate",
         Localization::WSLCCLI_MountOptionFamilyMismatchError(L"bind-*", L"volume")},
        {L"type=volume,source=data-volume,target=/data,tmpfs-size=1m",
         Localization::WSLCCLI_MountOptionFamilyMismatchError(L"tmpfs-*", L"volume")},
        {L"type=tmpfs,target=/tmp,volume-label=a=b", Localization::WSLCCLI_MountOptionFamilyMismatchError(L"volume-*", L"tmpfs")},
        {L"type=tmpfs,target=/tmp,bind-nonrecursive", Localization::WSLCCLI_MountOptionFamilyMismatchError(L"bind-*", L"tmpfs")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=bad", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"bad")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=-1", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"-1")},
        {L"type=tmpfs,target=/tmp,\"tmpfs-size=1,5MB\"", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"1,5MB")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1XB", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"1XB")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1Ki", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"1Ki")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1BB", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"1BB")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=nan", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"nan")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=inf", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"inf")},
        {L"type=tmpfs,target=/tmp,tmpfs-size=9223372036854775808",
         Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-size", L"9223372036854775808")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=+0700", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"+0700")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=-1", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"-1")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=8", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"8")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0899", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"0899")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0x700", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"0x700")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=40000000000",
         Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"40000000000")},
    };

    mount::Spec ParseAndValidateVolume(const std::wstring& value)
    {
        auto mountSpec = mount::ParseDockerVolumeString(value);
        mount::ValidateMountSpec(mountSpec);
        return mountSpec;
    }

    void VerifyExceptionType(const mount::MountException& ex, ExpectedException expected)
    {
        switch (expected)
        {
        case ExpectedException::Parse:
            VERIFY_IS_TRUE(dynamic_cast<const mount::MountParseException*>(&ex) != nullptr);
            break;

        case ExpectedException::Unsupported:
            VERIFY_IS_TRUE(dynamic_cast<const mount::MountUnsupportedException*>(&ex) != nullptr);
            break;

        case ExpectedException::Validation:
            VERIFY_IS_TRUE(dynamic_cast<const mount::MountValidationException*>(&ex) != nullptr);
            break;
        }
    }

    struct ValidVolumeCase
    {
        const wchar_t* Input;
        mount::Type Type;
        const wchar_t* Source;
        const char* Target;
        bool ReadOnly;
    };

    constexpr ValidVolumeCase c_validVolumeCases[] = {
        {L"C:\\hostPath:/data", WSLCMountTypeBind, L"C:\\hostPath", "/data", false},
        {L"C:\\hostPath:/data:ro", WSLCMountTypeBind, L"C:\\hostPath", "/data", true},
        {L"C:\\hostPath:/data:rw", WSLCMountTypeBind, L"C:\\hostPath", "/data", false},
        {L"C:/hostPath:/data", WSLCMountTypeBind, L"C:\\hostPath", "/data", false},
        {L"C:\\host Path:/container Path:ro", WSLCMountTypeBind, L"C:\\host Path", "/container Path", true},
        {L"named-volume:/data", WSLCMountTypeVolume, L"named-volume", "/data", false},
        {L"named-volume:/data:ro", WSLCMountTypeVolume, L"named-volume", "/data", true},
        {L"/data", WSLCMountTypeVolume, L"", "/data", false},
        {L":", WSLCMountTypeVolume, L"", ":", false},
        {L"::", WSLCMountTypeVolume, L"", "::", false},
        {L"ab", WSLCMountTypeVolume, L"", "ab", false},
        {L"C:", WSLCMountTypeVolume, L"", "C:", false},
        {L"C:\\test", WSLCMountTypeVolume, L"", "C:\\test", false},
        {L"\u00e9:\\test", WSLCMountTypeVolume, L"", "\xc3\xa9:\\test", false},
        {L"\u00e9:", WSLCMountTypeVolume, L"", "\xc3\xa9:", false},
    };

    const InvalidMountCase c_invalidVolumeCases[] = {
        {L"", Localization::WSLCCLI_VolumeEmptySpecError()},
        {L":/data", Localization::WSLCCLI_VolumeEmptySectionError()},
        {L":\u00e9", Localization::WSLCCLI_VolumeEmptySectionError()},
        {L"::/data:ro", Localization::WSLCCLI_VolumeEmptySectionError()},
        {L"C:\\hostPath::ro", Localization::WSLCCLI_VolumeEmptySectionError()},
        {L"C:\\hostPath:/data:", Localization::WSLCCLI_VolumeEmptySectionError()},
        {L"C:\\hostPath:/data:ro:extra", Localization::WSLCCLI_VolumeTooManyColonsError()},
        {L"named-volume:/data:z:ro", Localization::WSLCCLI_VolumeTooManyColonsError()},
        {L"C:\\bad|name:/data", Localization::WSLCCLI_VolumeHostPathInvalidError(L"C:\\bad|name")},
        {L"C:\\hostPath:ro", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"C:\\hostPath:C:\\data", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"/data:ro", Localization::WSLCCLI_MountTargetAbsoluteError(), ExpectedException::Validation},
        {L"sub/dir:/data", Localization::WSLCCLI_MountVolumeSourceInvalidError(), ExpectedException::Validation},
        {L"sub\\dir:/data", Localization::WSLCCLI_MountVolumeSourceInvalidError(), ExpectedException::Validation},
        {L"C:\\hostPath:/data:bogus", Localization::WSLCCLI_MountInvalidModeError(L"bogus"), ExpectedException::Validation},
        {L"C:\\hostPath:/data:ro,", Localization::WSLCCLI_MountInvalidModeError(L""), ExpectedException::Validation},
        {L"C:\\hostPath:/data:ro,rw", Localization::WSLCCLI_MountInvalidModeError(L"rw"), ExpectedException::Validation},
        {L"C:\\hostPath:/data:z,Z", Localization::WSLCCLI_MountInvalidModeError(L"Z"), ExpectedException::Validation},
        {L"C:\\hostPath:/data:z", Localization::WSLCCLI_MountOptionUnsupportedError(L"z"), ExpectedException::Unsupported},
        {L"named-volume:/data:nocopy", Localization::WSLCCLI_MountOptionUnsupportedError(L"nocopy"), ExpectedException::Unsupported},
        {L"C:\\hostPath:/data:ro,rshared", Localization::WSLCCLI_MountOptionUnsupportedError(L"rshared"), ExpectedException::Unsupported},
    };

} // namespace

class WSLCCLIMountParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIMountParserUnitTests)

    TEST_METHOD(Mount_ValidCases)
    {
        for (const auto& testCase : c_validMountCases)
        {
            Log::Comment(String().Format(L"Accepting: %ls", testCase.Input));

            const auto actual = ParseAndValidate(testCase.Input);
            VERIFY_ARE_EQUAL(static_cast<int>(testCase.Type), static_cast<int>(actual.MountType));
            VERIFY_ARE_EQUAL(std::wstring(testCase.Source), actual.Source);
            VERIFY_ARE_EQUAL(std::string(testCase.Target), actual.Target);
            VERIFY_ARE_EQUAL(testCase.ReadOnly, actual.ReadOnly);
            VERIFY_ARE_EQUAL(testCase.TmpfsSizeBytes.has_value(), actual.TmpfsSizeBytes.has_value());
            if (testCase.TmpfsSizeBytes.has_value() && actual.TmpfsSizeBytes.has_value())
            {
                VERIFY_ARE_EQUAL(testCase.TmpfsSizeBytes.value(), actual.TmpfsSizeBytes.value());
            }

            VERIFY_ARE_EQUAL(testCase.TmpfsMode.has_value(), actual.TmpfsMode.has_value());
            if (testCase.TmpfsMode.has_value() && actual.TmpfsMode.has_value())
            {
                VERIFY_ARE_EQUAL(testCase.TmpfsMode.value(), actual.TmpfsMode.value());
            }

            const auto actualTmpfsOptions = actual.MountType == WSLCMountTypeTmpfs ? mount::FormatTmpfsOptions(actual) : std::string{};
            VERIFY_ARE_EQUAL(std::string(testCase.TmpfsOptions), actualTmpfsOptions);
        }
    }

    TEST_METHOD(Mount_InvalidCases)
    {
        for (const auto& testCase : c_invalidMountCases)
        {
            Log::Comment(String().Format(L"Rejecting: %ls", testCase.Input));

            try
            {
                (void)ParseAndValidate(testCase.Input);
                VERIFY_FAIL(L"Expected MountException for invalid mount spec");
            }
            catch (const mount::MountException& ex)
            {
                VERIFY_ARE_EQUAL(testCase.ExpectedReason, ex.Reason());
                VerifyExceptionType(ex, testCase.Exception);
            }
        }
    }

    TEST_METHOD(Volume_ValidCases)
    {
        for (const auto& testCase : c_validVolumeCases)
        {
            Log::Comment(String().Format(L"Accepting: %ls", testCase.Input));

            const auto actual = ParseAndValidateVolume(testCase.Input);
            VERIFY_ARE_EQUAL(static_cast<int>(testCase.Type), static_cast<int>(actual.MountType));
            VERIFY_ARE_EQUAL(std::wstring(testCase.Source), actual.Source);
            VERIFY_ARE_EQUAL(std::string(testCase.Target), actual.Target);
            VERIFY_ARE_EQUAL(testCase.ReadOnly, actual.ReadOnly);

            const auto expectedPolicy = testCase.Type == WSLCMountTypeBind ? mount::BindSourcePolicy::CreateIfMissing
                                                                           : mount::BindSourcePolicy::RequireExisting;
            VERIFY_ARE_EQUAL(static_cast<int>(expectedPolicy), static_cast<int>(actual.BindSource));
            VERIFY_ARE_EQUAL(testCase.Type == WSLCMountTypeVolume && testCase.Source[0] == L'\0', actual.ConfigVolume);
        }
    }

    TEST_METHOD(Volume_InvalidCases)
    {
        for (const auto& testCase : c_invalidVolumeCases)
        {
            Log::Comment(String().Format(L"Rejecting: %ls", testCase.Input));

            try
            {
                (void)ParseAndValidateVolume(testCase.Input);
                VERIFY_FAIL(L"Expected MountException for invalid volume spec");
            }
            catch (const mount::MountException& ex)
            {
                VERIFY_ARE_EQUAL(testCase.ExpectedReason, ex.Reason());
                VerifyExceptionType(ex, testCase.Exception);
            }
        }
    }

    TEST_METHOD(Volume_AnonymousVolumesSkipDuplicateDestinationCheck)
    {
        const std::vector<mount::Spec> mounts{
            ParseAndValidateVolume(L"/data"),
            ParseAndValidateVolume(L"/data"),
            ParseAndValidateVolume(L"named-volume:/data"),
        };

        mount::ValidateMountCollection(mounts);
    }

    TEST_METHOD(Volume_RelativeBindSourceUsesCurrentDirectory)
    {
        const auto actual = ParseAndValidateVolume(L".\\mount:/data");
        VERIFY_ARE_EQUAL(static_cast<int>(WSLCMountTypeBind), static_cast<int>(actual.MountType));
        VERIFY_ARE_EQUAL(std::filesystem::weakly_canonical(std::filesystem::current_path() / L"mount").wstring(), actual.Source);
    }

    TEST_METHOD(Volume_ModesArePreserved)
    {
        const auto actual = mount::ParseDockerVolumeString(L"named-volume:/data:ro,z,nocopy");
        const std::vector<std::wstring> expected{L"ro", L"z", L"nocopy"};
        VERIFY_ARE_EQUAL(expected.size(), actual.Modes.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            VERIFY_ARE_EQUAL(expected[index], actual.Modes[index]);
        }

        VERIFY_IS_TRUE(actual.ReadOnly);
    }

    TEST_METHOD(Mount_DotRelativeBindSourceUsesCurrentDirectory)
    {
        const auto expected = (std::filesystem::current_path() / L"mount").lexically_normal().wstring();
        const auto actual = ParseAndValidate(L"type=bind,source=.\\mount,target=/data");
        VERIFY_ARE_EQUAL(expected, actual.Source);
    }

    TEST_METHOD(Mount_TypedSpecsAreValidated)
    {
        const mount::Spec relativeBind{
            .MountType = WSLCMountTypeBind,
            .Source = L"relative",
            .Target = "/data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(relativeBind), mount::MountValidationException);

        const mount::Spec relativeTarget{
            .MountType = WSLCMountTypeVolume,
            .Source = L"data-volume",
            .Target = "data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(relativeTarget), mount::MountValidationException);

        const mount::Spec tmpfsWithSource{
            .MountType = WSLCMountTypeTmpfs,
            .Source = L"data-volume",
            .Target = "/data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(tmpfsWithSource), mount::MountValidationException);

        const mount::Spec bindWithTmpfsOptions{
            .MountType = WSLCMountTypeBind,
            .Source = L"C:\\data",
            .Target = "/data",
            .TmpfsSizeBytes = 1024,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(bindWithTmpfsOptions), mount::MountValidationException);

        const mount::Spec negativeTmpfsSize{
            .MountType = WSLCMountTypeTmpfs,
            .Target = "/data",
            .TmpfsSizeBytes = -1,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(negativeTmpfsSize), mount::MountValidationException);

        const mount::Spec tmpfs{
            .MountType = WSLCMountTypeTmpfs,
            .Target = "/data",
            .TmpfsSizeBytes = 1024,
            .TmpfsMode = 0700,
        };
        VERIFY_NO_THROW(mount::ValidateMountSpec(tmpfs));

        const mount::Spec duplicateMounts[] = {
            {.MountType = WSLCMountTypeTmpfs, .Target = "/data"},
            {.MountType = WSLCMountTypeVolume, .Source = L"data-volume", .Target = "/data/"},
        };
        try
        {
            mount::ValidateMountCollection(duplicateMounts);
            VERIFY_FAIL(L"Expected MountValidationException for duplicate destinations");
        }
        catch (const mount::MountValidationException& ex)
        {
            VERIFY_ARE_EQUAL(static_cast<int>(mount::ValidationError::DuplicateDestination), static_cast<int>(ex.Error()));
            VERIFY_ARE_EQUAL(std::string("/data"), ex.Destination());
        }

        const mount::Spec distinctBackslashMounts[] = {
            {.MountType = WSLCMountTypeTmpfs, .Target = "/data\\cache"},
            {.MountType = WSLCMountTypeVolume, .Source = L"data-volume", .Target = "/data/cache"},
        };
        VERIFY_NO_THROW(mount::ValidateMountCollection(distinctBackslashMounts));
    }
};

} // namespace WSLCCLIMountParserUnitTests
