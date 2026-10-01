/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCImageReferenceUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC image and repository reference parsing.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslc.h"
#include "wslutil.h"

namespace WSLCImageReferenceUnitTests {

using wsl::windows::common::wslutil::ImageReference;
using wsl::windows::common::wslutil::RepositoryReference;

class WSLCImageReferenceUnitTests
{
    WSLC_TEST_CLASS(WSLCImageReferenceUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(ImageParsing)
    {
        auto ValidateImageParsing = [](const std::string& input,
                                       const std::string& expectedRepo,
                                       const std::optional<std::string>& expectedTag,
                                       const std::optional<std::string>& expectedDigest = std::nullopt) {
            auto reference = ImageReference::Parse(input);

            // The repository is parsed into a RepositoryReference; Name preserves the original token while Server and
            // Path hold its normalized form.
            const auto expectedRepository = RepositoryReference::Parse(expectedRepo);
            VERIFY_ARE_EQUAL(reference.Repository.Name, expectedRepository.Name);
            VERIFY_ARE_EQUAL(reference.Repository.Server, expectedRepository.Server);
            VERIFY_ARE_EQUAL(reference.Repository.Path, expectedRepository.Path);
            VERIFY_ARE_EQUAL(reference.Tag.value_or("<empty>"), expectedTag.value_or("<empty>"));
            VERIFY_ARE_EQUAL(reference.Digest.value_or("<empty>"), expectedDigest.value_or("<empty>"));

            // TagOrDigest() collapses to a single field where a digest takes precedence over a tag.
            const std::optional<std::string> expectedTagOrDigest = expectedDigest.has_value() ? expectedDigest : expectedTag;
            VERIFY_ARE_EQUAL(reference.TagOrDigest().value_or("<empty>"), expectedTagOrDigest.value_or("<empty>"));

            // Format mirrors that same classification.
            EnumReferenceFormat expectedFormat = EnumReferenceFormatNone;
            if (expectedDigest.has_value())
            {
                expectedFormat = EnumReferenceFormatDigest;
            }
            else if (expectedTag.has_value())
            {
                expectedFormat = EnumReferenceFormatTag;
            }

            VERIFY_ARE_EQUAL(reference.Format, expectedFormat);
        };

        ValidateImageParsing("ubuntu:22.04", "ubuntu", "22.04");
        ValidateImageParsing("ubuntu", "ubuntu", {});
        ValidateImageParsing("library/ubuntu:latest", "library/ubuntu", "latest");
        ValidateImageParsing("myregistry.io:5000/myimage:v1", "myregistry.io:5000/myimage", "v1");
        ValidateImageParsing("myregistry.io:5000/myimage", "myregistry.io:5000/myimage", {});

        ValidateImageParsing(
            "registry.example.com:8080/org/project/image:stable", "registry.example.com:8080/org/project/image", "stable");

        ValidateImageParsing("localhost:5000/myimage:latest", "localhost:5000/myimage", "latest");
        ValidateImageParsing("ghcr.io/owner/repo:sha-abc123", "ghcr.io/owner/repo", "sha-abc123");

        // A digest-only reference populates the digest field and leaves the tag empty.
        ValidateImageParsing(
            "ubuntu@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            {},
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        // A reference with both a tag and a digest captures each in its own field.
        ValidateImageParsing(
            "ubuntu:latest@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            "latest",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing(
            "myregistry.io:5000/myimage@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "myregistry.io:5000/myimage",
            {},
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing(
            "ubuntu:22.04@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            "22.04",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing("pytorch/pytorch", "pytorch/pytorch", {});

        // Invalid inputs
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse(""); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse(":debian:latest"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse("debian:latest@"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse(""); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse(":"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse("a:"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ImageReference::Parse(":b"); }), E_INVALIDARG);

        // TryParse reports the same malformed references without throwing, so a caller listing
        // references supplied by the daemon can skip a bad entry instead of failing.
        VERIFY_IS_FALSE(ImageReference::TryParse("").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse(":debian:latest").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("debian:latest@").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("a:").has_value());

        // The placeholders the daemon reports for an unnamed image are not valid references.
        VERIFY_IS_FALSE(ImageReference::TryParse("<none>").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("<none>:<none>").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("<none>@<none>").has_value());

        // A repository digest is only usable when the digest itself is well formed.
        VERIFY_IS_FALSE(ImageReference::TryParse("debian@").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("debian@sha256:").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("debian@sha256:notahexdigest").has_value());
        VERIFY_IS_FALSE(ImageReference::TryParse("debian").value().Digest.has_value());

        const auto parsed = ImageReference::TryParse("ubuntu:22.04");
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(parsed->Repository.Name, std::string{"ubuntu"});
        VERIFY_ARE_EQUAL(parsed->Tag.value_or("<empty>"), std::string{"22.04"});
    }

    TEST_METHOD(RepoParsing)
    {
        auto ValidateRepoParsing = [](const std::string& input, const std::string& expectedServer, const std::string& expectedPath) {
            auto repository = RepositoryReference::Parse(input);
            VERIFY_ARE_EQUAL(repository.Name, input);
            VERIFY_ARE_EQUAL(repository.Server, expectedServer);
            VERIFY_ARE_EQUAL(repository.Path, expectedPath);

            // GetCanonical() rejoins the normalized server and path.
            VERIFY_ARE_EQUAL(repository.GetCanonical(), std::format("{}/{}", expectedServer, expectedPath));
        };

        ValidateRepoParsing("ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("docker.io/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("index.docker.io/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("index.docker.io/library/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("docker.io/library/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("microsoft.com/ubuntu", "microsoft.com", "ubuntu");
        ValidateRepoParsing("microsoft.com:80/ubuntu", "microsoft.com:80", "ubuntu");
        ValidateRepoParsing("microsoft.com:80/ubuntu/foo/bar", "microsoft.com:80", "ubuntu/foo/bar");
        ValidateRepoParsing("127.0.0.1:80/ubuntu/foo/bar", "127.0.0.1:80", "ubuntu/foo/bar");
        ValidateRepoParsing("pytorch/pytorch", "docker.io", "pytorch/pytorch");
        ValidateRepoParsing("2001:0db8:85a3:0000:0000:8a2e:0370:7334/path", "2001:0db8:85a3:0000:0000:8a2e:0370:7334", "path");
        ValidateRepoParsing(
            "2001:0db8:85a3:0000:0000:8a2e:0370:7334:80/path", "2001:0db8:85a3:0000:0000:8a2e:0370:7334:80", "path");
    }

    TEST_METHOD(CanonicalImageReference)
    {
        auto Validate = [](const std::string& input, const std::string& expected) {
            VERIFY_ARE_EQUAL(ImageReference::Parse(input).GetCanonical(), expected);
        };

        // Name-only references default to ":latest" and the docker.io/library prefix (matches `docker pull` output).
        Validate("ubuntu", "docker.io/library/ubuntu:latest");
        Validate("ubuntu:22.04", "docker.io/library/ubuntu:22.04");
        Validate("library/ubuntu", "docker.io/library/ubuntu:latest");
        Validate("pytorch/pytorch", "docker.io/pytorch/pytorch:latest");
        Validate("docker.io/ubuntu", "docker.io/library/ubuntu:latest");
        Validate("index.docker.io/library/ubuntu:latest", "docker.io/library/ubuntu:latest");

        // Custom registries keep their domain and path.
        Validate("ghcr.io/owner/repo:sha-abc123", "ghcr.io/owner/repo:sha-abc123");
        Validate("myregistry.io:5000/myimage", "myregistry.io:5000/myimage:latest");
        Validate("localhost:5000/myimage:latest", "localhost:5000/myimage:latest");

        // A mixed-case registry domain is preserved verbatim, matching Docker (which never lowercases the domain).
        Validate("Example.COM/owner/repo", "Example.COM/owner/repo:latest");

        // Digest references are preserved.
        Validate(
            "ubuntu@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "docker.io/library/ubuntu@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        // A tag and digest are both preserved when both are present, matching Docker's canonical reference.
        Validate(
            "ubuntu:22.04@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "docker.io/library/ubuntu:22.04@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");
    }
};
} // namespace WSLCImageReferenceUnitTests
