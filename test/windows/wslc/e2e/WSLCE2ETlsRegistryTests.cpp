/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ETlsRegistryTests.cpp

Abstract:

    End-to-end test for private-registry SSL trust. Stands up a registry that serves TLS with a
    private CA, verifies that a push fails with an "unknown authority" error while the CA is not
    trusted, then adds the CA to the machine's Trusted Root store and verifies that a push from a
    freshly created session succeeds.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wslutil.h>
#include <ncrypt.h>

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::windows::common;

namespace {
    // The bridge IP assigned to the first container started in a fresh session. The registry is always
    // the first container we start in this test's session, so this is deterministic.
    constexpr auto c_registryIp = "172.17.0.2";
    constexpr USHORT c_registryPort = 5000;

    // DER-encodes a certificate-extension value structure (e.g. CERT_ALT_NAME_INFO for the SAN OID).
    std::vector<BYTE> EncodeCertExtensionValue(LPCSTR Oid, const void* StructInfo)
    {
        DWORD size = 0;
        THROW_IF_WIN32_BOOL_FALSE(CryptEncodeObjectEx(X509_ASN_ENCODING, Oid, StructInfo, 0, nullptr, nullptr, &size));
        std::vector<BYTE> encoded(size);
        THROW_IF_WIN32_BOOL_FALSE(CryptEncodeObjectEx(X509_ASN_ENCODING, Oid, StructInfo, 0, nullptr, encoded.data(), &size));
        encoded.resize(size);
        return encoded;
    }

