/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsCertStore.h

Abstract:

    Helpers for reading the Windows certificate stores so that the trusted root
    certificate authorities configured on the host can be mirrored into the
    container VM.

--*/

#pragma once

#include <string>

namespace wsl::windows::service::wslc {

// Collects the certificates from the host's "Trusted Root Certification
// Authorities" stores (both the local machine and current user stores) and
// returns them serialized as a single PEM bundle.
// Duplicate certificates that appear in more than one store are emitted once.
// Returns an empty string if no certificates were found.
std::string CollectTrustedRootCertificatesPem();

} // namespace wsl::windows::service::wslc
