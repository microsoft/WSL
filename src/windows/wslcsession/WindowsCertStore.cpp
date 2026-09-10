/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsCertStore.cpp

Abstract:

    Implementation of helpers for reading the Windows certificate stores.

--*/

#include "precomp.h"
#include "WindowsCertStore.h"
#include <optional>
#include <set>
#include <wincrypt.h>

namespace {

// Converts a single DER-encoded certificate into a PEM block.
// Returns nullopt if the certificate cannot be encoded.
std::optional<std::string> TryEncodeCertificateAsPem(const CERT_CONTEXT& Cert)
{
    DWORD pemSize = 0;
    if (!CryptBinaryToStringA(Cert.pbCertEncoded, Cert.cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &pemSize))
    {
        LOG_LAST_ERROR_MSG("CryptBinaryToStringA (size query) failed for a root certificate; skipping it");
        return std::nullopt;
    }

    std::string pem(pemSize, '\0');
    if (!CryptBinaryToStringA(Cert.pbCertEncoded, Cert.cbCertEncoded, CRYPT_STRING_BASE64HEADER, pem.data(), &pemSize))
    {
        LOG_LAST_ERROR_MSG("CryptBinaryToStringA (encode) failed for a root certificate; skipping it");
        return std::nullopt;
    }

    // The pemSize after the actual write call does not include terminating null,
    // and is 1 less than the value returned by the earlier query call.
    pem.resize(pemSize);

    return pem;
}

// Enumerates every certificate in the given "ROOT" system store and appends each
// (deduplicated by cert thumbprint) to the output PEM bundle. Returns number of skipped certs.
int AppendRootStore(DWORD StoreFlags, std::set<std::string>& Seen, std::string& Pem)
{
    const wil::unique_hcertstore store{CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, NULL, StoreFlags | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"ROOT")};
    if (!store)
    {
        LOG_LAST_ERROR_MSG("CertOpenStore failed for ROOT store (flags 0x%x)", StoreFlags);
        return 0;
    }

    // N.B. CertEnumCertificatesInStore frees the context passed to it and returns the next one,
    // so the loop must not free the context itself.
    int skippedCount = 0;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store.get(), cert)) != nullptr)
    {
        if (cert->cbCertEncoded == 0)
        {
            continue;
        }

        // Use cert Thumbprint for dedupe.
        BYTE hash[20];
        DWORD hashSize = sizeof(hash);
        if (!CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, hash, &hashSize))
        {
            LOG_LAST_ERROR_MSG("CertGetCertificateContextProperty(CERT_SHA1_HASH_PROP_ID) failed; skipping a root certificate");
            skippedCount++;
            continue;
        }

        if (!Seen.insert(std::string{reinterpret_cast<const char*>(hash), hashSize}).second)
        {
            continue;
        }

        if (auto pem = TryEncodeCertificateAsPem(*cert))
        {
            Pem += *pem;
        }
        else
        {
            skippedCount++;
        }
    }

    return skippedCount;
}

} // namespace

namespace wsl::windows::service::wslc {

std::string CollectTrustedRootCertificatesPem()
{
    std::set<std::string> seen;
    std::string pem;

    auto skippedCount = AppendRootStore(CERT_SYSTEM_STORE_LOCAL_MACHINE, seen, pem);
    if (skippedCount > 0)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageWslcImportCertsSkippedComputer(std::to_wstring(skippedCount)));
    }

    skippedCount = AppendRootStore(CERT_SYSTEM_STORE_CURRENT_USER, seen, pem);
    if (skippedCount > 0)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageWslcImportCertsSkippedUser(std::to_wstring(skippedCount)));
    }

    return pem;
}

} // namespace wsl::windows::service::wslc
