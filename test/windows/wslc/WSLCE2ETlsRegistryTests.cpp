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
#include "TestImageRegistry.h"
#include <atomic>
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
    constexpr auto c_proxyAuthRegistryHost = "registry.proxy.invalid";
    constexpr auto c_proxyTokenHost = "token.proxy.invalid";

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

    namespace io = wsl::windows::common::io;
    namespace socket = wsl::windows::common::socket;

    class CountingReadHandle : public io::ReadHandle
    {
    public:
        CountingReadHandle(io::HandleWrapper&& handle, std::function<void(const gsl::span<char>&)>&& onRead, std::atomic<uint64_t>& bytes) :
            ReadHandle(std::move(handle), [onRead = std::move(onRead), &bytes](const gsl::span<char>& buffer) {
                bytes.fetch_add(buffer.size(), std::memory_order_relaxed);
                onRead(buffer);
            })
        {
        }
    };

    class RecordingConnectProxy
    {
    public:
        NON_COPYABLE(RecordingConnectProxy);
        NON_MOVABLE(RecordingConnectProxy);

        explicit RecordingConnectProxy(bool tokenAuthentication = false) : m_tokenAuthentication(tokenAuthentication)
        {
            m_listener.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            THROW_LAST_ERROR_IF(!m_listener);

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            THROW_LAST_ERROR_IF(bind(m_listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
            THROW_LAST_ERROR_IF(listen(m_listener.get(), SOMAXCONN) == SOCKET_ERROR);
            int addressSize = sizeof(address);
            THROW_LAST_ERROR_IF(getsockname(m_listener.get(), reinterpret_cast<sockaddr*>(&address), &addressSize) == SOCKET_ERROR);
            m_port = ntohs(address.sin_port);

            m_thread = std::thread([this] {
                try
                {
                    while (!m_exit.is_signaled())
                    {
                        auto accepted = socket::CancellableAccept(m_listener.get(), INFINITE, m_exit.get());
                        if (!accepted)
                        {
                            break;
                        }

                        THROW_HR_IF(E_UNEXPECTED, m_connections.size() >= 64);
                        m_connections.emplace_back([this, client = std::move(*accepted)] {
                            try
                            {
                                Serve(client.get());
                            }
                            CATCH_LOG()
                        });
                    }
                }
                CATCH_LOG()
            });
        }

        ~RecordingConnectProxy()
        {
            Stop();
        }

        void Stop()
        {
            m_exit.SetEvent();
            if (m_thread.joinable())
            {
                m_thread.join();
            }

            for (auto& connection : m_connections)
            {
                if (connection.joinable())
                {
                    connection.join();
                }
            }

            m_listener.reset();
        }

        USHORT Port() const
        {
            return m_port;
        }

        uint64_t ResponseBytes() const
        {
            return m_responseBytes.load(std::memory_order_relaxed);
        }

        uint64_t TokenConnections() const
        {
            return m_tokenConnections.load(std::memory_order_relaxed);
        }

    private:
        void Serve(SOCKET client)
        {
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos)
            {
                char buffer[1024];
                const auto received = socket::Receive(client, gsl::as_writable_bytes(gsl::make_span(buffer)), m_exit.get(), 0, 10000);
                if (received == 0)
                {
                    return;
                }

                request.append(buffer, received);
                THROW_HR_IF(E_INVALIDARG, request.size() > 8192);
            }

            const auto registryHost = m_tokenAuthentication ? c_proxyAuthRegistryHost : c_proxyRegistryHost;
            const auto expected = std::format("CONNECT {}:{} HTTP/1.1\r\n", registryHost, c_proxyRegistryPort);
            const bool tokenRequest = m_tokenAuthentication &&
                                      request.starts_with(std::format("CONNECT {}:{} HTTP/1.1\r\n", c_proxyTokenHost, c_proxyRegistryPort));
            THROW_HR_IF(E_ACCESSDENIED, (!request.starts_with(expected) && !tokenRequest) || !request.ends_with("\r\n\r\n"));
            if (tokenRequest)
            {
                m_tokenConnections.fetch_add(1, std::memory_order_relaxed);
            }

            wil::unique_socket upstream{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
            THROW_LAST_ERROR_IF(!upstream);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(c_proxyRegistryPort);
            THROW_LAST_ERROR_IF(connect(upstream.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

            constexpr std::string_view response = "HTTP/1.1 200 Connection Established\r\n\r\n";
            socket::Send(client, gsl::as_bytes(gsl::make_span(response.data(), response.size())), m_exit.get());

            io::MultiHandleWait relay;
            // Half-close upstream on EOF and let its response drain before ending the tunnel.
            io::HandleWrapper clientInput{
                client, [socket = upstream.get()] { LOG_LAST_ERROR_IF(shutdown(socket, SD_SEND) == SOCKET_ERROR); }};
            auto requestRelay = std::make_unique<io::RelayHandle<>>(std::move(clientInput), upstream.get());
            relay.AddHandle(std::move(requestRelay), io::MultiHandleWait::NeedNotComplete);
            auto responseRelay = std::make_unique<io::RelayHandle<CountingReadHandle>>(upstream.get(), client, m_responseBytes);
            relay.AddHandle(std::move(responseRelay));
            const auto exitFlags = io::MultiHandleWait::CancelOnCompleted | io::MultiHandleWait::NeedNotComplete;
            relay.AddHandle(std::make_unique<io::EventHandle>(m_exit.get()), exitFlags);
            relay.Run(std::nullopt);
        }

        wil::unique_socket m_listener;
        wil::unique_event m_exit{wil::EventOptions::ManualReset};
        USHORT m_port{};
        bool m_tokenAuthentication{};
        std::atomic<uint64_t> m_responseBytes{};
        std::atomic<uint64_t> m_tokenConnections{};
        std::vector<std::thread> m_connections;
        std::thread m_thread;
    };

    struct PublishedRegistryImage
    {
        std::wstring Reference;
        std::string Id;
    };

    void PullAndVerifyImage(const PublishedRegistryImage& image)
    {
        RunWslcAndVerify(std::format(L"image pull {}", image.Reference), {.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(image.Id, InspectImage(image.Reference).Id);
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

    template <typename T>
    void WithColdProxySession(const std::filesystem::path& storagePath, const std::string& proxySettings, T&& verify)
    {
        EnsureSessionIsTerminated();
        auto terminate = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { EnsureSessionIsTerminated(); });
        const auto settingsPath = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"settings.yaml";
        HostFileChange settings(
            settingsPath,
            std::format(
                "session:\n  cpuCount: 2\n  memorySize: 2GB\n  maxStorageSize: 4GB\n  idleTimeout: 600\n  storagePath: {}\n{}",
                nlohmann::json(string::WideToMultiByte(storagePath.wstring())).dump(),
                proxySettings));

        // A new storage location emits a warning; the image listing itself must be empty.
        RunWslc(L"image list --format json").Verify({.Stdout = L"", .ExitCode = 0});
        auto session = OpenDefaultElevatedSession();
        verify(*session, settings);
    }

    template <typename T>
    void WithProxyRegistry(T&& verify, bool tokenAuthentication = false)
    {
        WSADATA data{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
        auto cleanupWinsock = wil::scope_exit([] { WSACleanup(); });

        GUID guid{};
        VERIFY_SUCCEEDED(CoCreateGuid(&guid));
        const auto name = L"wslc-proxy-" + string::GuidToString<wchar_t>(guid, string::GuidToStringFlags::None);
        const auto directory = std::filesystem::current_path() / name;
        auto cleanupDirectory = SetupTestDirectory(directory);
        const auto registryHost = tokenAuthentication ? c_proxyAuthRegistryHost : c_proxyRegistryHost;
        auto certificate = GenerateRegistryTlsCertificate(
            c_registryIp, directory, tokenAuthentication ? L"*.proxy.invalid" : string::MultiByteToWide(registryHost));
        TrustedRootCertificate trustedCa{*certificate.get()};

        auto producer = TestSession::Create(name, WSLCNetworkingModeConsomme);
        auto [registry, address] =
            StartLocalRegistry(producer.Session(), "", "", c_proxyRegistryPort, directory.wstring(), !tokenAuthentication);
        VERIFY_ARE_EQUAL(std::format("{}:{}", c_registryIp, c_proxyRegistryPort), address);

        const auto prefix = std::format(L"--session \"{}\" ", producer.Name());
        const auto publish = [&](const TestImage& image) {
            RunWslcAndVerify(std::format(L"{}image load --input \"{}\"", prefix, image.Path.wstring()), {.Stderr = L"", .ExitCode = 0});
            const auto id = InspectImage(image.NameAndTag(), producer.Name()).Id;
            const auto registryImage = std::format(L"{}/{}", string::MultiByteToWide(address), image.NameAndTag());
            RunWslcAndVerify(std::format(L"{}image tag {} {}", prefix, image.NameAndTag(), registryImage), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"{}push {}", prefix, registryImage), {.Stderr = L"", .ExitCode = 0});
            return PublishedRegistryImage{
                std::format(L"{}:{}/{}", string::MultiByteToWide(registryHost), c_proxyRegistryPort, image.NameAndTag()), id};
        };

        const auto alpine = publish(AlpineTestImage());
        const auto helloWorld = publish(HelloWorldTestImage());
        std::optional<wsl::windows::common::RunningWSLCContainer> tokenRegistry;
        if (tokenAuthentication)
        {
            WriteTestFileContent(directory / L"registry-auth.py", R"py(import contextlib
import http.client
import http.server
import ssl
import sys

upstream, realm, port = sys.argv[1:]
tls = ssl.create_default_context(cafile="/test/server.crt")
token = "wslc-proxy-test-token"

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?", 1)[0] == "/token":
            body = ('{"token":"' + token + '","expires_in":300}').encode()
            status, headers = 200, {"Content-Type": "application/json"}
        elif self.headers.get("Authorization") != "Bearer " + token:
            body = b""
            status, headers = 401, {"WWW-Authenticate": f'Bearer realm="{realm}",service="wslc-proxy-test"'}
        else:
            with contextlib.closing(http.client.HTTPSConnection(upstream, context=tls, timeout=30)) as connection:
                connection.request(self.command, self.path, headers={"Accept": self.headers.get("Accept", "*/*")})
                response = connection.getresponse()
                status = response.status
                headers = {k.lower(): v for k, v in response.getheaders()
                           if k.lower() not in ("connection", "transfer-encoding")}
                body = response.read()
        headers.setdefault("content-length", str(len(body)))
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    do_HEAD = do_GET

server = http.server.ThreadingHTTPServer(("0.0.0.0", int(port)), Handler)
server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
server_tls.load_cert_chain("/test/server.crt", "/test/server.key")
server.socket = server_tls.wrap_socket(server.socket, server_side=True)
print("Token registry ready", flush=True)
server.serve_forever()
)py");
            RunWslcAndVerify(
                std::format(L"{}image load --input \"{}\"", prefix, PythonTestImage().Path.wstring()), {.Stderr = L"", .ExitCode = 0});
            wsl::windows::common::WSLCContainerLauncher launcher(
                string::WideToMultiByte(PythonTestImage().NameAndTag()),
                {},
                {"python3",
                 "-u",
                 "/test/registry-auth.py",
                 address,
                 std::format("https://{}:{}/token", c_proxyTokenHost, c_proxyRegistryPort),
                 std::to_string(c_proxyRegistryPort)},
                {},
                "bridge");
            launcher.AddVolume(directory.wstring(), "/test", true);
            launcher.AddPort(c_proxyRegistryPort, c_proxyRegistryPort, AF_INET, IPPROTO_TCP, "127.0.0.1");
            tokenRegistry.emplace(launcher.Launch(producer.Session()));
            auto process = tokenRegistry->GetInitProcess();
            WaitForOutput(process.GetStdHandle(1), "Token registry ready");
        }

        RecordingConnectProxy proxy(tokenAuthentication);
        verify(directory, proxy, alpine, helloWorld);
    }

} // namespace

class WSLCE2ETlsRegistryTests
{
    WSLC_TEST_CLASS(WSLCE2ETlsRegistryTests)

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_BuildTokenAuthentication)
    {
        WithProxyRegistry(
            [](const auto& directory, auto& proxy, const auto& alpine, const auto&) {
                const auto context = directory / L"context";
                VERIFY_IS_TRUE(std::filesystem::create_directory(context));
                WriteTestFileContent(
                    context / L"Dockerfile",
                    std::format(
                        "FROM {}\n"
                        "RUN test -z \"$HTTP_PROXY$http_proxy$HTTPS_PROXY$https_proxy$NO_PROXY$no_proxy\""
                        " && echo buildx-proxy-ok > /proxy-test\n",
                        string::WideToMultiByte(alpine.Reference)));

                for (const bool bypassToken : {false, true})
                {
                    const auto proxySettings = std::format(
                        "  proxy:\n    https: http://localhost:{}\n    noProxy: {}\n", proxy.Port(), bypassToken ? c_proxyTokenHost : ".unrelated.invalid");
                    WithColdProxySession(directory / (bypassToken ? L"bypass" : L"proxied"), proxySettings, [&](IWSLCSession&, const HostFileChange&) {
                        const auto before = proxy.TokenConnections();
                        const auto result =
                            RunWslc(std::format(L"build \"{}\" --pull --no-cache -t wslc-proxy-build:latest", context.wstring()));
                        result.Dump();
                        if (bypassToken)
                        {
                            result.Verify({.ExitCode = 1});
                            VERIFY_IS_TRUE(result.StderrContainsSubstring(L"failed to fetch anonymous token"));
                            VERIFY_IS_TRUE(result.StderrContainsSubstring(string::MultiByteToWide(c_proxyTokenHost)));
                            VERIFY_ARE_EQUAL(before, proxy.TokenConnections());
                            RunWslc(L"image list --format json").Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
                        }
                        else
                        {
                            result.Verify({.ExitCode = 0});
                            VERIFY_IS_TRUE(proxy.TokenConnections() > before, L"Expected a bearer-token request through CONNECT");
                            RunWslcAndVerify(
                                L"container run --rm wslc-proxy-build:latest cat /proxy-test",
                                {.Stdout = L"buildx-proxy-ok\n", .Stderr = L"", .ExitCode = 0});
                        }
                    });
                }
            },
            true);
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_ColdPullAndIdleRestart)
    {
        WithProxyRegistry([](const auto& directory, auto& proxy, const auto& alpine, const auto& helloWorld) {
            for (const auto* host : {"localhost", "127.0.0.1"})
            {
                const auto proxySettings =
                    std::format("  proxy:\n    http: http://{}:{}\n    https: http://{}:{}\n", host, proxy.Port(), host, proxy.Port());
                WithColdProxySession(directory / host, proxySettings, [&](IWSLCSession& session, const HostFileChange& settings) {
                    const auto beforePull = proxy.ResponseBytes();
                    PullAndVerifyImage(helloWorld);
                    VERIFY_IS_TRUE(proxy.ResponseBytes() > beforePull, L"Expected registry traffic through CONNECT");

                    const auto missing = RunWslc(std::format(L"image inspect {}", alpine.Reference));
                    missing.Verify({
                        .Stdout = L"[]\r\n",
                        .Stderr = Localization::MessageWslcImageNotFound(string::WideToMultiByte(alpine.Reference).c_str()) + L"\r\n",
                        .ExitCode = 1,
                    });
                    const auto bootId = ReadBootId(session);
                    ULONG sessionId{};
                    VERIFY_SUCCEEDED(session.GetId(&sessionId));

                    // Idle VM recreation must retain the session's owned settings, not reread YAML.
                    settings.Update("session: {}\n");
                    BOOL wasAlreadyIdle = TRUE;
                    VERIFY_SUCCEEDED(session.TriggerIdleTermination(&wasAlreadyIdle));
                    VERIFY_IS_FALSE(wasAlreadyIdle);
                    VERIFY_ARE_NOT_EQUAL(bootId, ReadBootId(session));
                    ULONG restartedSessionId{};
                    VERIFY_SUCCEEDED(OpenDefaultElevatedSession()->GetId(&restartedSessionId));
                    VERIFY_ARE_EQUAL(sessionId, restartedSessionId);

                    const auto beforeRestartedPull = proxy.ResponseBytes();
                    PullAndVerifyImage(alpine);
                    VERIFY_IS_TRUE(
                        proxy.ResponseBytes() - beforeRestartedPull > 1024 * 1024,
                        L"Expected uncached image payload after restart");
                });
            }
        });
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Proxy_NoProxy)
    {
        WithProxyRegistry([](const auto& directory, auto& proxy, const auto& alpine, const auto&) {
            proxy.Stop();

            WithColdProxySession(
                directory / L"unset", "", [&](IWSLCSession&, const HostFileChange&) { PullAndVerifyImage(alpine); });

            for (const auto* noProxy : {"", ".unrelated.invalid", c_proxyRegistryHost})
            {
                auto proxySettings = std::format("  proxy:\n    https: http://127.0.0.1:{}\n", proxy.Port());
                if (noProxy[0] != '\0')
                {
                    proxySettings += std::format("    noProxy: {}\n", noProxy);
                }

                const auto storage = directory / (noProxy[0] == '\0' ? "stopped" : noProxy);
                WithColdProxySession(storage, proxySettings, [&](IWSLCSession&, const HostFileChange&) {
                    if (std::string_view(noProxy) == c_proxyRegistryHost)
                    {
                        PullAndVerifyImage(alpine);
                    }
                    else
                    {
                        const auto result = RunWslc(std::format(L"image pull {}", alpine.Reference));
                        result.Verify({.ExitCode = 1});
                        VERIFY_IS_TRUE(
                            result.StderrContainsSubstring(L"proxyconnect tcp"), L"Failure must come from the stopped proxy");
                        RunWslc(L"image list --format json").Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
                    }
                });
            }
        });
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
