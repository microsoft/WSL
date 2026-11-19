/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ETlsRegistryTests.cpp

Abstract:

    End-to-end tests for private-registry TLS trust and session proxy routing.

--*/

#include "precomp.h"
#include "Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "WSLCSessionDefaults.h"
#include "TestImageRegistry.h"
#include <wslutil.h>
#include <ncrypt.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

namespace wslutil = wsl::windows::common::wslutil;

namespace {
    // The bridge IP assigned to the first container started in a fresh session. The registry is always
    // the first container we start in this test's session, so this is deterministic.
    constexpr auto c_registryIp = "172.17.0.2";
    constexpr USHORT c_registryPort = 5000;
    constexpr auto c_proxyRegistryHost = "host.wslc.internal";
    constexpr USHORT c_proxyRegistryPort = 15011;
    constexpr USHORT c_proxyPort = 15012;
    constexpr USHORT c_changedProxyPort = 15013;
    constexpr auto c_proxyTargetHost = "registry.proxy.invalid";
    constexpr auto c_proxyTokenHost = "token.proxy.invalid";
    const auto c_registryConnect = std::format("CONNECT {}:{} ", c_proxyTargetHost, c_registryPort);

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
    wil::unique_cert_context GenerateRegistryTlsCertificate(
        const std::string& IpAddress, const std::filesystem::path& OutDir, const std::wstring& DnsName = L"")
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
        CERT_ALT_NAME_ENTRY altEntries[2]{};
        altEntries[0].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        altEntries[0].IPAddress.cbData = sizeof(ipv4);
        altEntries[0].IPAddress.pbData = reinterpret_cast<BYTE*>(&ipv4);
        altEntries[1].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        altEntries[1].pwszDNSName = const_cast<LPWSTR>(DnsName.c_str());
        CERT_ALT_NAME_INFO altInfo{DnsName.empty() ? 1u : 2u, altEntries};
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

    wsl::windows::common::RunningWSLCContainer StartProxyServer(IWSLCSession& session, const std::filesystem::path& requestsPath, USHORT port)
    {
        wsl::windows::common::WSLCContainerLauncher launcher(
            string::WideToMultiByte(PythonTestImage().NameAndTag()),
            {},
            {"sh",
             "-c",
             std::format(
                 "exec {} --bind 0.0.0.0 --directory /tmp 2>/test/{}",
                 string::WideToMultiByte(GetPythonHttpServerScript(port)),
                 requestsPath.filename().string())},
            {},
            "bridge");
        launcher.AddVolume(requestsPath.parent_path().wstring(), "/test", false);
        launcher.AddPort(port, port, AF_INET, IPPROTO_TCP, "127.0.0.1");
        auto server = launcher.Launch(session);
        auto process = server.GetInitProcess();
        WaitForOutput(process.GetStdHandle(1), "Serving HTTP on");
        VERIFY_IS_TRUE(std::filesystem::exists(requestsPath));
        return server;
    }

    size_t CountProxyRequests(const std::filesystem::path& requestsPath, std::string_view request)
    {
        std::ifstream log(requestsPath);
        VERIFY_IS_TRUE(log.is_open());

        // The user proxy can also receive unrelated requests from the host.
        const auto marker = std::format("] \"{}", request);
        size_t count = 0;
        for (std::string line; std::getline(log, line);)
        {
            if (line.find(marker) != std::string::npos)
            {
                ++count;
            }
        }

        VERIFY_IS_FALSE(log.bad());
        return count;
    }

    void PullAndVerifyProxyRequest(const std::filesystem::path& requestsPath)
    {
        // The HTTP server records CONNECT without implementing a proxy tunnel.
        const auto expectedRequest = c_registryConnect + "HTTP/1.1\" 501";
        const auto before = CountProxyRequests(requestsPath, expectedRequest);
        const auto result =
            RunWslc(std::format(L"image pull {}:{}/proxy-test:latest", string::MultiByteToWide(c_proxyTargetHost), c_registryPort));
        result.Verify({.ExitCode = 1});
        VERIFY_IS_GREATER_THAN(CountProxyRequests(requestsPath, expectedRequest), before);
    }

    std::string ReadBootId(IWSLCSession& session)
    {
        wsl::windows::common::WSLCProcessLauncher launcher("/bin/cat", {"/bin/cat", "/proc/sys/kernel/random/boot_id"});
        auto process = launcher.Launch(session);
        const auto result = process.WaitAndCaptureOutput(30000);
        VERIFY_ARE_EQUAL(0, result.Code);
        const auto id = result.Output.at(1);
        VERIFY_IS_FALSE(id.empty());
        return id;
    }

