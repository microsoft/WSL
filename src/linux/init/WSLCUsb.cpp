/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCUsb.cpp

Abstract:

    USB/IP client implementation. The kernel's vhci-hcd driver does the actual
    work; this code performs the USB/IP handshake with the server on the Windows
    host and hands the resulting socket to the driver, which is all the usbip
    command line tool does. Doing it here avoids shipping that tool in the VM.

--*/

#include "common.h"
#include "util.h"
#include "WSLCUsb.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

constexpr auto c_vhciPath = "/sys/devices/platform/vhci_hcd.0";
constexpr auto c_usbDevicesPath = "/sys/bus/usb/devices";

// Commands are run through popen, which uses a shell, and init's environment has
// no useful PATH, so these are named in full.
constexpr auto c_modprobe = "/sbin/modprobe";

constexpr uint16_t c_usbipVersion = 0x0111;
constexpr uint16_t c_opRequestImport = 0x8003;
constexpr uint16_t c_opReplyImport = 0x0003;
constexpr uint16_t c_opRequestDeviceList = 0x8005;
constexpr uint16_t c_opReplyDeviceList = 0x0005;

constexpr size_t c_busIdSize = 32;

// vhci reports a port as free with this status value.
constexpr int c_portFree = 4;

// How long to wait for a device to enumerate and its class nodes to appear.
constexpr auto c_enumerationTimeout = std::chrono::seconds(10);

// How often the watcher checks whether a device needs importing again.
constexpr auto c_watchInterval = std::chrono::seconds(2);

#pragma pack(push, 1)

// Every USB/IP message starts with this header. All fields are big-endian.
struct OpHeader
{
    uint16_t Version;
    uint16_t Code;
    uint32_t Status;
};

// Device description returned by both import and device-list replies.
struct OpDevice
{
    char Path[256];
    char BusId[c_busIdSize];
    uint32_t BusNum;
    uint32_t DevNum;
    uint32_t Speed;
    uint16_t IdVendor;
    uint16_t IdProduct;
    uint16_t BcdDevice;
    uint8_t DeviceClass;
    uint8_t DeviceSubClass;
    uint8_t DeviceProtocol;
    uint8_t ConfigurationValue;
    uint8_t NumConfigurations;
    uint8_t NumInterfaces;
};

#pragma pack(pop)

struct Attachment
{
    int Port{};
    pid_t WatcherPid{};
    std::vector<std::string> DeviceNodes;
    int References{};
};

std::map<std::string, Attachment> g_attachments;

// Messages can be handled on more than one thread (see WSLC_FORK::Thread in
// WSLCInit.cpp), so the table is locked. Holding the lock across a whole attach
// also stops two of them picking the same free vhci port.
std::mutex g_attachmentLock;

void WriteAll(int Socket, const void* Buffer, size_t Size)
{
    auto* current = static_cast<const char*>(Buffer);
    while (Size > 0)
    {
        const auto written = TEMP_FAILURE_RETRY(send(Socket, current, Size, MSG_NOSIGNAL));
        THROW_LAST_ERROR_IF(written <= 0);

        current += written;
        Size -= written;
    }
}

void ReadAll(int Socket, void* Buffer, size_t Size)
{
    auto* current = static_cast<char*>(Buffer);
    while (Size > 0)
    {
        const auto read = TEMP_FAILURE_RETRY(recv(Socket, current, Size, 0));

        // A server that closes early means the device went away mid-handshake.
        THROW_ERRNO_IF(ENODEV, read == 0);
        THROW_LAST_ERROR_IF(read < 0);

        current += read;
        Size -= read;
    }
}

