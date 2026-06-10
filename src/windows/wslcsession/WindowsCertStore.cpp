/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsCertStore.cpp

Abstract:

    Implementation of helpers for reading the Windows certificate stores.

--*/

#include "precomp.h"
#include "WindowsCertStore.h"
#include <set>
#include <wincrypt.h>

namespace {

// Converts a single DER-encoded certificate into a PEM block delimited by
// "-----BEGIN CERTIFICATE-----" / "-----END CERTIFICATE-----".
std::string EncodeCertificateAsPem(const CERT_CONTEXT& Cert)
{
    DWORD pemSize = 0;
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(Cert.pbCertEncoded, Cert.cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &pemSize));

    std::string pem(pemSize, '\0');
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(Cert.pbCertEncoded, Cert.cbCertEncoded, CRYPT_STRING_BASE64HEADER, pem.data(), &pemSize));

    // CryptBinaryToStringA reports the size including the terminating null; drop it.
    pem.resize(pemSize);

    return pem;
}

// Enumerates every certificate in the given "ROOT" system store and appends each
// (deduplicated by its raw encoded bytes) to the output PEM bundle.
void AppendRootStore(DWORD StoreFlags, std::set<std::string>& Seen, std::string& Pem)
{
    const wil::unique_hcertstore store{CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, NULL, StoreFlags | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"ROOT")};
    if (!store)
    {
        // The store may not exist (for example, no per-user root store); this is not fatal.
        LOG_LAST_ERROR_MSG("CertOpenStore failed for ROOT store (flags 0x%x)", StoreFlags);
        return;
    }

    // N.B. CertEnumCertificatesInStore frees the context passed to it and returns the next one,
    // so the loop must not free the context itself.
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store.get(), cert)) != nullptr)
    {
        std::string der{reinterpret_cast<const char*>(cert->pbCertEncoded), cert->cbCertEncoded};
        if (!Seen.insert(der).second)
        {
            continue;
        }

        Pem += EncodeCertificateAsPem(*cert);
    }
}

} // namespace

namespace wsl::windows::service::wslc {

std::string CollectTrustedRootCertificatesPem()
{
    std::set<std::string> seen;
    std::string pem;

    // Machine roots first: enterprise CAs distributed via group policy land here.
    AppendRootStore(CERT_SYSTEM_STORE_LOCAL_MACHINE, seen, pem);
    AppendRootStore(CERT_SYSTEM_STORE_CURRENT_USER, seen, pem);

    return pem;
}

} // namespace wsl::windows::service::wslc