    void SetWindowsProxySettings(const std::string& proxy, const std::string& bypasses)
    {
        LxsstuLaunchPowershellAndCaptureOutput(std::format(
            L"Set-WinhttpProxy -SettingScope User -Proxy \\\"{}\\\" -ProxyBypass \\\"{}\\\" -AutoconfigUrl \\\"\\\"",
            string::MultiByteToWide(proxy),
            string::MultiByteToWide(bypasses)));
    }

    template <typename T>
    void WithColdProxySession(
        const std::filesystem::path& storagePath,
        const std::string& proxy,
        const std::string& bypasses,
        T&& verify,
        const std::vector<std::pair<std::wstring, std::wstring>>& overrides = {})
    {
        ScopedEnvVariable httpProxy{L"HTTP_PROXY"};
        ScopedEnvVariable httpsProxy{L"HTTPS_PROXY"};
        ScopedEnvVariable noProxy{L"NO_PROXY"};
        for (const auto& [name, value] : overrides)
        {
            VERIFY_IS_TRUE(SetEnvironmentVariableW(name.c_str(), value.c_str()));
        }

        ScopedWindowsProxySettings originalSettings;
        SetWindowsProxySettings(proxy, bypasses);
        EnsureSessionIsTerminated();
        auto terminate = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { EnsureSessionIsTerminated(); });
        const auto settingsPath = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"settings.yaml";
        HostFileChange settings(
            settingsPath,
            std::format(
                "session:\n  cpuCount: 2\n  memorySize: 2GB\n  maxStorageSize: 4GB\n  idleTimeout: 600\n  storagePath: {}\n",
                nlohmann::json(string::WideToMultiByte(storagePath.wstring())).dump()));

        // A new storage location emits a warning; the image listing itself must be empty.
        RunWslc(L"image list --format json").Verify({.Stdout = L"", .ExitCode = 0});
        auto session = OpenDefaultElevatedSession();
        verify(*session, settings);
    }

    template <typename T>
    void WithProxyServer(T&& verify)
    {
        GUID guid{};
        VERIFY_SUCCEEDED(CoCreateGuid(&guid));
        const auto name = L"wslc-proxy-" + string::GuidToString<wchar_t>(guid, string::GuidToStringFlags::None);
        const auto directory = std::filesystem::current_path() / name;
        auto cleanupDirectory = SetupTestDirectory(directory);
        ScopedWindowsProxySettings originalSettings;
        SetWindowsProxySettings("", "");
        auto producer = TestSession::Create(name, WSLCNetworkingModeConsomme);
        TestImageRegistry::Instance().EnsureLoaded(PythonTestImage(), producer.Name());
        const auto requestsPath = directory / L"proxy.log";
        auto proxy = StartProxyServer(producer.Session(), requestsPath, c_proxyPort);
        verify(directory, producer.Session(), requestsPath);
    }

} // namespace

class WSLCE2ETlsRegistryTests
{
    WSLC_TEST_CLASS(WSLCE2ETlsRegistryTests)

    WSLC_TEST_METHOD(WSLCE2E_Proxy_RestoresWindowsSettingsOnFailure)
    {
        WINHTTP_PROXY_TEST_ONLY();
        ScopedWindowsProxySettings originalSettings;
        LxsstuLaunchPowershellAndCaptureOutput(
            L"Set-WinhttpProxy -SettingScope User -Proxy 'http=original.proxy.invalid:3128;https=secure.proxy.invalid:3129' "
            L"-ProxyBypass '<local>;*.original.invalid' -AutoconfigUrl 'http://127.0.0.1:9/original.pac?token=synthetic' "
            L"-AutoDetect");
        const auto expected = LxsstuLaunchPowershellAndCaptureOutput(L"netsh winhttp show advproxy").first;
        const auto directory = std::filesystem::current_path() / std::format(L"wslc-proxy-restore-{}", GetCurrentProcessId());
        auto cleanup = SetupTestDirectory(directory);

        VERIFY_ARE_EQUAL(E_ABORT, wil::ResultFromException([&]() {
                             WithColdProxySession(
                                 directory / L"consumer",
                                 "http://windows.proxy.invalid:3128",
                                 ".windows.invalid",
                                 [&](IWSLCSession&, const HostFileChange&) { THROW_HR(E_ABORT); });
                         }));

        VERIFY_ARE_EQUAL(expected, LxsstuLaunchPowershellAndCaptureOutput(L"netsh winhttp show advproxy").first);
    }