    // Wraps DER bytes in a PEM block with the given label (e.g. "CERTIFICATE", "PRIVATE KEY").
    std::string ToPem(const BYTE* Data, DWORD Size, const std::string& Label)
    {
        DWORD base64Size = 0;
        THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(Data, Size, CRYPT_STRING_BASE64, nullptr, &base64Size));
        std::string base64(base64Size, '\0');
        THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(Data, Size, CRYPT_STRING_BASE64, base64.data(), &base64Size));
        base64.resize(base64Size);
        return std::format("-----BEGIN {}-----\n{}-----END {}-----\n", Label, base64, Label);
    }

    // Generates a self-signed certificate usable as both a TLS server cert and a CA,
    // writes server.crt and server.key (PEM) into OutDir for the registry to serve,
    // and returns the certificate context so the test can add it to a trust store later.
    wil::unique_cert_context GenerateRegistryTlsCertificate(const std::string& IpAddress, const std::filesystem::path& OutDir)
    {
        // Create an exportable RSA key
        NCRYPT_PROV_HANDLE prov{};
        THROW_IF_FAILED(NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0));
        auto freeProv = wil::scope_exit([&] { NCryptFreeObject(prov); });

        GUID guid{};
        THROW_IF_FAILED(CoCreateGuid(&guid));
        const auto keyName =
            L"WslcE2eTlsKey-" + wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);

        NCRYPT_KEY_HANDLE key{};
        THROW_IF_FAILED(NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, keyName.c_str(), 0, 0));
        auto deleteKey = wil::scope_exit([&] { NCryptDeleteKey(key, 0); }); // also frees the handle

        DWORD keyLength = 2048;
        THROW_IF_FAILED(NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&keyLength), sizeof(keyLength), 0));
        DWORD exportPolicy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
        THROW_IF_FAILED(NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&exportPolicy), sizeof(exportPolicy), 0));
        THROW_IF_FAILED(NCryptFinalizeKey(key, 0));

        // Build the extensions (SAN, basic constraints, key usage, extended key usage)
        in_addr ipv4{};
        wslutil::ParseIpv4Address(IpAddress.c_str(), ipv4);
        CERT_ALT_NAME_ENTRY altEntry{};
        altEntry.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        altEntry.IPAddress.cbData = sizeof(ipv4);
        altEntry.IPAddress.pbData = reinterpret_cast<BYTE*>(&ipv4);
        CERT_ALT_NAME_INFO altInfo{1, &altEntry};
        auto sanEncoded = EncodeCertExtensionValue(szOID_SUBJECT_ALT_NAME2, &altInfo);

        CERT_BASIC_CONSTRAINTS2_INFO basicConstraints{};
        basicConstraints.fCA = TRUE;
        auto bcEncoded = EncodeCertExtensionValue(szOID_BASIC_CONSTRAINTS2, &basicConstraints);

        BYTE keyUsageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE | CERT_KEY_CERT_SIGN_KEY_USAGE;
        CRYPT_BIT_BLOB keyUsage{};
        keyUsage.cbData = 1;
        keyUsage.pbData = &keyUsageBits;
        auto kuEncoded = EncodeCertExtensionValue(szOID_KEY_USAGE, &keyUsage);

        LPSTR serverAuthOid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        CERT_ENHKEY_USAGE enhKeyUsage{1, &serverAuthOid};
        auto ekuEncoded = EncodeCertExtensionValue(szOID_ENHANCED_KEY_USAGE, &enhKeyUsage);

        CERT_EXTENSION extensions[4]{};
        extensions[0] = {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE, {static_cast<DWORD>(sanEncoded.size()), sanEncoded.data()}};
        extensions[1] = {const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2), TRUE, {static_cast<DWORD>(bcEncoded.size()), bcEncoded.data()}};
        extensions[2] = {const_cast<LPSTR>(szOID_KEY_USAGE), TRUE, {static_cast<DWORD>(kuEncoded.size()), kuEncoded.data()}};
        extensions[3] = {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {static_cast<DWORD>(ekuEncoded.size()), ekuEncoded.data()}};
        CERT_EXTENSIONS certExtensions{ARRAYSIZE(extensions), extensions};

        // Subject / issuer name.
        const auto subject = L"CN=" + wsl::shared::string::MultiByteToWide(IpAddress);
        DWORD nameSize = 0;
        THROW_IF_WIN32_BOOL_FALSE(CertStrToNameW(X509_ASN_ENCODING, subject.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &nameSize, nullptr));
        std::vector<BYTE> nameBlob(nameSize);
        THROW_IF_WIN32_BOOL_FALSE(
            CertStrToNameW(X509_ASN_ENCODING, subject.c_str(), CERT_X500_NAME_STR, nullptr, nameBlob.data(), &nameSize, nullptr));
        CERT_NAME_BLOB subjectBlob{nameSize, nameBlob.data()};

        CRYPT_KEY_PROV_INFO keyProvInfo{};
        keyProvInfo.pwszContainerName = const_cast<LPWSTR>(keyName.c_str());
        keyProvInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);

        CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm{};
        signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = nowFt.dwLowDateTime;
        ticks.HighPart = nowFt.dwHighDateTime;
        ticks.QuadPart += 100ULL * 24 * 60 * 60 * 10'000'000; // 100 days, in 100-ns intervals
        FILETIME endFt{ticks.LowPart, ticks.HighPart};
        SYSTEMTIME notAfter{};
        THROW_IF_WIN32_BOOL_FALSE(FileTimeToSystemTime(&endFt, &notAfter));

        wil::unique_cert_context cert{CertCreateSelfSignCertificate(
            key, &subjectBlob, 0, &keyProvInfo, &signatureAlgorithm, nullptr, &notAfter, &certExtensions)};
        THROW_LAST_ERROR_IF_NULL(cert);

        // Export server.crt and server.key
        const auto certPem = ToPem(cert.get()->pbCertEncoded, cert.get()->cbCertEncoded, "CERTIFICATE");

        DWORD keyBlobSize = 0;
        THROW_IF_FAILED(NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0, &keyBlobSize, 0));
        std::vector<BYTE> keyBlob(keyBlobSize);
        THROW_IF_FAILED(NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, keyBlob.data(), keyBlobSize, &keyBlobSize, 0));
        const auto keyPem = ToPem(keyBlob.data(), keyBlobSize, "PRIVATE KEY");

        const auto writeFile = [](const std::filesystem::path& path, const std::string& contents) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            THROW_HR_IF_MSG(E_FAIL, !stream.is_open(), "Failed to open %ls for writing", path.c_str());
            stream.write(contents.data(), contents.size());
        };
        writeFile(OutDir / "server.crt", certPem);
        writeFile(OutDir / "server.key", keyPem);

        return cert;
    }

    // Helper that adds a certificate to the machine's Trusted Root store and removes it on destruction.
    class TrustedRootCertificate
    {
    public:
        NON_COPYABLE(TrustedRootCertificate);
        NON_MOVABLE(TrustedRootCertificate);

        explicit TrustedRootCertificate(const CERT_CONTEXT& Cert)
        {
            m_store.reset(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, NULL, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT"));
            THROW_LAST_ERROR_IF_NULL(m_store);

            PCCERT_CONTEXT added{};
            THROW_IF_WIN32_BOOL_FALSE(CertAddEncodedCertificateToStore(
                m_store.get(), X509_ASN_ENCODING, Cert.pbCertEncoded, Cert.cbCertEncoded, CERT_STORE_ADD_REPLACE_EXISTING, &added));
            m_added.reset(added);
        }

        ~TrustedRootCertificate()
        {
            if (m_added)
            {
                // CertDeleteCertificateFromStore frees the context; the store is still open here.
                LOG_IF_WIN32_BOOL_FALSE(CertDeleteCertificateFromStore(m_added.release()));
            }
        }

    private:
        wil::unique_hcertstore m_store;
        wil::unique_cert_context m_added;
    };

    // Starts a TLS-enabled registry container on the docker bridge (so dockerd reaches it at a
    // non-loopback IP and performs real TLS verification). The certs in HostCertDir are mounted at
    // /certs. Returns the running container and its "ip:port" address.
    std::pair<RunningWSLCContainer, std::string> StartLocalRegistryTls(IWSLCSession& Session, const std::wstring& HostCertDir)
    {
        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        THROW_IF_FAILED(Session.ListImages(nullptr, &images, images.size_address<ULONG>()));
        const bool found = std::ranges::any_of(std::span{images.get(), images.size()}, [](const auto& e) {
            return std::strcmp(e.Image, "wslc-registry:latest") == 0;
        });
        if (!found)
        {
            LoadTestImage(Session, "wslc-registry:latest");
        }

        const std::vector<std::string> env = {
            std::format("REGISTRY_HTTP_ADDR=0.0.0.0:{}", c_registryPort),
            "REGISTRY_HTTP_TLS_CERTIFICATE=/certs/server.crt",
            "REGISTRY_HTTP_TLS_KEY=/certs/server.key",
        };

        WSLCContainerLauncher launcher("wslc-registry:latest", "", {}, env, "bridge");
        launcher.SetEntrypoint({"/entrypoint.sh"});
        launcher.AddVolume(HostCertDir, "/certs", true);

        auto container = launcher.Launch(Session, WSLCContainerStartFlagsNone);

        // Resolve the container's bridge IP.
        auto inspect = container.Inspect();
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.empty());
        const auto ip = inspect.NetworkSettings.Networks.begin()->second.IPAddress;

        return {std::move(container), std::format("{}:{}", ip, c_registryPort)};
    }

    // Pushes via the CLI, retrying while the registry is still coming up (connection refused / no route),
    // so the returned result reflects a real response (success or a TLS error) rather than a startup race.
    WSLCExecutionResult PushWithRetry(const std::wstring& ImageRef, const std::wstring& SessionName)
    {
        WSLCExecutionResult result;
        for (int attempt = 0; attempt < 30; ++attempt)
        {
            result = RunWslc(std::format(L"push {} --session {}", ImageRef, SessionName));
            const auto& err = result.Stderr.value_or(L"");
            // Only a connection-level error means "registry not up yet". Do NOT treat "EOF" as
            // transient: an untrusted-CA rejection can surface as EOF, and retrying would mask the
            // very failure Phase 1 asserts on.
            const bool notReady =
                err.find(L"connection refused") != std::wstring::npos || err.find(L"no route to host") != std::wstring::npos;
            if (!notReady)
            {
                break;
            }
            Sleep(1000);
        }
        return result;
    }

} // namespace

