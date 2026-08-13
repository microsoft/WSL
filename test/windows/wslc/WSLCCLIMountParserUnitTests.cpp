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

    struct InvalidMountCase
    {
        const wchar_t* Input;
        const wchar_t* ExpectedReason;
    };

    constexpr ValidMountCase c_validMountCases[] = {
        {L"source=data-volume,target=/data", mount::Type::Volume, L"data-volume", "/data", false, {}, {}, ""},
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

    constexpr InvalidMountCase c_invalidMountCases[] = {
        {L"", L"invalid field '' must be a key=value pair"},
        {L",", L"invalid field '' must be a key=value pair"},
        {L"type=volume,source=data-volume,target=/data,", L"invalid field '' must be a key=value pair"},
        {L",type=volume,source=data-volume,target=/data", L"invalid field '' must be a key=value pair"},
        {L"type=bind,\"source=C:\\mount,target=/data", L"malformed CSV"},
        {L"type=volume,bogus", L"invalid field 'bogus' must be a key=value pair"},
        {L"type=volume,bogus=value", L"unexpected key 'bogus'"},
        {L"type", L"invalid field 'type' must be a key=value pair"},
        {L"source", L"invalid field 'source' must be a key=value pair"},
        {L"target", L"invalid field 'target' must be a key=value pair"},
        {L"type=,source=data-volume,target=/data", L"type is required"},
        {L"type=volume,source=data-volume", L"target is required"},
        {L"type=volume,source=data-volume,target=", L"target is required"},
        {L"type=volume,source=data-volume,dst=", L"target is required"},
        {L"type=cluster,source=data-volume,target=/data", L"mount type 'cluster' is not supported."},
        {L"type=npipe,source=data-volume,target=/data", L"mount type 'npipe' is not supported."},
        {L"type=bogus,source=data-volume,target=/data", L"mount type 'bogus' is not supported."},
        {L"type=CLUSTER,source=data-volume,target=/data", L"mount type 'cluster' is not supported."},
        {L"type=volume,target=/data", L"anonymous volume mounts are not supported."},
        {L"type=bind,target=/data", L"source is required"},
        {L"type=bind,source=relative,target=/data", L"bind source path must be absolute"},
        {L"type=volume,source=a,target=/data", L"volume source must be a valid named volume"},
        {L"type=volume,source=data/volume,target=/data", L"volume source must be a valid named volume"},
        {L"type=volume,source=C:\\mount,target=/data", L"volume source must be a valid named volume"},
        {L"type=tmpfs,source=data-volume,target=/data", L"source is not supported for tmpfs mounts"},
        {L"type=volume,source=data-volume,target=/data:part", L"target paths containing ':' are not supported."},
        {L"type=volume,source=data-volume,target=data", L"target path must be absolute"},
        {L"type=volume,source=data-volume,dst=.", L"target path must be absolute"},
        {L"type=volume,source=data-volume,destination=\\data", L"target path must be absolute"},
        {L"type=bind,source=C:\\mount,target=data", L"target path must be absolute"},
        {L"type=tmpfs,target=data", L"target path must be absolute"},
        {L"type=volume,source=data-volume,target=/data,readonly=no", L"invalid value for readonly: no"},
        {L"type=volume,source=data-volume,target=/data,readonly=yes", L"invalid value for readonly: yes"},
        {L"type=volume,source=data-volume,target=/data,readonly=", L"invalid value for readonly: "},
        {L"type=volume,source=data-volume,target=/data,readonly=2", L"invalid value for readonly: 2"},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=no", L"invalid value for volume-nocopy: no"},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=", L"invalid value for volume-nocopy: "},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive=no", L"invalid value for bind-nonrecursive: no"},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=", L"invalid value for bind-recursive: "},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=Enabled", L"invalid value for bind-recursive: Enabled"},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=bogus", L"invalid value for bind-recursive: bogus"},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=writable",
         L"option 'bind-recursive=writable' requires 'readonly' to be specified in conjunction"},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly",
         L"option 'bind-recursive=readonly' requires 'readonly' to be specified in conjunction"},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly,readonly",
         L"option 'bind-recursive=readonly' requires 'bind-propagation=rprivate' to be specified in conjunction"},
        {L"type=bind,source=C:\\mount,target=/data,consistency=cached", L"option 'consistency' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-propagation=rprivate", L"option 'bind-propagation' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive", L"option 'bind-nonrecursive' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-nonrecursive=true", L"option 'bind-nonrecursive' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=disabled", L"option 'bind-recursive' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=writable,readonly",
         L"option 'bind-recursive' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly,readonly,bind-propagation=rprivate",
         L"option 'bind-recursive' is not supported."},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy", L"option 'volume-nocopy' is not supported."},
        {L"type=volume,source=data-volume,target=/data,volume-nocopy=true", L"option 'volume-nocopy' is not supported."},
        {L"type=volume,source=data-volume,target=/data,volume-label=a=b", L"option 'volume-label' is not supported."},
        {L"type=volume,source=data-volume,target=/data,volume-driver=local", L"option 'volume-driver' is not supported."},
        {L"type=volume,source=data-volume,target=/data,volume-opt=a=b", L"option 'volume-opt' is not supported."},
        {L"type=bind,source=C:\\mount,target=/data,volume-nocopy=true", L"cannot mix 'volume-*' options with mount type 'bind'"},
        {L"type=volume,source=data-volume,target=/data,bind-propagation=rprivate",
         L"cannot mix 'bind-*' options with mount type 'volume'"},
        {L"type=volume,source=data-volume,target=/data,tmpfs-size=1m", L"cannot mix 'tmpfs-*' options with mount type 'volume'"},
        {L"type=tmpfs,target=/tmp,volume-label=a=b", L"cannot mix 'volume-*' options with mount type 'tmpfs'"},
        {L"type=tmpfs,target=/tmp,bind-nonrecursive", L"cannot mix 'bind-*' options with mount type 'tmpfs'"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=", L"invalid value for tmpfs-size: "},
        {L"type=tmpfs,target=/tmp,tmpfs-size=bad", L"invalid value for tmpfs-size: bad"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=-1", L"invalid value for tmpfs-size: -1"},
        {L"type=tmpfs,target=/tmp,\"tmpfs-size=1,5MB\"", L"invalid value for tmpfs-size: 1,5MB"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1XB", L"invalid value for tmpfs-size: 1XB"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1Ki", L"invalid value for tmpfs-size: 1Ki"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=1BB", L"invalid value for tmpfs-size: 1BB"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=nan", L"invalid value for tmpfs-size: nan"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=inf", L"invalid value for tmpfs-size: inf"},
        {L"type=tmpfs,target=/tmp,tmpfs-size=9223372036854775808", L"invalid value for tmpfs-size: 9223372036854775808"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=", L"invalid value for tmpfs-mode: "},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=-1", L"invalid value for tmpfs-mode: -1"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=8", L"invalid value for tmpfs-mode: 8"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0899", L"invalid value for tmpfs-mode: 0899"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=0x700", L"invalid value for tmpfs-mode: 0x700"},
        {L"type=tmpfs,target=/tmp,tmpfs-mode=40000000000", L"invalid value for tmpfs-mode: 40000000000"},
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
                VERIFY_FAIL(L"Expected ValidationException for invalid mount spec");
            }
            catch (const mount::ValidationException& ex)
            {
                VERIFY_IS_TRUE(ex.Reason().find(testCase.ExpectedReason) != std::wstring::npos);
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
        VERIFY_THROWS(mount::ValidateMountSpec(relativeBind), mount::ValidationException);

        const mount::Spec relativeTarget{
            .MountType = mount::Type::Volume,
            .Source = L"data-volume",
            .Target = "data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(relativeTarget), mount::ValidationException);

        const mount::Spec tmpfsWithSource{
            .MountType = mount::Type::Tmpfs,
            .Source = L"data-volume",
            .Target = "/data",
        };
        VERIFY_THROWS(mount::ValidateMountSpec(tmpfsWithSource), mount::ValidationException);

        const mount::Spec bindWithTmpfsOptions{
            .MountType = mount::Type::Bind,
            .Source = L"C:\\data",
            .Target = "/data",
            .TmpfsSizeBytes = 1024,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(bindWithTmpfsOptions), mount::ValidationException);

        const mount::Spec negativeTmpfsSize{
            .MountType = mount::Type::Tmpfs,
            .Target = "/data",
            .TmpfsSizeBytes = -1,
        };
        VERIFY_THROWS(mount::ValidateMountSpec(negativeTmpfsSize), mount::ValidationException);

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
            VERIFY_FAIL(L"Expected ValidationException for duplicate destinations");
        }
        catch (const mount::ValidationException& ex)
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
