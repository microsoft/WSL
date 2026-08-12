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
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include "Exceptions.h"
#include "MountSpecParsing.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::models;
using namespace WEX::Logging;
using namespace WEX::Common;

namespace WSLCCLIMountParserUnitTests {

class WSLCCLIMountParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIMountParserUnitTests)

    static void VerifyVolume(const std::wstring& spec, const std::wstring& expected)
    {
        const auto mount = validation::ParseMount(spec);
        VERIFY_IS_FALSE(mount.IsTmpfs);
        VERIFY_ARE_EQUAL(expected, mount.VolumeSpec);
        VERIFY_IS_TRUE(mount.TmpfsSpec.empty());
    }

    static void VerifyTmpfs(const std::wstring& spec, const std::string& expected)
    {
        const auto mount = validation::ParseMount(spec);
        VERIFY_IS_TRUE(mount.IsTmpfs);
        VERIFY_ARE_EQUAL(expected, mount.TmpfsSpec);
        VERIFY_IS_TRUE(mount.VolumeSpec.empty());
    }

    static void VerifyInvalid(const std::wstring& spec, const std::wstring& expectedReason)
    {
        Log::Comment(String().Format(L"Rejecting: %ls", spec.c_str()));
        try
        {
            (void)validation::ParseMount(spec);
            VERIFY_FAIL(L"Expected ArgumentException for invalid mount spec");
        }
        catch (const ArgumentException& ex)
        {
            const auto& message = ex.Message();
            VERIFY_IS_TRUE(message.find(L"for '--mount' flag") != std::wstring::npos);
            VERIFY_IS_TRUE(message.find(expectedReason) != std::wstring::npos);
        }
    }

    TEST_METHOD(Mount_KeysAndTypeAreCaseInsensitive)
    {
        VerifyVolume(L"TYPE=VOLUME,SOURCE=data-volume,TARGET=/data", L"data-volume:/data");
    }

    TEST_METHOD(Mount_AliasesMatchDocker)
    {
        VerifyVolume(L"type=volume,src=data-volume,dst=/data,ro", L"data-volume:/data:ro");
        VerifyVolume(L"type=volume,source=data-volume,destination=/data", L"data-volume:/data");
    }

    TEST_METHOD(Mount_DefaultTypeIsVolume)
    {
        VerifyVolume(L"source=data-volume,target=/data", L"data-volume:/data");
    }

    TEST_METHOD(Mount_ReadOnlyUsesGoBooleanSpellings)
    {
        VerifyVolume(L"type=volume,source=data-volume,target=/data,readonly=t", L"data-volume:/data:ro");
        VerifyVolume(L"type=volume,source=data-volume,target=/data,readonly=TRUE", L"data-volume:/data:ro");
        VerifyVolume(L"type=volume,source=data-volume,target=/data,readonly=0", L"data-volume:/data");
        VerifyVolume(L"type=volume,source=data-volume,target=/data,readonly=F", L"data-volume:/data");
    }

    TEST_METHOD(Mount_CsvQuotedFieldPreservesComma)
    {
        VerifyVolume(L"type=bind,\"source=C:\\mount,a\",target=/data", L"C:\\mount,a:/data");
    }

    TEST_METHOD(Mount_BindRecursiveEnabledIsDefaultBehavior)
    {
        VerifyVolume(L"type=bind,source=C:\\mount,target=/data,bind-recursive=enabled", L"C:\\mount:/data");
    }

    TEST_METHOD(Mount_TmpfsOptionsMatchDockerConversion)
    {
        VerifyTmpfs(L"type=tmpfs,target=/tmp,tmpfs-size=1MB,tmpfs-mode=0700,readonly", "/tmp:ro,mode=700,size=1m");
        VerifyTmpfs(L"type=tmpfs,target=/tmp,tmpfs-size=1.5MB", "/tmp:size=1536k");
        VerifyTmpfs(L"type=tmpfs,target=/tmp,tmpfs-size=1536", "/tmp:size=1536");
        VerifyTmpfs(L"type=tmpfs,target=/tmp,tmpfs-size=0,tmpfs-mode=0000", "/tmp");
    }

    TEST_METHOD(Mount_InvalidFieldsMatchDocker)
    {
        VerifyInvalid(L"type=volume,bogus", L"invalid field 'bogus' must be a key=value pair");
        VerifyInvalid(L"type=volume,bogus=value", L"unexpected key 'bogus'");
        VerifyInvalid(L"type=volume,source=data-volume,target=/data,readonly=no", L"invalid value for readonly: no");
        VerifyInvalid(L"type=tmpfs,target=/tmp,tmpfs-size=bad", L"invalid value for tmpfs-size: bad");
        VerifyInvalid(L"type=tmpfs,target=/tmp,\"tmpfs-size=1,5MB\"", L"invalid value for tmpfs-size: 1,5MB");
        VerifyInvalid(
            L"type=tmpfs,target=/tmp,tmpfs-size=9223372036854775808", L"invalid value for tmpfs-size: 9223372036854775808");
        VerifyInvalid(L"type=tmpfs,target=/tmp,tmpfs-mode=0899", L"invalid value for tmpfs-mode: 0899");
        VerifyInvalid(L"type=bind,source=C:\\mount,target=/data,bind-recursive=Enabled", L"invalid value for bind-recursive");
    }

    TEST_METHOD(Mount_RequiredFieldsMatchDocker)
    {
        VerifyInvalid(L"type=,source=data-volume,target=/data", L"type is required");
        VerifyInvalid(L"type=volume,source=data-volume", L"target is required");
    }

    TEST_METHOD(Mount_OptionTypeConflictsMatchDocker)
    {
        VerifyInvalid(
            L"type=bind,source=C:\\mount,target=/data,volume-nocopy=true",
            L"cannot mix 'volume-*' options with mount type 'bind'");
        VerifyInvalid(
            L"type=volume,source=data-volume,target=/data,bind-propagation=rprivate",
            L"cannot mix 'bind-*' options with mount type 'volume'");
        VerifyInvalid(
            L"type=volume,source=data-volume,target=/data,tmpfs-size=1m",
            L"cannot mix 'tmpfs-*' options with mount type 'volume'");
    }

    TEST_METHOD(Mount_BindRecursiveValidationMatchesDocker)
    {
        VerifyInvalid(
            L"type=bind,source=C:\\mount,target=/data,bind-recursive=writable",
            L"requires 'readonly' to be specified in conjunction");
        VerifyInvalid(
            L"type=bind,source=C:\\mount,target=/data,bind-recursive=readonly,readonly",
            L"requires 'bind-propagation=rprivate' to be specified in conjunction");
    }

    TEST_METHOD(Mount_UnsupportedBackendFeaturesAreExplicit)
    {
        VerifyInvalid(
            L"type=volume,source=data-volume,target=/data,volume-nocopy", L"option 'volume-nocopy' is not supported by WSLC");
        VerifyInvalid(
            L"type=bind,source=C:\\mount,target=/data,consistency=cached", L"option 'consistency' is not supported by WSLC");
        VerifyInvalid(L"type=cluster,source=data-volume,target=/data", L"mount type 'cluster' is not supported by WSLC");
        VerifyInvalid(L"type=volume,target=/data", L"anonymous volume mounts are not supported by WSLC");
    }

    TEST_METHOD(Mount_BackendRepresentationLimitsAreExplicit)
    {
        VerifyInvalid(L"type=bind,source=relative,target=/data", L"bind source path must be absolute");
        VerifyInvalid(L"type=volume,source=C:\\mount,target=/data", L"volume source must be a valid named volume");
        VerifyInvalid(L"type=tmpfs,source=data-volume,target=/data", L"source is not supported for tmpfs mounts");
        VerifyInvalid(
            L"type=volume,source=data-volume,target=/data:part", L"target paths containing ':' are not supported by WSLC");
    }

    TEST_METHOD(Mount_MalformedCsvIsRejected)
    {
        VerifyInvalid(L"type=bind,\"source=C:\\mount,target=/data", L"malformed CSV");
    }

    TEST_METHOD(Mount_DuplicateDestinationsAreRejected)
    {
        ContainerOptions options;
        options.Tmpfs = {"/data", "/data/"};
        VERIFY_THROWS(ValidateUniqueMountDestinations(options), wil::ResultException);

        options.Tmpfs = {"/data/../cache"};
        options.Volumes = {L"data-volume:/cache"};
        VERIFY_THROWS(ValidateUniqueMountDestinations(options), wil::ResultException);
    }

    TEST_METHOD(Mount_UniqueDestinationsAreAccepted)
    {
        ContainerOptions options;
        options.Tmpfs = {"/cache"};
        options.Volumes = {L"data-volume:/data"};
        VERIFY_NO_THROW(ValidateUniqueMountDestinations(options));
    }
};

} // namespace WSLCCLIMountParserUnitTests