wil::unique_fd Connect(const std::string& Host, uint16_t Port)
{
    wil::unique_fd socketFd{socket(AF_INET, SOCK_STREAM, 0)};
    THROW_LAST_ERROR_IF(!socketFd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(Port);
    THROW_ERRNO_IF(EINVAL, inet_pton(AF_INET, Host.c_str(), &address.sin_addr) != 1);

    THROW_LAST_ERROR_IF(connect(socketFd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0);

    return socketFd;
}

void SendRequest(int Socket, uint16_t Code, const std::string* BusId)
{
    const OpHeader header{htons(c_usbipVersion), htons(Code), 0};
    WriteAll(Socket, &header, sizeof(header));

    if (BusId != nullptr)
    {
        char busId[c_busIdSize]{};
        THROW_ERRNO_IF(EINVAL, BusId->size() >= sizeof(busId));
        std::copy(BusId->begin(), BusId->end(), busId);

        WriteAll(Socket, busId, sizeof(busId));
    }
}

void ReadReplyHeader(int Socket, uint16_t ExpectedCode)
{
    OpHeader header{};
    ReadAll(Socket, &header, sizeof(header));

    THROW_ERRNO_IF(EPROTO, ntohs(header.Code) != ExpectedCode);

    // The server reports a non-zero status when it will not export the device,
    // which is nearly always because it has not been shared with 'usbipd bind'.
    THROW_ERRNO_IF(ENODEV, header.Status != 0);
}

// Returns the bus IDs of every device the server is currently sharing.
std::vector<std::string> ListDevices(const std::string& Host, uint16_t Port)
{
    auto socketFd = Connect(Host, Port);
    SendRequest(socketFd.get(), c_opRequestDeviceList, nullptr);
    ReadReplyHeader(socketFd.get(), c_opReplyDeviceList);

    uint32_t count{};
    ReadAll(socketFd.get(), &count, sizeof(count));
    count = ntohl(count);

    std::vector<std::string> busIds;
    for (uint32_t index = 0; index < count; index += 1)
    {
        OpDevice device{};
        ReadAll(socketFd.get(), &device, sizeof(device));

        // Each device is followed by one descriptor per interface.
        for (uint8_t iface = 0; iface < device.NumInterfaces; iface += 1)
        {
            uint8_t descriptor[4]{};
            ReadAll(socketFd.get(), descriptor, sizeof(descriptor));
        }

        device.BusId[sizeof(device.BusId) - 1] = '\0';
        busIds.emplace_back(device.BusId);
    }

    return busIds;
}

// Reads the vhci port table. Each row is "hub port sta spd dev sockfd busid".
std::vector<std::array<std::string, 7>> ReadPortTable()
{
    std::istringstream status{UtilReadFileContent(std::format("{}/status", c_vhciPath))};

    std::string line;
    std::getline(status, line); // Header row.

    std::vector<std::array<std::string, 7>> rows;
    while (std::getline(status, line))
    {
        std::istringstream columns{line};
        std::array<std::string, 7> row;
        if (columns >> row[0] >> row[1] >> row[2] >> row[3] >> row[4] >> row[5] >> row[6])
        {
            rows.push_back(std::move(row));
        }
    }

    return rows;
}

int FindFreePort()
{
    for (const auto& row : ReadPortTable())
    {
        if (std::stoi(row[2]) == c_portFree)
        {
            return std::stoi(row[1]);
        }
    }

    // Every vhci port is in use; the kernel is built with a fixed number of them.
    THROW_ERRNO(EBUSY);
    return -1;
}

// Returns the local bus ID the kernel gave the device on the supplied port, for
// example "1-1". Returns an empty string while the port is free or the device is
// still enumerating, which it reports as the placeholder bus ID "0-0".
std::string LocalBusId(int Port)
{
    for (const auto& row : ReadPortTable())
    {
        if (std::stoi(row[1]) == Port && std::stoi(row[2]) != c_portFree && row[6] != "0-0")
        {
            return row[6];
        }
    }

    return {};
}

// True when the port holds no device at all, as opposed to one that is still
// coming up. Only then is it worth importing again.
bool IsPortFree(int Port)
{
    for (const auto& row : ReadPortTable())
    {
        if (std::stoi(row[1]) == Port)
        {
            return std::stoi(row[2]) == c_portFree;
        }
    }

    return false;
}

// The VM runs no udev, so the driver a device needs is loaded here instead.
// Using modalias keeps this working for every device type, not just serial ones.
void LoadDeviceDriver(const std::string& LocalBusId)
{
    const std::filesystem::path devicePath{std::format("{}/{}", c_usbDevicesPath, LocalBusId)};

    std::vector<std::string> modaliases;
    std::error_code error;

    // Drivers usually bind to an interface rather than to the device itself, and
    // a device commonly exposes no modalias of its own, so every one of these is
    // optional.
    const auto deviceAlias = devicePath / "modalias";
    if (std::filesystem::exists(deviceAlias, error))
    {
        modaliases.push_back(UtilReadFileContent(deviceAlias.native()));
    }

    for (const auto& entry : std::filesystem::directory_iterator{devicePath, error})
    {
        const auto alias = entry.path() / "modalias";
        if (std::filesystem::exists(alias, error))
        {
            modaliases.push_back(UtilReadFileContent(alias.native()));
        }
    }

    for (auto& alias : modaliases)
    {
        // The file has a trailing newline that modprobe will not accept.
        while (!alias.empty() && std::isspace(static_cast<unsigned char>(alias.back())))
        {
            alias.pop_back();
        }

        if (!alias.empty())
        {
            // A device with no driver in the image is expected, so failure here
            // is not fatal: /dev/bus/usb access still works without one.
            UtilExecCommandLine(std::format("{} -q {}", c_modprobe, alias).c_str(), nullptr, 0, false);
        }
    }
}

// Finds the character devices a class driver created for the device, such as
// "/dev/ttyUSB0". Devices driven from user space through libusb have none.
std::vector<std::string> FindClassDeviceNodes(const std::string& LocalBusId)
{
    const std::filesystem::path devicePath{std::format("{}/{}", c_usbDevicesPath, LocalBusId)};

    std::vector<std::string> nodes;
    std::error_code error;

    // Adds /dev/<name> for every entry naming a device node that exists.
    const auto collect = [&nodes, &error](const std::filesystem::path& Directory) {
        for (const auto& entry : std::filesystem::directory_iterator{Directory, error})
        {
            const auto name = entry.path().filename().native();
            if (!name.starts_with("tty"))
            {
                continue;
            }

            auto node = std::format("/dev/{}", name);
            if (std::filesystem::exists(node, error))
            {
                nodes.push_back(std::move(node));
            }
        }
    };

    // Drivers bind to an interface, and put their node either straight into the
    // interface directory (ch341, ftdi_sio) or into a "tty" directory inside it
    // (cdc_acm), so both layouts are searched.
    for (const auto& iface : std::filesystem::directory_iterator{devicePath, error})
    {
        if (!std::filesystem::is_directory(iface.path(), error))
        {
            continue;
        }

        collect(iface.path());
        collect(iface.path() / "tty");
    }

    return nodes;
}

// Performs the handshake and hands the socket to vhci-hcd, which takes ownership
// of it. Returns the port the device was attached to.
//
// VhciPort reuses a specific port, which matters when re-importing a device that
// was unplugged: the record of where it lives must stay correct, or detaching it
// later would release whatever else had taken its place.
int Import(const std::string& Host, uint16_t Port, const std::string& BusId, std::optional<int> VhciPort = {})
{
    // The port is chosen before the device is requested. The server hands the
    // device over as soon as it is asked for it, so anything that can fail must
    // happen first, or a failure here would strand the device away from Windows.
    const auto vhciPort = VhciPort.has_value() ? VhciPort.value() : FindFreePort();

    auto socketFd = Connect(Host, Port);
    SendRequest(socketFd.get(), c_opRequestImport, &BusId);
    ReadReplyHeader(socketFd.get(), c_opReplyImport);

    OpDevice device{};
    ReadAll(socketFd.get(), &device, sizeof(device));
    const auto deviceId = (ntohl(device.BusNum) << 16) | ntohl(device.DevNum);

    const auto attach = std::format("{} {} {} {}", vhciPort, socketFd.get(), deviceId, ntohl(device.Speed));
    THROW_LAST_ERROR_IF(WriteToFile(std::format("{}/attach", c_vhciPath).c_str(), attach.c_str(), O_WRONLY | O_CLOEXEC) < 0);

    // The driver holds its own reference now, so this one is released.
    socketFd.reset();

    return vhciPort;
}

// Waits for the device to enumerate, loads its driver and reports its nodes.
std::vector<std::string> WaitForDevice(int Port)
{
    const auto deadline = std::chrono::steady_clock::now() + c_enumerationTimeout;

    std::string localBusId;
    while (localBusId.empty() && std::chrono::steady_clock::now() < deadline)
    {
        localBusId = LocalBusId(Port);
        if (localBusId.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    THROW_ERRNO_IF(ENODEV, localBusId.empty());

    // Best effort: a device with no driver in the image is still usable through
    // /dev/bus/usb, so a failure to load one must not fail the whole attach.
    try
    {
        LoadDeviceDriver(localBusId);
    }
    CATCH_LOG()

    // A class driver creates its node shortly after binding. Devices without one
    // are normal, so this waits only until something shows up or time runs out.
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto nodes = FindClassDeviceNodes(localBusId);
        if (!nodes.empty())
        {
            return nodes;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return {};
}

// Re-imports a device after it is unplugged and plugged back in. Without this a
// device that resets itself, as boards do while being flashed, would stay gone.
void WatchDevice(const std::string& Host, uint16_t Port, const std::string& BusId, int VhciPort)
{
    for (;;)
    {
        std::this_thread::sleep_for(c_watchInterval);

        try
        {
            // Anything other than an empty port means the device is present or
            // still coming up, so there is nothing to do.
            if (!IsPortFree(VhciPort))
            {
                continue;
            }

            std::ignore = Import(Host, Port, BusId, VhciPort);
            std::ignore = WaitForDevice(VhciPort);

            LOG_INFO("Re-imported USB device {} on port {}", BusId.c_str(), VhciPort);
        }
        catch (...)
        {
            // The device is simply not back yet, which is the common case.
        }
    }
}

std::vector<std::string> AttachOne(const std::string& Host, uint16_t Port, const std::string& BusId)
{
    auto existing = g_attachments.find(BusId);
    if (existing != g_attachments.end())
    {
        existing->second.References += 1;
        return existing->second.DeviceNodes;
    }

    const auto vhciPort = Import(Host, Port, BusId);
    auto nodes = WaitForDevice(vhciPort);

    Attachment attachment{vhciPort, 0, nodes, 1};
    attachment.WatcherPid = UtilCreateChildProcess("UsbWatcher", [Host, Port, BusId, vhciPort]() {
        WatchDevice(Host, Port, BusId, vhciPort);
        return 0;
    });

    g_attachments.emplace(BusId, std::move(attachment));

    return nodes;
}

// Callers hold g_attachmentLock.
void DetachOne(const std::string& BusId)
{
    auto attachment = g_attachments.find(BusId);
    if (attachment == g_attachments.end())
    {
        return;
    }

    attachment->second.References -= 1;
    if (attachment->second.References > 0)
    {
        return;
    }

    // The watcher has to go first, or it races this and imports the device again.
    if (attachment->second.WatcherPid > 0)
    {
        kill(attachment->second.WatcherPid, SIGKILL);
        waitpid(attachment->second.WatcherPid, nullptr, 0);
    }

    const auto detach = std::to_string(attachment->second.Port);
    if (WriteToFile(std::format("{}/detach", c_vhciPath).c_str(), detach.c_str(), O_WRONLY | O_CLOEXEC) < 0)
    {
        LOG_ERROR("Failed to detach USB device {} from port {}, {}", BusId.c_str(), attachment->second.Port, errno);
    }

    g_attachments.erase(attachment);
}

} // namespace

namespace wsl::linux::usb {

AttachResult Attach(const std::string& Host, uint16_t Port, const std::string& BusId)
{
    // vhci-hcd is not loaded until something needs it. init runs with a minimal
    // environment, so modprobe is named by full path rather than found on PATH.
    UtilExecCommandLine(std::format("{} vhci-hcd", c_modprobe).c_str(), nullptr, 0, false);

    // Fail here rather than part way through an import, so that a guest without
    // USB/IP support never takes the device away from Windows.
    THROW_ERRNO_IF(ENODEV, !std::filesystem::exists(std::format("{}/status", c_vhciPath)));

    AttachResult result;

    if (BusId == "all")
    {
        result.BusIds = ListDevices(Host, Port);

        // Nothing shared means the user has not run 'usbipd bind' yet, which is
        // worth reporting rather than starting a container with no devices.
        THROW_ERRNO_IF(ENODEV, result.BusIds.empty());
    }
    else
    {
        result.BusIds.push_back(BusId);
    }

    std::lock_guard lock{g_attachmentLock};

    // 'all' imports several devices under one request. If a later one fails,
    // give back the ones already taken, or they stay claimed by a VM that never
    // got a container and Windows cannot see them again.
    std::vector<std::string> attached;
    auto undo = wil::scope_exit([&]() {
        for (const auto& busId : attached)
        {
            DetachOne(busId);
        }
    });

    for (const auto& busId : result.BusIds)
    {
        auto nodes = AttachOne(Host, Port, busId);
        attached.push_back(busId);
        result.DeviceNodes.insert(result.DeviceNodes.end(), nodes.begin(), nodes.end());
    }

    undo.release();

    return result;
}

void Detach(const std::string& BusId)
{
    std::lock_guard lock{g_attachmentLock};

    DetachOne(BusId);
}

} // namespace wsl::linux::usb
