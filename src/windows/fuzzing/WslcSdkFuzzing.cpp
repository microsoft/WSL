// Copyright (C) Microsoft Corporation. All rights reserved.

// libFuzzer harness for the WSLC SDK C API.
//
// Exercises the session and container settings flow with fuzz-controlled inputs.
// Requires WSL service running on the host (installed via setup.ps1 on OneFuzz VMs).

#include "precomp.h"

#include "wslcsdk.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Helper to read a null-terminated narrow string from the fuzz buffer.
// Advances offset past the string (including null) or to end of buffer.
static std::string ReadString(const uint8_t* data, size_t size, size_t& offset, size_t maxLen = 64)
{
    std::string result;
    while (offset < size && result.size() < maxLen)
    {
        char ch = static_cast<char>(data[offset++]);
        if (ch == '\0')
        {
            break;
        }
        result.push_back(ch);
    }
    return result;
}

// Helper to read a null-terminated wide string from the fuzz buffer.
static std::wstring ReadWideString(const uint8_t* data, size_t size, size_t& offset, size_t maxLen = 64)
{
    std::wstring result;
    while (offset + 1 < size && result.size() < maxLen)
    {
        wchar_t ch = static_cast<wchar_t>(data[offset] | (data[offset + 1] << 8));
        offset += 2;
        if (ch == L'\0')
        {
            break;
        }
        result.push_back(ch);
    }
    return result;
}

template <typename T>
static T ReadValue(const uint8_t* data, size_t size, size_t& offset)
{
    T value{};
    if (offset + sizeof(T) <= size)
    {
        std::memcpy(&value, data + offset, sizeof(T));
        offset += sizeof(T);
    }
    return value;
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 10 || size > 4096)
    {
        return -1;
    }

    size_t offset = 0;

    // Read session parameters from corpus
    std::wstring sessionName = ReadWideString(data, size, offset);
    std::wstring storagePath = ReadWideString(data, size, offset);

    // Initialize session settings
    WslcSessionSettings sessionSettings{};
    HRESULT hr = WslcInitSessionSettings(sessionName.c_str(), storagePath.c_str(), &sessionSettings);
    if (FAILED(hr))
    {
        return -1;
    }

    // Configure session with minimal resources
    WslcSetSessionSettingsCpuCount(&sessionSettings, 1);
    WslcSetSessionSettingsMemory(&sessionSettings, 512);
    WslcSetSessionSettingsTimeout(&sessionSettings, 5000);

    // Read container parameters
    std::string imageName = ReadString(data, size, offset);
    std::string containerName = ReadString(data, size, offset);
    std::string hostName = ReadString(data, size, offset);

    // Read port mappings
    uint8_t portCount = (offset < size) ? data[offset++] : 0;
    std::vector<WslcContainerPortMapping> ports(portCount);
    for (uint8_t i = 0; i < portCount; ++i)
    {
        ports[i].windowsPort = ReadValue<uint16_t>(data, size, offset);
        ports[i].containerPort = ReadValue<uint16_t>(data, size, offset);
        ports[i].protocol = static_cast<WslcPortProtocol>(ReadValue<uint8_t>(data, size, offset) % 2);
    }

    // Read flags
    auto containerFlags = static_cast<WslcContainerFlags>(ReadValue<uint32_t>(data, size, offset));

    // Attempt to create session — may fail if service isn't available
    WslcSession session = nullptr;
    hr = WslcCreateSession(&sessionSettings, &session, nullptr);
    if (FAILED(hr) || !session)
    {
        return 0;
    }

    // Initialize container settings
    WslcContainerSettings containerSettings{};
    hr = WslcInitContainerSettings(imageName.c_str(), &containerSettings);
    if (FAILED(hr))
    {
        WslcTerminateSession(session);
        WslcReleaseSession(session);
        return 0;
    }

    if (!containerName.empty())
    {
        WslcSetContainerSettingsName(&containerSettings, containerName.c_str());
    }
    if (!hostName.empty())
    {
        WslcSetContainerSettingsHostName(&containerSettings, hostName.c_str());
    }
    if (portCount > 0)
    {
        WslcSetContainerSettingsPortMappings(&containerSettings, ports.data(), portCount);
    }
    WslcSetContainerSettingsFlags(&containerSettings, containerFlags);

    // Attempt container creation — exercises validation and service communication
    WslcContainer container = nullptr;
    hr = WslcCreateContainer(session, &containerSettings, &container, nullptr);
    if (SUCCEEDED(hr) && container)
    {
        WslcReleaseContainer(container);
    }

    WslcTerminateSession(session);
    WslcReleaseSession(session);

    return 0;
}

#include "FuzzingHarness.h"
