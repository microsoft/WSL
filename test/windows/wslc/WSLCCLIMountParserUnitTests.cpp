/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIMountParserUnitTests.cpp

Abstract:

    Unit tests for Docker-compatible --mount parsing.

--*/

#include "precomp.h"
#include "windows/Common.h"
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
        {L"type=volume,target=/data", mount::Type::Volume, L"", "/data", false, {}, {}, ""},
        {L"source=data-volume,target=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/path:voldir",
         mount::Type::Volume,
         L"data-volume",
         "/path:voldir",
         false,
         {},
         {},
         ""},
        {L"TYPE=VOLUME,SOURCE=data-volume,TARGET=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=VoLuMe,source=data-volume,target=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,src=data-volume,dst=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,src=data-volume,destination=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
        {L"type=volume,source=first,source=second,target=/data", mount::Type::Volume, L"second", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/first,target=/second",
         mount::Type::Volume,
         L"data-volume",
         "/second",
         false,
         {},
         {},
         ""},
        {L"type=volume,type=bind,source=C:\\data,target=/data", mount::Type::Bind, L"C:\\data", "/data", false, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,readonly", mount::Type::Volume, L"data-volume", "/data", true, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,ro", mount::Type::Volume, L"data-volume", "/data", true, {}, {}, ""},
        {L"type=volume,source=data-volume,target=/data,readonly=1",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=t",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=T",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=TRUE",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=true",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=True",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         true,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=0",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=f",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=F",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=FALSE",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=false",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=False",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,readonly=true,readonly=false",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=bind,\"source=C:\\mount,a\",target=/data", mount::Type::Bind, L"C:\\mount,a", "/data", false, {}, {}, ""},
        {L"type=bind,source=C:\\mount with spaces,target=/data",
         mount::Type::Bind,
         L"C:\\mount with spaces",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=bind,source=C:\\mount,target=/path:mntdir", mount::Type::Bind, L"C:\\mount", "/path:mntdir", false, {}, {}, ""},
        {L"type=bind,source=C:\\,target=/data", mount::Type::Bind, L"C:\\", "/data", false, {}, {}, ""},
        {L"type=bind,source=\\\\server\\share,target=/data", mount::Type::Bind, L"\\\\server\\share", "/data", false, {}, {}, ""},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=enabled",
         mount::Type::Bind,
         L"C:\\mount",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=data-volume,target=/data,bind-recursive=enabled",
         mount::Type::Volume,
         L"data-volume",
         "/data",
         false,
         {},
         {},
         ""},
        {L"type=volume,source=A_,target=/data", mount::Type::Volume, L"A_", "/data", false, {}, {}, ""},
        {L"type=volume,source=data.volume-1,target=/data", mount::Type::Volume, L"data.volume-1", "/data", false, {}, {}, ""},
        {L"type=tmpfs,target=/tmp", mount::Type::Tmpfs, L"", "/tmp", false, {}, {}, ""},
        {L"type=tmpfs,target=/path:tmpfs", mount::Type::Tmpfs, L"", "/path:tmpfs", false, {}, {}, ""},
        {L"type=tmpfs,target=/tmp,readonly", mount::Type::Tmpfs, L"", "/tmp", true, {}, {}, "ro"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=0", mount::Type::Tmpfs, L"", "/tmp", false, 0, {}, ""},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1", mount::Type::Tmpfs, L"", "/tmp", false, 1, {}, "size=1"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1024", mount::Type::Tmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1536", mount::Type::Tmpfs, L"", "/tmp", false, 1536, {}, "size=1536"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1k", mount::Type::Tmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1KB", mount::Type::Tmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1KiB", mount::Type::Tmpfs, L"", "/tmp", false, 1024, {}, "size=1k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MB", mount::Type::Tmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MiB", mount::Type::Tmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1GB", mount::Type::Tmpfs, L"", "/tmp", false, 1LL << 30, {}, "size=1g"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1.5MB", mount::Type::Tmpfs, L"", "/tmp", false, 1536LL << 10, {}, "size=1536k"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=+1MB", mount::Type::Tmpfs, L"", "/tmp", false, 1LL << 20, {}, "size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1e3", mount::Type::Tmpfs, L"", "/tmp", false, 1000, {}, "size=1000"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0000", mount::Type::Tmpfs, L"", "/tmp", false, {}, 0, ""},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0700", mount::Type::Tmpfs, L"", "/tmp", false, {}, 0700, "mode=700"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=+0700", mount::Type::Tmpfs, L"", "/tmp", false, {}, 0700, "mode=700"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1MB,tmpfs-mode=0700,readonly",
         mount::Type::Tmpfs,
         L"",
         "/tmp",
         true,
         1LL << 20,
         0700,
         "ro,mode=700,size=1m"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=0,tmpfs-mode=0000,readonly=false", mount::Type::Tmpfs, L"", "/tmp", false, 0, 0, ""},
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
        {L"type=volume,source=data-volume,target=/data,bind-recursive=enabled",
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
        {L"type=tmpfs,target=/tmp,tmpfs-mode=-1", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"-1")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=8", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"8")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0899", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"0899")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0x700", Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"0x700")},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=40000000000",
         Localization::WSLCCLI_MountInvalidValueError(L"tmpfs-mode", L"40000000000")},
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

            const auto actualTmpfsOptions = actual.MountType == mount::Type::Tmpfs ? mount::FormatTmpfsOptions(actual) : std::string{};
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
                switch (testCase.Exception)
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
        }
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
            .MountType = mount::Type::Bind,
            .Source = L"relative",
            .Target = "/data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(relativeBind), mount::MountValidationException);

        const mount::Spec relativeTarget{
            .MountType = mount::Type::Volume,
            .Source = L"data-volume",
            .Target = "data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(relativeTarget), mount::MountValidationException);

        const mount::Spec tmpfsWithSource{
            .MountType = mount::Type::Tmpfs,
            .Source = L"data-volume",
            .Target = "/data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(tmpfsWithSource), mount::MountValidationException);

        const mount::Spec bindWithTmpfsOptions{
            .MountType = mount::Type::Bind,
            .Source = L"C:\\data",
            .Target = "/data",
            .TmpfsSizeBytes = 1024,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(bindWithTmpfsOptions), mount::MountValidationException);

        const mount::Spec negativeTmpfsSize{
            .MountType = mount::Type::Tmpfs,
            .Target = "/data",
            .TmpfsSizeBytes = -1,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(negativeTmpfsSize), mount::MountValidationException);

        const mount::Spec tmpfs{
            .MountType = mount::Type::Tmpfs,
            .Target = "/data",
            .TmpfsSizeBytes = 1024,
            .TmpfsMode = 0700,
        };
        VERIFY_NO_THROW(mount::ValidateMountSpec(tmpfs));

        const mount::Spec duplicateMounts[] = {
            {.MountType = mount::Type::Tmpfs, .Target = "/data"},
            {.MountType = mount::Type::Volume, .Source = L"data-volume", .Target = "/data/"},
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
    }

    TEST_METHOD(Mount_DuplicateDestinationsAreRejected)
    {
        ContainerOptions options;
        options.Tmpfs = {"/data"};
        options.Mounts = {
            {.MountType = mount::Type::Volume, .Source = L"data-volume", .Target = "/data/"},
        };
        VERIFY_THROWS(ValidateUniqueMountDestinations(options), wil::ResultException);

        options.Tmpfs.clear();
        options.Mounts = {
            {.MountType = mount::Type::Tmpfs, .Target = "/data/../cache"},
        };
        options.Volumes = {L"data-volume:/cache"};
        VERIFY_THROWS(ValidateUniqueMountDestinations(options), wil::ResultException);
    }

    TEST_METHOD(Mount_UniqueDestinationsAreAccepted)
    {
        ContainerOptions options;
        options.Tmpfs = {"/cache"};
        options.Volumes = {L"data-volume:/data"};
        options.Mounts = {
            {.MountType = mount::Type::Bind, .Source = L"C:\\logs", .Target = "/logs"},
        };
        VERIFY_NO_THROW(ValidateUniqueMountDestinations(options));
    }
};

} // namespace WSLCCLIMountParserUnitTests
