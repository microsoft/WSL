/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIOutputParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI --output spec validation and parsing (validation::ParseOutputSpec).

    These tests define the contract for the docker-style `wslc image build --output` flag, mirroring
    `docker buildx build --output`. The parser under test is expected to expose:

        namespace wsl::windows::wslc::services {
        struct BuildOutput
        {
            std::wstring Type;                             // resolved exporter type (e.g. L"local", L"tar", ...)
            std::wstring Dest;                             // destination path; L"-" means stdout; empty when not applicable
            std::map<std::wstring, std::wstring> Attributes; // remaining key=value attributes (name, push, compression, ...)
        };
        }

        namespace wsl::windows::wslc::validation {
        services::BuildOutput ParseOutputSpec(const std::wstring& spec);
        }

    Grammar / behavior (docker buildx parity):
      * A single token with no '=' is shorthand for the destination:
          - L"-"            -> rejected (streaming a tarball to stdout is not supported)
          - any other path  -> {type=local, dest=<path>}
      * Otherwise the spec is a comma separated list of key=value pairs. Keys are matched
        case-insensitively. 'type' and 'dest' populate the struct fields; every other key is
        stored verbatim in Attributes (values may themselves contain '=').
      * Validation:
          - 'type' is required once any key=value pair is present.
          - 'type' must be one of: local, tar, oci, docker, image, registry, cacheonly.
          - local / tar / oci require 'dest='.
          - tar may not target stdout ('dest=-').
          - registry requires 'name='.
          - docker / image / cacheonly do not require a destination.
      * On rejection the parser throws ArgumentException whose message is the standard
        "Invalid --output value '<spec>': <reason>" wrapper (Localization::MessageWslcOutputInvalidSpec).

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ArgumentValidation.h"
#include "ImageService.h"
#include "Exceptions.h"
#include <map>
#include <string>

using namespace wsl::windows::wslc;