class WSLCE2ETlsRegistryTests
{
    WSLC_TEST_CLASS(WSLCE2ETlsRegistryTests)

    // Verifies private-registry SSL trust end to end: a push to a registry serving a private-CA cert
    // fails with "unknown authority" until the CA is added to the machine Trusted Root store, after
    // which a push from a freshly created session (which mirrors the host roots into the UVM) succeeds.
    WSLC_TEST_METHOD(WSLCE2E_Registry_PrivateCa_SslTrust)
    {
        const auto& image = AlpineTestImage();
        const auto registryImage =
            std::format(L"{}:{}/{}", wsl::shared::string::MultiByteToWide(c_registryIp), c_registryPort, image.NameAndTag());

        // Generate the registry's CA/cert (SAN = the bridge IP) into a temp directory mounted into the container.
        const auto certDir = std::filesystem::temp_directory_path() / std::format(L"wslc-tls-registry-{}", GetCurrentProcessId());
        std::filesystem::create_directories(certDir);
        auto removeCertDir = wil::scope_exit([&] {
            std::error_code ec;
            std::filesystem::remove_all(certDir, ec);
        });

        auto caCert = GenerateRegistryTlsCertificate(c_registryIp, certDir);

        // Phase 1: CA NOT trusted -> the push must fail because the cert chains to an untrusted root.
        {
            // Defensively terminate any session left behind by a previously crashed run so this one
            // creates a fresh session (and the registry reclaims the expected bridge IP).
            EnsureSessionIsTerminated(L"wslc-tls-untrusted");
            auto session = TestSession::Create(L"wslc-tls-untrusted");
            EnsureImageIsLoaded(image, session.Name());

            auto [registry, address] = StartLocalRegistryTls(session.Session(), certDir.wstring());
            VERIFY_ARE_EQUAL(std::format("{}:{}", c_registryIp, c_registryPort), address);

            RunWslcAndVerify(std::format(L"image tag {} {} --session {}", image.NameAndTag(), registryImage, session.Name()), {.ExitCode = 0});

            auto result = PushWithRetry(registryImage, session.Name());
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0), L"Push should fail while the CA is not trusted");
            VERIFY_IS_TRUE(result.Stderr.has_value());
            VERIFY_IS_TRUE(
                result.Stderr->find(L"certificate signed by unknown authority") != std::wstring::npos,
                L"Expected an untrusted-certificate error");
        }

        // Trust the CA on the host. The session service mirrors LocalMachine\Root into the UVM at init.
        // The RAII object removes the cert from the store when it goes out of scope at the end of the test.
        TrustedRootCertificate trustedCa{*caCert.get()};

        // Phase 2: CA trusted + a fresh session (so the mirror includes it) -> the push must succeed.
        {
            EnsureSessionIsTerminated(L"wslc-tls-trusted");
            auto session = TestSession::Create(L"wslc-tls-trusted");
            EnsureImageIsLoaded(image, session.Name());

            auto [registry, address] = StartLocalRegistryTls(session.Session(), certDir.wstring());
            VERIFY_ARE_EQUAL(std::format("{}:{}", c_registryIp, c_registryPort), address);

            RunWslcAndVerify(std::format(L"image tag {} {} --session {}", image.NameAndTag(), registryImage, session.Name()), {.ExitCode = 0});

            auto result = PushWithRetry(registryImage, session.Name());
            VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1), L"Push should succeed once the CA is trusted");
        }
    }
};

} // namespace WSLCE2ETests