    WSLC_TEST_METHOD(WSLCE2E_Proxy_ContainerEnvironment)
    {
        WINHTTP_PROXY_TEST_ONLY();
        const auto directory = std::filesystem::current_path() / std::format(L"wslc-proxy-env-{}", GetCurrentProcessId());
        auto cleanup = SetupTestDirectory(directory);
        WithColdProxySession(
            directory / L"consumer",
            "http://windows.proxy.invalid:3128",
            ".windows.invalid",
            [&](IWSLCSession&, const HostFileChange&) {
                const auto& image = AlpineTestImage();
                RunWslcAndVerify(std::format(L"image load --input \"{}\"", image.Path.wstring()), {.Stderr = L"", .ExitCode = 0});
                const auto defaults = RunWslcAndGetStdoutLineSet(std::format(L"container run --rm {} env", image.NameAndTag()));
                for (const auto* name : {L"http_proxy", L"HTTP_PROXY", L"https_proxy", L"HTTPS_PROXY"})
                {
                    VERIFY_IS_TRUE(defaults.contains(std::format(L"{}=http://windows.proxy.invalid:3128", name)));
                }
                VERIFY_IS_TRUE(defaults.contains(L"no_proxy=.windows.invalid"));
                VERIFY_IS_TRUE(defaults.contains(L"NO_PROXY=.windows.invalid"));

                for (const bool upper : {false, true})
                {
                    const auto* http = upper ? L"HTTP_PROXY" : L"http_proxy";
                    const auto* https = upper ? L"HTTPS_PROXY" : L"https_proxy";
                    const auto* noProxy = upper ? L"NO_PROXY" : L"no_proxy";
                    const auto environment = RunWslcAndGetStdoutLineSet(std::format(
                        L"container run --rm -e {}=127.0.0.1:8080 -e {}= -e {}= {} env", http, https, noProxy, image.NameAndTag()));
                    VERIFY_IS_TRUE(environment.contains(std::format(L"{}=127.0.0.1:8080", http)));
                    VERIFY_IS_TRUE(environment.contains(std::format(L"{}=", https)));
                    VERIFY_IS_TRUE(environment.contains(std::format(L"{}=", noProxy)));
                    for (const auto* name :
                         {upper ? L"http_proxy" : L"HTTP_PROXY", upper ? L"https_proxy" : L"HTTPS_PROXY", upper ? L"no_proxy" : L"NO_PROXY"})
                    {
                        VERIFY_IS_FALSE(std::ranges::any_of(
                            environment, [&](const auto& entry) { return entry.starts_with(std::format(L"{}=", name)); }));
                    }
                }

                const auto imported =
                    RunWslcAndGetStdoutLineSet(std::format(L"container run --rm -e http_proxy -e no_proxy {} env", image.NameAndTag()));
                VERIFY_IS_TRUE(imported.contains(L"http_proxy=http://caller.proxy.invalid:8080"));
                VERIFY_IS_TRUE(imported.contains(L"no_proxy=.caller.invalid"));
                VERIFY_IS_TRUE(imported.contains(L"HTTPS_PROXY=http://windows.proxy.invalid:3128"));
                for (const auto* name : {L"HTTP_PROXY", L"NO_PROXY"})
                {
                    VERIFY_IS_FALSE(std::ranges::any_of(
                        imported, [&](const auto& entry) { return entry.starts_with(std::format(L"{}=", name)); }));
                }

                const auto envFile = directory / L"proxy.env";
                WriteTestFileContent(envFile, "HTTP_PROXY=http://file.proxy.invalid:8080\nhttps_proxy=\nNO_PROXY=.file.invalid\n");
                const auto fromFile = RunWslcAndGetStdoutLineSet(
                    std::format(L"container run --rm --env-file \"{}\" {} env", envFile.wstring(), image.NameAndTag()));
                VERIFY_IS_TRUE(fromFile.contains(L"HTTP_PROXY=http://file.proxy.invalid:8080"));
                VERIFY_IS_TRUE(fromFile.contains(L"https_proxy="));
                VERIFY_IS_TRUE(fromFile.contains(L"NO_PROXY=.file.invalid"));
                for (const auto* name : {L"http_proxy", L"HTTPS_PROXY", L"no_proxy"})
                {
                    VERIFY_IS_FALSE(std::ranges::any_of(
                        fromFile, [&](const auto& entry) { return entry.starts_with(std::format(L"{}=", name)); }));
                }

                WriteTestFileContent(
                    envFile,
                    "Http_Proxy=http://Manual.Proxy:8080\nhTtPs_PrOxY=\nNo_PrOxY=LocalHost,.Manual.Example\nCustom_Name=VaLuE\n");
                for (const auto& options :
                     {std::wstring{L"-e Http_Proxy=http://Manual.Proxy:8080 -e hTtPs_PrOxY= "
                                   L"-e No_PrOxY=LocalHost,.Manual.Example -e Custom_Name=VaLuE"},
                      std::format(L"--env-file \"{}\"", envFile.wstring())})
                {
                    const auto environment =
                        RunWslcAndGetStdoutLineSet(std::format(L"container run --rm {} {} env", options, image.NameAndTag()));
                    VERIFY_IS_TRUE(environment.contains(L"Http_Proxy=http://Manual.Proxy:8080"));
                    VERIFY_IS_TRUE(environment.contains(L"hTtPs_PrOxY="));
                    VERIFY_IS_TRUE(environment.contains(L"No_PrOxY=LocalHost,.Manual.Example"));
                    VERIFY_IS_TRUE(environment.contains(L"Custom_Name=VaLuE"));
                    for (const auto* name : {L"http_proxy", L"HTTP_PROXY", L"https_proxy", L"HTTPS_PROXY"})
                    {
                        VERIFY_IS_TRUE(environment.contains(std::format(L"{}=http://windows.proxy.invalid:3128", name)));
                    }
                    VERIFY_IS_TRUE(environment.contains(L"no_proxy=.windows.invalid"));
                    VERIFY_IS_TRUE(environment.contains(L"NO_PROXY=.windows.invalid"));
                }
            },
            {{L"Http_Proxy", L"http://caller.proxy.invalid:8080"}, {L"HTTPS_PROXY", L""}, {L"NO_PROXY", L".caller.invalid"}});
    }