namespace WSLCCLIOutputParserUnitTests {

using AttrMap = std::map<std::wstring, std::wstring>;

class WSLCCLIOutputParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIOutputParserUnitTests)

    // Parses a spec expected to be valid and asserts the resolved type, destination and attributes.
    static void VerifyValid(const std::wstring& spec, const std::wstring& expectedType, const std::wstring& expectedDest, const AttrMap& expectedAttrs = {})
    {
        auto output = validation::ParseOutputSpec(spec);
        VERIFY_ARE_EQUAL(expectedType, output.Type);
        VERIFY_ARE_EQUAL(expectedDest, output.Dest);
        VERIFY_ARE_EQUAL(expectedAttrs.size(), output.Attributes.size());
        for (const auto& [key, value] : expectedAttrs)
        {
            const auto it = output.Attributes.find(key);
            VERIFY_IS_TRUE(it != output.Attributes.end());
            if (it != output.Attributes.end())
            {
                VERIFY_ARE_EQUAL(value, it->second);
            }
        }
    }

    // Parses a spec expected to be rejected and asserts it throws an ArgumentException whose message is
    // the standard "Invalid --output value '<spec>': <reason>" wrapper and contains the expected reason.
    static void VerifyInvalid(const std::wstring& spec, const std::wstring& expectedReasonSubstr)
    {
        try
        {
            (void)validation::ParseOutputSpec(spec);
            VERIFY_FAIL(L"Expected ArgumentException for invalid output spec");
        }
        catch (const ArgumentException& ex)
        {
            const std::wstring& message = ex.Message();
            VERIFY_IS_TRUE(message.find(L"Invalid --output value") != std::wstring::npos);
            VERIFY_IS_TRUE(message.find(expectedReasonSubstr) != std::wstring::npos);
        }
    }

    // --- Valid: shorthand (single token, no key=value pairs) ---

    TEST_METHOD(Output_Shorthand_LocalDirectory)
    {
        // A bare path is shorthand for exporting the final stage's filesystem to that directory.
        VerifyValid(L"./out", L"local", L"./out");
    }

    TEST_METHOD(Output_Shorthand_WindowsPath)
    {
        VerifyValid(L"C:\\build\\artifacts", L"local", L"C:\\build\\artifacts");
    }

    TEST_METHOD(Output_Shorthand_DashIsTarToStdout)
    {
        // '-' is docker's shorthand for streaming a tarball to stdout, which we disallow.
        VerifyInvalid(L"-", L"streaming a tarball to stdout is not supported");
    }

    // --- Valid: explicit local / tar / oci / docker exporters ---

    TEST_METHOD(Output_Local_ExplicitDest)
    {
        VerifyValid(L"type=local,dest=./out", L"local", L"./out");
    }

    TEST_METHOD(Output_Tar_ToFile)
    {
        VerifyValid(L"type=tar,dest=out.tar", L"tar", L"out.tar");
    }

    TEST_METHOD(Output_Tar_ToStdout)
    {
        // Streaming a tarball to stdout is not supported; a real file path is required.
        VerifyInvalid(L"type=tar,dest=-", L"streaming a tarball to stdout is not supported");
    }

    TEST_METHOD(Output_Oci_ToFile)
    {
        VerifyValid(L"type=oci,dest=image.tar", L"oci", L"image.tar");
    }

    TEST_METHOD(Output_Docker_ToFile)
    {
        VerifyValid(L"type=docker,dest=image.tar", L"docker", L"image.tar");
    }

    TEST_METHOD(Output_Docker_NoDestLoadsIntoStore)
    {
        // The docker exporter loads the image into the local store when no destination is given.
        VerifyValid(L"type=docker", L"docker", L"");
    }

    TEST_METHOD(Output_CacheOnly)
    {
        // cacheonly runs the build without exporting an artifact.
        VerifyValid(L"type=cacheonly", L"cacheonly", L"");
    }

    // --- Valid: image / registry exporters with attributes ---

    TEST_METHOD(Output_Image_NameAndPush)
    {
        VerifyValid(
            L"type=image,name=myrepo/app:1.0,push=true", L"image", L"", AttrMap{{L"name", L"myrepo/app:1.0"}, {L"push", L"true"}});
    }

    TEST_METHOD(Output_Registry_Name)
    {
        VerifyValid(L"type=registry,name=myrepo/app:latest", L"registry", L"", AttrMap{{L"name", L"myrepo/app:latest"}});
    }

    TEST_METHOD(Output_Registry_PushAttributes)
    {
        // Registry/push related attributes are passed through verbatim.
        VerifyValid(
            L"type=registry,name=myrepo/app:latest,push-by-digest=true,insecure=true,dangling-name-prefix=cache",
            L"registry",
            L"",
            AttrMap{
                {L"name", L"myrepo/app:latest"},
                {L"push-by-digest", L"true"},
                {L"insecure", L"true"},
                {L"dangling-name-prefix", L"cache"}});
    }

    TEST_METHOD(Output_Image_StoreAttributes)
    {
        // Image-store related attributes are passed through verbatim.
        VerifyValid(
            L"type=image,name=x,store=true,unpack=true,name-canonical=true",
            L"image",
            L"",
            AttrMap{{L"name", L"x"}, {L"store", L"true"}, {L"unpack", L"true"}, {L"name-canonical", L"true"}});
    }

    // --- Valid: attribute passthrough ---

    TEST_METHOD(Output_Attributes_CompressionOptions)
    {
        VerifyValid(
            L"type=image,name=x,compression=zstd,compression-level=19,oci-mediatypes=true",
            L"image",
            L"",
            AttrMap{{L"name", L"x"}, {L"compression", L"zstd"}, {L"compression-level", L"19"}, {L"oci-mediatypes", L"true"}});
    }

    TEST_METHOD(Output_Attributes_ForceCompression)
    {
        VerifyValid(
            L"type=oci,dest=o.tar,compression=gzip,compression-level=5,force-compression=true",
            L"oci",
            L"o.tar",
            AttrMap{{L"compression", L"gzip"}, {L"compression-level", L"5"}, {L"force-compression", L"true"}});
    }

    TEST_METHOD(Output_Attributes_ScopedAnnotation)
    {
        // Scoped annotations (annotation-manifest./annotation-index.) are preserved as-is.
        VerifyValid(
            L"type=oci,dest=o.tar,annotation-manifest.org.opencontainers.image.title=app",
            L"oci",
            L"o.tar",
            AttrMap{{L"annotation-manifest.org.opencontainers.image.title", L"app"}});
    }

    TEST_METHOD(Output_Local_PlatformSplit)
    {
        // platform-split controls per-platform subdirectories for the local/tar exporters.
        VerifyValid(L"type=local,dest=./out,platform-split=false", L"local", L"./out", AttrMap{{L"platform-split", L"false"}});
    }

    TEST_METHOD(Output_Attributes_AnnotationValueMayContainEquals)
    {
        // Only the first '=' separates key from value, so annotation values may themselves contain '='.
        VerifyValid(
            L"type=oci,dest=o.tar,annotation.org.opencontainers.image.source=https://example.com/repo?ref=main",
            L"oci",
            L"o.tar",
            AttrMap{{L"annotation.org.opencontainers.image.source", L"https://example.com/repo?ref=main"}});
    }

    TEST_METHOD(Output_Attributes_EmptyValuePreserved)
    {
        // A key with an explicit but empty value is preserved (the separator was present).
        VerifyValid(L"type=image,name=x,push=", L"image", L"", AttrMap{{L"name", L"x"}, {L"push", L""}});
    }

    TEST_METHOD(Output_Keys_AreCaseInsensitive)
    {
        VerifyValid(L"TYPE=local,DEST=./out", L"local", L"./out");
    }

    // --- Invalid: spec structure ---

    TEST_METHOD(Output_Invalid_Empty)
    {
        VerifyInvalid(L"", L"may not be empty");
    }

    TEST_METHOD(Output_Invalid_FieldWithoutEquals)
    {
        VerifyInvalid(L"type=local,garbage", L"expected key=value pairs separated by ','");
    }

    TEST_METHOD(Output_Invalid_LeadingFieldWithoutEquals)
    {
        VerifyInvalid(L"garbage,type=local", L"expected key=value pairs separated by ','");
    }

    TEST_METHOD(Output_Invalid_EmptyField)
    {
        VerifyInvalid(L"type=local,,dest=x", L"expected key=value pairs separated by ','");
    }

    // --- Invalid: type constraints ---

    TEST_METHOD(Output_Invalid_EmptyTypeValue)
    {
        VerifyInvalid(L"type=,dest=x", L"type is required");
    }

    TEST_METHOD(Output_Invalid_MissingType)
    {
        VerifyInvalid(L"dest=./out", L"type is required");
    }

    TEST_METHOD(Output_Invalid_UnsupportedType)
    {
        VerifyInvalid(L"type=bogus", L"unsupported output type 'bogus'");
    }

    // --- Invalid: destination / name requirements ---

    TEST_METHOD(Output_Invalid_LocalRequiresDest)
    {
        VerifyInvalid(L"type=local", L"'type=local' requires 'dest='");
    }

    TEST_METHOD(Output_Invalid_TarRequiresDest)
    {
        VerifyInvalid(L"type=tar", L"'type=tar' requires 'dest='");
    }

    TEST_METHOD(Output_Invalid_OciRequiresDest)
    {
        VerifyInvalid(L"type=oci", L"'type=oci' requires 'dest='");
    }

    TEST_METHOD(Output_Invalid_RegistryRequiresName)
    {
        VerifyInvalid(L"type=registry", L"'type=registry' requires 'name='");
    }

    // --- Round-trip: FormatOutputSpec re-serializes a BuildOutput into a canonical buildx spec ---

    // Parses spec, formats the result, and asserts the canonical serialized form.
    static void VerifyFormat(const std::wstring& spec, const std::wstring& expectedCanonical)
    {
        const auto canonical = validation::FormatOutputSpec(validation::ParseOutputSpec(spec));
        VERIFY_ARE_EQUAL(expectedCanonical, canonical);

        // The canonical form must itself parse back to an equivalent BuildOutput (idempotent round-trip).
        const auto reparsed = validation::ParseOutputSpec(canonical);
        const auto original = validation::ParseOutputSpec(spec);
        VERIFY_ARE_EQUAL(original.Type, reparsed.Type);
        VERIFY_ARE_EQUAL(original.Dest, reparsed.Dest);
        VERIFY_ARE_EQUAL(original.Attributes.size(), reparsed.Attributes.size());
        for (const auto& [key, value] : original.Attributes)
        {
            const auto it = reparsed.Attributes.find(key);
            VERIFY_IS_TRUE(it != reparsed.Attributes.end());
            if (it != reparsed.Attributes.end())
            {
                VERIFY_ARE_EQUAL(value, it->second);
            }
        }
    }

    TEST_METHOD(Format_Shorthand_LocalNormalizedToCanonical)
    {
        // Shorthand paths are normalized to their explicit type=local,dest= form for buildx.
        VerifyFormat(L"./out", L"type=local,dest=./out");
    }

    TEST_METHOD(Format_TypeOnly_NoDestOrAttributes)
    {
        // docker/cacheonly need neither dest nor attributes, so the canonical form is just the type.
        VerifyFormat(L"type=docker", L"type=docker");
        VerifyFormat(L"type=cacheonly", L"type=cacheonly");
    }

    TEST_METHOD(Format_TypeAndDest)
    {
        VerifyFormat(L"type=tar,dest=out.tar", L"type=tar,dest=out.tar");
    }

    TEST_METHOD(Format_CaseInsensitiveKeysNormalizedToLower)
    {
        // 'type'/'dest' keys are lowercased; the type value is lowercased too.
        VerifyFormat(L"TYPE=LOCAL,DEST=./out", L"type=local,dest=./out");
    }

    TEST_METHOD(Format_AttributesAppendedAfterDest)
    {
        // Attributes follow type/dest; std::map orders them, so 'name' precedes 'push'.
        VerifyFormat(L"type=image,push=true,name=x", L"type=image,name=x,push=true");
    }

    TEST_METHOD(Format_RegistryWithAttributes)
    {
        VerifyFormat(
            L"type=registry,name=myrepo/app:latest,push-by-digest=true",
            L"type=registry,name=myrepo/app:latest,push-by-digest=true");
    }
};

} // namespace WSLCCLIOutputParserUnitTests
