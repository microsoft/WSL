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
      * The spec is parsed as a single CSV record (RFC 4180, as buildx does via go-csvvalue): fields
        are comma separated, a field may be double-quoted, "" inside a quoted field is a literal quote,
        and a comma inside a quoted field is part of the value.
      * A single field that equals the whole input and does not start with "type=" is shorthand for the
        destination:
          - L"-"            -> {type=tar, dest=-} (stream a tarball to stdout, matching docker)
          - any other path  -> {type=local, dest=<path>} (rejected: directory exporters are unsupported)
      * Otherwise each field is split on its FIRST '=' into key/value (two parts required). The key is
        trimmed and lowercased; the value is kept verbatim (may itself contain '='). 'type' and 'dest'
        populate the struct fields; every other key is stored in Attributes.
      * Validation / destination resolution:
          - 'type' is required and must be one of: local, tar, oci, docker, image, registry, cacheonly.
          - Directory exporters (local, or oci/docker with tar=false) are not supported and are rejected.
          - tar / oci with no 'dest=' default to streaming a tarball to stdout ('dest=-'), matching buildx.
          - docker with no 'dest=' loads the image into the store; 'dest=-' streams a tarball to stdout;
            a path writes a file.
          - image / registry / cacheonly run in the build VM and ignore 'dest'; 'name=' is optional
            (buildx only enforces it at export time, not at parse time).
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
        // A bare path is shorthand for the local (directory) exporter, which is not supported.
        VerifyInvalid(L"./out", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Shorthand_WindowsPath)
    {
        // A bare Windows path is likewise the local (directory) exporter, which is not supported.
        VerifyInvalid(L"C:\\build\\artifacts", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Shorthand_DashIsTarToStdout)
    {
        // '-' is docker's shorthand for streaming a tarball to stdout ('type=tar,dest=-').
        VerifyValid(L"-", L"tar", L"-");
    }

    // --- Valid: explicit local / tar / oci / docker exporters ---

    TEST_METHOD(Output_Local_ExplicitDest)
    {
        // The local exporter writes a directory tree and is not supported.
        VerifyInvalid(L"type=local,dest=./out", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Tar_ToFile)
    {
        VerifyValid(L"type=tar,dest=out.tar", L"tar", L"out.tar");
    }

    TEST_METHOD(Output_Tar_ToStdout)
    {
        // tar streams a single tarball, so it may target stdout ('dest=-'), matching docker.
        VerifyValid(L"type=tar,dest=-", L"tar", L"-");
    }

    TEST_METHOD(Output_Tar_NoDest_DefaultsToStdout)
    {
        // tar with no destination streams a tarball to stdout ('dest=-'), matching buildx.
        VerifyValid(L"type=tar", L"tar", L"-");
    }

    TEST_METHOD(Output_Local_ToStdout_Rejected)
    {
        // The local exporter writes a directory tree and is not supported.
        VerifyInvalid(L"type=local,dest=-", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_ToFile)
    {
        VerifyValid(L"type=oci,dest=image.tar", L"oci", L"image.tar");
    }

    TEST_METHOD(Output_Oci_NoDest_DefaultsToStdout)
    {
        // oci with no destination streams a tarball to stdout ('dest=-'), matching buildx.
        VerifyValid(L"type=oci", L"oci", L"-");
    }

    TEST_METHOD(Output_Oci_TarFalse_Directory)
    {
        // oci with tar=false exports an OCI layout directory, which is not supported.
        VerifyInvalid(L"type=oci,dest=./layout,tar=false", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_TarFalse_RequiresDest)
    {
        // A directory exporter is not supported regardless of dest.
        VerifyInvalid(L"type=oci,tar=false", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Docker_TarFalse_Directory)
    {
        // docker with tar=false likewise exports an OCI layout directory, which is not supported.
        VerifyInvalid(L"type=docker,dest=./layout,tar=false", L"directory exporters are not supported");
    }

    // --- Valid: 'tar' accepts every spelling Go's strconv.ParseBool does (buildx parity) ---

    TEST_METHOD(Output_Oci_TarFalse_CapitalizedIsDirectory)
    {
        // buildx parses 'tar' with Go's ParseBool, so "False" is a directory exporter just like "false",
        // and is rejected the same way.
        VerifyInvalid(L"type=oci,dest=./layout,tar=False", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_TarZeroIsDirectory)
    {
        // "0" is false for Go's ParseBool, so it selects the OCI layout directory exporter, which is not
        // supported.
        VerifyInvalid(L"type=oci,dest=./layout,tar=0", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_TarShortFalseIsDirectory)
    {
        // "f" is the short false form accepted by Go's ParseBool, so it too is a directory exporter.
        VerifyInvalid(L"type=oci,dest=./layout,tar=f", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_TarCapitalizedFalse_RequiresDest)
    {
        // A directory exporter is not supported regardless of the boolean spelling.
        VerifyInvalid(L"type=oci,tar=False", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Docker_TarZeroIsDirectory)
    {
        // docker with tar=0 likewise exports an OCI layout directory, which is not supported.
        VerifyInvalid(L"type=docker,dest=./layout,tar=0", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Oci_TarTrue_IsSingleTarballToStdout)
    {
        // "True" is true for Go's ParseBool, so oci stays a single tarball and defaults to stdout.
        VerifyValid(L"type=oci,tar=True", L"oci", L"-", AttrMap{{L"tar", L"True"}});
    }

    TEST_METHOD(Output_Oci_TarOne_IsSingleTarballToFile)
    {
        // "1" is true, so this is a single-tarball export to a file (not a directory).
        VerifyValid(L"type=oci,dest=image.tar,tar=1", L"oci", L"image.tar", AttrMap{{L"tar", L"1"}});
    }

    TEST_METHOD(Output_Docker_TarTrue_LoadsIntoStore)
    {
        // docker with tar=true is a single tarball; with no dest it loads into the VM store (dest empty).
        VerifyValid(L"type=docker,tar=t", L"docker", L"", AttrMap{{L"tar", L"t"}});
    }

    TEST_METHOD(Output_Oci_TarInvalidBool_Rejected)
    {
        // A non-boolean 'tar' value is rejected up front, matching buildx (which errors in ParseBool).
        VerifyInvalid(L"type=oci,dest=./layout,tar=yes", L"invalid boolean value 'yes' for 'tar'");
    }

    TEST_METHOD(Output_Docker_TarInvalidBool_Rejected)
    {
        VerifyInvalid(L"type=docker,dest=./layout,tar=maybe", L"invalid boolean value 'maybe' for 'tar'");
    }

    TEST_METHOD(Output_Docker_ToFile)
    {
        VerifyValid(L"type=docker,dest=image.tar", L"docker", L"image.tar");
    }

    TEST_METHOD(Output_Docker_ToStdout)
    {
        // docker with dest=- streams the image tarball to stdout (matching docker), which the client
        // routes to the redirected stdout handle.
        VerifyValid(L"type=docker,dest=-", L"docker", L"-");
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

    TEST_METHOD(Output_Registry_NoName_Valid)
    {
        // buildx only enforces 'name=' at export time, not at parse time, so parsing must accept it.
        VerifyValid(L"type=registry", L"registry", L"");
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

    TEST_METHOD(Output_Tar_PlatformSplit)
    {
        // platform-split is forwarded verbatim as an exporter attribute for the tar exporter.
        VerifyValid(L"type=tar,dest=out.tar,platform-split=false", L"tar", L"out.tar", AttrMap{{L"platform-split", L"false"}});
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
        VerifyValid(L"TYPE=tar,DEST=out.tar", L"tar", L"out.tar");
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
        // With two or more fields no shorthand applies, so a spec without 'type=' is rejected.
        VerifyInvalid(L"dest=./out,compression=gzip", L"type is required");
    }

    TEST_METHOD(Output_Shorthand_SingleFieldWithEqualsIsLocalPath)
    {
        // buildx parity quirk: a single field equal to the whole input that does not start with
        // "type=" is shorthand for a local path, even if it happens to contain '=' (so '--output
        // dest=./out' names a local directory "dest=./out"). The local exporter is not supported.
        VerifyInvalid(L"dest=./out", L"directory exporters are not supported");
    }

    TEST_METHOD(Output_Invalid_UnsupportedType)
    {
        VerifyInvalid(L"type=bogus", L"unsupported output type 'bogus'");
    }

    // --- Invalid: destination requirements ---

    TEST_METHOD(Output_Invalid_LocalRequiresDest)
    {
        // The local exporter writes a directory tree and is not supported.
        VerifyInvalid(L"type=local", L"directory exporters are not supported");
    }

    // --- CSV grammar (buildx go-csvvalue parity) ---

    TEST_METHOD(Output_Csv_QuotedValueWithComma)
    {
        // A comma inside a double-quoted field is part of the value, not a field separator.
        VerifyValid(
            L"type=image,name=x,\"annotation.foo=a,b,c\"", L"image", L"", AttrMap{{L"name", L"x"}, {L"annotation.foo", L"a,b,c"}});
    }

    TEST_METHOD(Output_Csv_QuotedValueWithEscapedQuote)
    {
        // A doubled quote inside a quoted field is a single literal quote.
        VerifyValid(
            L"type=image,name=x,\"annotation.foo=a\"\"b\"", L"image", L"", AttrMap{{L"name", L"x"}, {L"annotation.foo", L"a\"b"}});
    }

    TEST_METHOD(Output_Csv_LeadingSpaceAfterCommaTrimmedFromKey)
    {
        // buildx TrimSpace's the key, so a space after a comma is accepted (the value is untrimmed).
        VerifyValid(L"type=tar, dest=out.tar", L"tar", L"out.tar");
    }

    TEST_METHOD(Output_Csv_UnterminatedQuoteRejected)
    {
        VerifyInvalid(L"type=image,\"name=x", L"malformed quoting");
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
        VerifyFormat(L"TYPE=TAR,DEST=out.tar", L"type=tar,dest=out.tar");
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

    TEST_METHOD(Format_QuotesValueContainingComma)
    {
        // An attribute value containing a comma is CSV-quoted so it round-trips through the parser.
        // std::map orders attributes, so 'annotation.foo' precedes 'name'.
        VerifyFormat(L"type=image,name=x,\"annotation.foo=a,b,c\"", L"type=image,\"annotation.foo=a,b,c\",name=x");
    }

    TEST_METHOD(Format_TarNoDestDefaultsToStdout)
    {
        // tar with no dest resolves to dest=- and serializes back to that canonical form.
        VerifyFormat(L"type=tar", L"type=tar,dest=-");
    }
};

} // namespace WSLCCLIOutputParserUnitTests