    WSLC_TEST_METHOD(WSLCE2E_Proxy_SessionEnterIgnoresCallerEnvironment)
    {
        WINHTTP_PROXY_TEST_ONLY();
        const auto directory = std::filesystem::current_path() / std::format(L"wslc-proxy-enter-{}", GetCurrentProcessId());
        auto cleanup = SetupTestDirectory(directory);
        const auto storagePath = directory / L"consumer";
        WithColdProxySession(
            storagePath,
            "http://windows.proxy.invalid:3128",
            ".windows.invalid",
            [&](IWSLCSession& defaultSession, const HostFileChange&) {
                const auto& image = AlpineTestImage();
                RunWslcAndVerify(std::format(L"image load --input \"{}\"", image.Path.wstring()), {.Stderr = L"", .ExitCode = 0});
                wil::unique_cotaskmem_string defaultName;
                VERIFY_SUCCEEDED(defaultSession.GetDisplayName(defaultName.put()));
                const auto defaultStoragePath = storagePath / wsl::windows::wslc::DefaultStorageSubPath / defaultName.get();
                EnsureSessionIsTerminated();

                constexpr auto sessionName = L"wslc-proxy-enter";
                auto terminate = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { EnsureSessionIsTerminated(sessionName); });
                auto session =
                    RunWslcInteractive(std::format(L"system session enter \"{}\" --name {}", defaultStoragePath.wstring(), sessionName));
                session.ExpectStdout(VT::SESSION_PROMPT);
                const auto environment =
                    RunWslcAndGetStdoutLineSet(std::format(L"--session {} container run --rm {} env", sessionName, image.NameAndTag()));
                for (const auto* name : {L"http_proxy", L"HTTP_PROXY", L"https_proxy", L"HTTPS_PROXY"})
                {
                    VERIFY_IS_TRUE(environment.contains(std::format(L"{}=http://windows.proxy.invalid:3128", name)));
                }
                VERIFY_IS_TRUE(environment.contains(L"no_proxy=.windows.invalid"));
                VERIFY_IS_TRUE(environment.contains(L"NO_PROXY=.windows.invalid"));
                VERIFY_ARE_EQUAL(0, session.Exit());
            },
            {{L"HTTP_PROXY", L"http://caller.proxy.invalid:8080"}, {L"https_proxy", L""}, {L"NO_PROXY", L".caller.invalid"}});
    }

    WSLC_TEST_METHOD(WSLCE2E_Proxy_ContainerEnvironmentRefresh)
    {
        WINHTTP_PROXY_TEST_ONLY();
        const auto directory = std::filesystem::current_path() / std::format(L"wslc-proxy-refresh-{}", GetCurrentProcessId());
        auto cleanup = SetupTestDirectory(directory);
        WithColdProxySession(directory / L"consumer", "http://windows.proxy.invalid:3128", ".windows.invalid", [&](IWSLCSession& session, const HostFileChange&) {
            const auto& image = AlpineTestImage();
            RunWslcAndVerify(std::format(L"image load --input \"{}\"", image.Path.wstring()), {.Stderr = L"", .ExitCode = 0});
            RunWslcAndVerify(std::format(L"container create --name original-proxy {} env", image.NameAndTag()), {.Stderr = L"", .ExitCode = 0});
            const auto bootId = ReadBootId(session);
            ULONG sessionId{};
            VERIFY_SUCCEEDED(session.GetId(&sessionId));

            constexpr std::array proxyNames{
                L"http_proxy", L"HTTP_PROXY", L"https_proxy", L"HTTPS_PROXY", L"no_proxy", L"NO_PROXY"};
            for (const bool configured : {true, false})
            {
                SetWindowsProxySettings(configured ? "http://changed.proxy.invalid:3129" : "", configured ? ".changed.invalid" : "");

                // WinHTTP notifies asynchronously, and each container keeps its creation-time environment.
                const auto deadline = std::chrono::steady_clock::now() + 30s;
                for (;;)
                {
                    const auto environment = RunWslcAndGetStdoutLineSet(std::format(L"container run --rm {} env", image.NameAndTag()));
                    VERIFY_IS_FALSE(environment.empty());
                    const auto mismatch = std::ranges::find_if_not(proxyNames, [&](const auto* name) {
                        if (configured)
                        {
                            const auto* value = _wcsicmp(name, L"no_proxy") == 0 ? L".changed.invalid" : L"http://changed.proxy.invalid:3129";
                            return environment.contains(std::format(L"{}={}", name, value));
                        }

                        const auto prefix = std::format(L"{}=", name);
                        return std::ranges::none_of(environment, [&](const auto& entry) { return entry.starts_with(prefix); });
                    });
                    if (mismatch == proxyNames.end())
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        VERIFY_FAIL(std::format(L"Timed out waiting for {} to {}", *mismatch, configured ? L"change" : L"clear").c_str());
                        break;
                    }

                    std::this_thread::sleep_for(500ms);
                }

                ULONG currentSessionId{};
                VERIFY_SUCCEEDED(OpenDefaultElevatedSession()->GetId(&currentSessionId));
                VERIFY_ARE_EQUAL(sessionId, currentSessionId);
                VERIFY_ARE_EQUAL(bootId, ReadBootId(session));
            }

            const auto originalEnvironment = RunWslcAndGetStdoutLineSet(L"container start --attach original-proxy");
            for (const auto* name : {L"http_proxy", L"HTTP_PROXY", L"https_proxy", L"HTTPS_PROXY"})
            {
                VERIFY_IS_TRUE(originalEnvironment.contains(std::format(L"{}=http://windows.proxy.invalid:3128", name)));
            }
            VERIFY_IS_TRUE(originalEnvironment.contains(L"no_proxy=.windows.invalid"));
            VERIFY_IS_TRUE(originalEnvironment.contains(L"NO_PROXY=.windows.invalid"));
        });
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_IgnoresCallerEnvironment)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WithProxyServer([](const auto& directory, IWSLCSession&, const auto& requestsPath) {
            WithColdProxySession(
                directory / L"consumer",
                std::format("http://127.0.0.1:{}", c_proxyPort),
                ".windows.invalid",
                [&](IWSLCSession&, const HostFileChange&) { PullAndVerifyProxyRequest(requestsPath); },
                {{L"HTTP_PROXY", L"http://caller.proxy.invalid:8080"},
                 {L"https_proxy", L"http://secure.caller.proxy.invalid:8081"},
                 {L"NO_PROXY", string::MultiByteToWide(c_proxyTargetHost)}});
        });
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_BuildTokenAuthentication)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WithProxyServer([](const auto& directory, IWSLCSession& producer, const auto& requestsPath) {
            auto certificate = GenerateRegistryTlsCertificate("127.0.0.1", directory, string::MultiByteToWide(c_proxyRegistryHost));
            TrustedRootCertificate trustedCa{*certificate.get()};
            WriteTestFileContent(directory / L"registry-auth.py", R"py(import http.server
import ssl
import sys

realm, port = sys.argv[1:]

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(401)
        self.send_header("WWW-Authenticate", f'Bearer realm="{realm}",service="wslc-proxy-test"')
        self.end_headers()

    do_HEAD = do_GET

server = http.server.ThreadingHTTPServer(("0.0.0.0", int(port)), Handler)
server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
server_tls.load_cert_chain("/test/server.crt", "/test/server.key")
server.socket = server_tls.wrap_socket(server.socket, server_side=True)
print("Token registry ready", flush=True)
server.serve_forever()
)py");
            wsl::windows::common::WSLCContainerLauncher launcher(
                string::WideToMultiByte(PythonTestImage().NameAndTag()),
                {},
                {"python3", "-u", "/test/registry-auth.py", std::format("http://{}:{}/token", c_proxyTokenHost, c_proxyRegistryPort), std::to_string(c_proxyRegistryPort)},
                {},
                "bridge");
            launcher.AddVolume(directory.wstring(), "/test", true);
            launcher.AddPort(c_proxyRegistryPort, c_proxyRegistryPort, AF_INET, IPPROTO_TCP, "127.0.0.1");
            auto registry = launcher.Launch(producer);
            auto process = registry.GetInitProcess();
            WaitForOutput(process.GetStdHandle(1), "Token registry ready");

            const auto context = directory / L"context";
            VERIFY_IS_TRUE(std::filesystem::create_directory(context));
            WriteTestFileContent(context / L"Dockerfile", std::format("FROM {}:{}/proxy-test:latest\n", c_proxyRegistryHost, c_proxyRegistryPort));
            const auto command = std::format(L"build \"{}\" --pull --no-cache -t wslc-proxy-build:latest", context.wstring());
            const auto tokenRequest = std::format("GET http://{}:{}/token?", c_proxyTokenHost, c_proxyRegistryPort);
            const auto bypassedRegistryRequest = std::format("CONNECT {}:{} ", c_proxyRegistryHost, c_proxyRegistryPort);

            for (const bool bypassToken : {false, true})
            {
                WithColdProxySession(
                    directory / (bypassToken ? L"bypass" : L"proxied"),
                    std::format("http://localhost:{}", c_proxyPort),
                    bypassToken ? std::format("{};{}", c_proxyRegistryHost, c_proxyTokenHost) : c_proxyRegistryHost,
                    [&](IWSLCSession& session, const HostFileChange&) {
                        const auto bootId = ReadBootId(session);
                        const auto before = CountProxyRequests(requestsPath, tokenRequest);
                        const auto result = RunWslc(command);
                        result.Verify({.ExitCode = 1});
                        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"failed to fetch anonymous token"));
                        VERIFY_IS_TRUE(result.StderrContainsSubstring(string::MultiByteToWide(c_proxyTokenHost)));
                        const auto originalRequests = CountProxyRequests(requestsPath, tokenRequest);
                        if (bypassToken)
                        {
                            VERIFY_ARE_EQUAL(before, originalRequests);
                        }
                        else
                        {
                            VERIFY_IS_TRUE(result.StderrContainsSubstring(L"404"));
                            VERIFY_IS_GREATER_THAN(originalRequests, before);

                            const auto changedRequestsPath = directory / L"changed-proxy.log";
                            auto changedProxy = StartProxyServer(producer, changedRequestsPath, c_changedProxyPort);
                            SetWindowsProxySettings(std::format("http://localhost:{}", c_changedProxyPort), c_proxyRegistryHost);
                            const auto changedBefore = CountProxyRequests(changedRequestsPath, tokenRequest);
                            const auto changed = RunWslc(command);
                            changed.Verify({.ExitCode = 1});
                            VERIFY_IS_TRUE(changed.StderrContainsSubstring(L"failed to fetch anonymous token"));
                            VERIFY_IS_TRUE(changed.StderrContainsSubstring(L"404"));
                            const auto changedRequests = CountProxyRequests(changedRequestsPath, tokenRequest);
                            VERIFY_IS_GREATER_THAN(changedRequests, changedBefore);
                            VERIFY_ARE_EQUAL(originalRequests, CountProxyRequests(requestsPath, tokenRequest));

                            SetWindowsProxySettings("", "");
                            const auto cleared = RunWslc(command);
                            cleared.Verify({.ExitCode = 1});
                            VERIFY_IS_TRUE(cleared.StderrContainsSubstring(L"failed to fetch anonymous token"));
                            VERIFY_IS_TRUE(cleared.StderrContainsSubstring(string::MultiByteToWide(c_proxyTokenHost)));
                            VERIFY_ARE_EQUAL(originalRequests, CountProxyRequests(requestsPath, tokenRequest));
                            VERIFY_ARE_EQUAL(changedRequests, CountProxyRequests(changedRequestsPath, tokenRequest));
                            VERIFY_ARE_EQUAL(size_t{0}, CountProxyRequests(changedRequestsPath, bypassedRegistryRequest));
                            VERIFY_ARE_EQUAL(bootId, ReadBootId(session));
                        }

                        VERIFY_ARE_EQUAL(size_t{0}, CountProxyRequests(requestsPath, bypassedRegistryRequest));
                        RunWslc(L"image list --format json").Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
                    });
            }
        });
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_ColdPullAndIdleRestart)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WithProxyServer([](const auto& directory, IWSLCSession& producer, const auto& requestsPath) {
            WithColdProxySession(
                directory / L"localhost", std::format("http://localhost:{}", c_proxyPort), "", [&](IWSLCSession& session, const HostFileChange& settings) {
                    PullAndVerifyProxyRequest(requestsPath);
                    const auto originalRequests = CountProxyRequests(requestsPath, c_registryConnect);
                    const auto bootId = ReadBootId(session);
                    ULONG sessionId{};
                    VERIFY_SUCCEEDED(session.GetId(&sessionId));

                    const auto changedRequestsPath = directory / L"changed-proxy.log";
                    auto changedProxy = StartProxyServer(producer, changedRequestsPath, c_changedProxyPort);
                    SetWindowsProxySettings(std::format("http://localhost:{}", c_changedProxyPort), "");
                    settings.Update("session: {}\n");
                    BOOL wasAlreadyIdle = TRUE;
                    VERIFY_SUCCEEDED(session.TriggerIdleTermination(&wasAlreadyIdle));
                    VERIFY_IS_FALSE(wasAlreadyIdle);
                    VERIFY_ARE_NOT_EQUAL(bootId, ReadBootId(session));
                    ULONG restartedSessionId{};
                    VERIFY_SUCCEEDED(OpenDefaultElevatedSession()->GetId(&restartedSessionId));
                    VERIFY_ARE_EQUAL(sessionId, restartedSessionId);

                    PullAndVerifyProxyRequest(changedRequestsPath);
                    VERIFY_ARE_EQUAL(originalRequests, CountProxyRequests(requestsPath, c_registryConnect));

                    const auto changedRequests = CountProxyRequests(changedRequestsPath, c_registryConnect);
                    const auto restartedBootId = ReadBootId(session);
                    SetWindowsProxySettings("", "");
                    VERIFY_SUCCEEDED(session.TriggerIdleTermination(&wasAlreadyIdle));
                    VERIFY_IS_FALSE(wasAlreadyIdle);
                    VERIFY_ARE_NOT_EQUAL(restartedBootId, ReadBootId(session));
                    VERIFY_SUCCEEDED(OpenDefaultElevatedSession()->GetId(&restartedSessionId));
                    VERIFY_ARE_EQUAL(sessionId, restartedSessionId);
                    const auto result =
                        RunWslc(std::format(L"image pull {}:{}/proxy-test:latest", string::MultiByteToWide(c_proxyTargetHost), c_registryPort));
                    result.Verify({.ExitCode = 1});
                    VERIFY_IS_TRUE(result.StderrContainsSubstring(string::MultiByteToWide(c_proxyTargetHost)));
                    VERIFY_IS_FALSE(result.StderrContainsSubstring(L"proxyconnect tcp"));
                    VERIFY_ARE_EQUAL(originalRequests, CountProxyRequests(requestsPath, c_registryConnect));
                    VERIFY_ARE_EQUAL(changedRequests, CountProxyRequests(changedRequestsPath, c_registryConnect));
                });
        });
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_NoProxy)
    {
        WINHTTP_PROXY_TEST_ONLY();
        ScopedWindowsProxySettings originalSettings;
        SetWindowsProxySettings("http://windows.proxy.invalid:3128", "*.unrelated.invalid");
        const auto expected = LxsstuLaunchPowershellAndCaptureOutput(L"netsh winhttp show advproxy").first;

        WithProxyServer([](const auto& directory, IWSLCSession&, const auto& requestsPath) {
            for (const auto* noProxy : {"", ".unrelated.invalid", c_proxyTargetHost})
            {
                const bool unset = *noProxy == '\0';
                WithColdProxySession(
                    directory / (unset ? "unset" : noProxy),
                    unset ? "" : std::format("http://127.0.0.1:{}", c_proxyPort),
                    noProxy,
                    [&](IWSLCSession&, const HostFileChange&) {
                        if (!unset && std::string_view(noProxy) != c_proxyTargetHost)
                        {
                            PullAndVerifyProxyRequest(requestsPath);
                        }
                        else
                        {
                            const auto before = CountProxyRequests(requestsPath, c_registryConnect);
                            const auto result = RunWslc(std::format(
                                L"image pull {}:{}/proxy-test:latest", string::MultiByteToWide(c_proxyTargetHost), c_registryPort));
                            result.Verify({.ExitCode = 1});
                            VERIFY_IS_TRUE(result.StderrContainsSubstring(string::MultiByteToWide(c_proxyTargetHost)));
                            VERIFY_IS_FALSE(result.StderrContainsSubstring(L"proxyconnect tcp"));
                            VERIFY_ARE_EQUAL(before, CountProxyRequests(requestsPath, c_registryConnect));
                        }

                        RunWslc(L"image list --format json").Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
                    });
            }
        });

        VERIFY_ARE_EQUAL(expected, LxsstuLaunchPowershellAndCaptureOutput(L"netsh winhttp show advproxy").first);
    }

    // Verifies registry SSL trust end to end: a push to a registry serving a private-CA cert fails
    // with "unknown authority" and succeeds after the CA is added to the host Trusted Root store.
    WSLC_TEST_METHOD(WSLCE2E_Registry_PrivateCa_SslTrust)
    {
        const auto& image = AlpineTestImage();
        const auto registryImage =
            std::format(L"{}:{}/{}", wsl::shared::string::MultiByteToWide(c_registryIp), c_registryPort, image.NameAndTag());

        // Generate the registry's certs.
        const auto certDir = std::filesystem::temp_directory_path() / std::format(L"wslc-tls-registry-{}", GetCurrentProcessId());
        std::filesystem::create_directories(certDir);
        auto removeCertDir = wil::scope_exit([&] {
            std::error_code ec;
            std::filesystem::remove_all(certDir, ec);
        });

        auto caCert = GenerateRegistryTlsCertificate(c_registryIp, certDir);

        // CA NOT trusted: the push fails with unknown authority.
        {
            auto session = TestSession::Create(L"wslc-tls-untrusted");

            auto cleanup = wil::scope_exit([&] { EnsureSessionIsTerminated(L"wslc-tls-untrusted"); });

            TestImageRegistry::Instance().EnsureLoaded(image, session.Name());

            auto [registry, address] = StartLocalRegistry(session.Session(), "", "", c_registryPort, certDir.wstring());
            VERIFY_ARE_EQUAL(std::format("{}:{}", c_registryIp, c_registryPort), address);

            RunWslcAndVerify(
                std::format(L"--session \"{}\" image tag {} {}", session.Name(), image.NameAndTag(), registryImage), {.ExitCode = 0});

            auto result = RunWslc(std::format(L"--session \"{}\" push {}", session.Name(), registryImage));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0), L"Push should fail while the CA is not trusted");
            VERIFY_IS_TRUE(result.Stderr.has_value());
            VERIFY_IS_TRUE(
                result.Stderr->find(L"certificate signed by unknown authority") != std::wstring::npos,
                L"Expected an untrusted-certificate error");
        }

        // CA trusted: push succeeds.
        {
            // Trust the CA on the host.
            TrustedRootCertificate trustedCa{*caCert.get()};

            auto session = TestSession::Create(L"wslc-tls-trusted");

            auto cleanup = wil::scope_exit([&] { EnsureSessionIsTerminated(L"wslc-tls-trusted"); });

            TestImageRegistry::Instance().EnsureLoaded(image, session.Name());

            auto [registry, address] = StartLocalRegistry(session.Session(), "", "", c_registryPort, certDir.wstring());
            VERIFY_ARE_EQUAL(std::format("{}:{}", c_registryIp, c_registryPort), address);

            RunWslcAndVerify(
                std::format(L"--session \"{}\" image tag {} {}", session.Name(), image.NameAndTag(), registryImage), {.ExitCode = 0});

            auto result = RunWslc(std::format(L"--session \"{}\" push {}", session.Name(), registryImage));
            VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1), L"Push should succeed once the CA is trusted");
        }
    }
};

} // namespace WSLCE2ETests
