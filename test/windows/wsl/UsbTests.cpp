// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "UsbIpTestClient.hpp"
#include "UsbTypes.h"
#include "UsbUxdRegistry.h"
#include "retryshared.h"
#include "svccomm.hpp"
#include <iterator>
#include <sstream>

#define WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(_cli) \
    const auto _cli = ::wsl::test::usbip::FindClient(); \
    if (!(_cli)) \
    { \
        LogSkipped("usbip-win2 is not installed."); \
        return; \
    } \
    if (!::wsl::windows::common::usb::uxd::IsSupported()) \
    { \
        LogSkipped("This version of Windows does not support WSL USB redirection."); \
        return; \
    }

using namespace wsl::windows::common;

namespace UsbTests {
namespace fixture = wsl::test::usbip;

namespace {

    struct ListEntry
    {
        std::wstring Port;
        std::wstring State;
    };

    std::optional<ListEntry> FindDeviceInVerboseList(const fixture::Server& server, fixture::Device device, HANDLE token = nullptr)
    {
        const auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"--usb --list --verbose", 0, nullptr, token);
        VERIFY_ARE_EQUAL(L"", error);
        auto id = server.InstanceId(device);
        std::ranges::replace(id, L'\\', L'#');
        std::wistringstream lines(output);
        for (std::wstring line; std::getline(lines, line);)
        {
            std::wistringstream stream(line);
            using Field = std::istream_iterator<std::wstring, wchar_t>;
            const std::vector<std::wstring> fields{Field{stream}, Field{}};
            if (fields.size() >= 4 && fields.back() == id)
            {
                return ListEntry{fields.front(), fields[fields.size() - 2]};
            }
        }
        return {};
    }

    std::wstring WaitForDeviceStateAndGetPortId(const fixture::Server& server, fixture::Device device, LPCWSTR expectedState, HANDLE token = nullptr)
    {
        return wsl::shared::retry::RetryWithTimeout<std::wstring>(
            [&] {
                const auto entry = FindDeviceInVerboseList(server, device, token);
                THROW_HR_IF_MSG(
                    E_PENDING,
                    !entry || entry->State != expectedState,
                    "Waiting for %ls to be %ls (last state: %ls)",
                    server.InstanceId(device).c_str(),
                    expectedState,
                    entry ? entry->State.c_str() : L"not listed");
                return entry->Port;
            },
            std::chrono::milliseconds(100),
            std::chrono::seconds(15),
            [] { return wil::ResultFromCaughtException() == E_PENDING; });
    }

    std::vector<std::wstring> GetPortIdsFromDefaultList()
    {
        const auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"--usb --list");
        VERIFY_ARE_EQUAL(L"", error);
        std::wistringstream lines(output);
        std::vector<std::wstring> ports;
        for (std::wstring line; std::getline(lines, line);)
        {
            std::wistringstream stream(line);
            std::wstring port;
            if (stream >> port)
            {
                ports.push_back(std::move(port));
            }
        }
        return ports;
    }

    auto AttachSerialDeviceToWsl(const fixture::Server& server, const std::wstring& port)
    {
        auto detachOnExit = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&server, port] {
            const auto current = FindDeviceInVerboseList(server, fixture::Device::Serial);
            if (current && current->Port == port && (current->State == L"Attached" || current->State == L"Unavailable"))
            {
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--usb --detach {}", port));
            }
        });
        LxsstuLaunchWslAndCaptureOutput(std::format(L"--usb --attach {}", port));
        return detachOnExit;
    }

    void RunGuestUsbScript(const fixture::Server& server, fixture::Device device, const char* scriptBody)
    {
        const auto args = std::format(
            L"-d {} -u root --exec timeout -k 2 15 sh -eu -s -- {} {:04x} {:04x}",
            LXSS_DISTRO_NAME_TEST_L,
            server.SerialNumber(),
            fixture::c_vendorId,
            fixture::GetProfile(device).ProductId);
        auto command = LxssGenerateWslCommandLine(args.c_str());
        std::string script = R"sh(serial=$1 vendor=$2 product=$3
find_device() {
    for node in /sys/bus/usb/devices/*; do
        grep -Fxq "$serial" "$node/serial" 2>/dev/null &&
        grep -Fxq "$vendor" "$node/idVendor" && grep -Fxq "$product" "$node/idProduct" || continue
        printf '%s' "$node"
        return 0
    done
    return 1
}
)sh";
        script += scriptBody;
        LxsstuLaunchCommandAndCaptureOutput(command.data(), script.c_str());
    }

    void WaitUntilDeviceIsAbsentInGuest(const fixture::Server& server, fixture::Device device)
    {
        RunGuestUsbScript(server, device, "while find_device >/dev/null; do sleep 0.1; done\n");
    }

    void VerifyGuestSerialLoopback(const fixture::Server& server)
    {
        LxsstuLaunchWslAndCaptureOutput(
            L"-d " LXSS_DISTRO_NAME_TEST_L L" --system -u root --exec /sbin/modprobe -a xhci_pci cdc_acm");
        RunGuestUsbScript(server, fixture::Device::Serial, R"sh(until device=$(find_device); do sleep 0.1; done
while :; do
    for node in "$device"/*:*/tty/ttyACM*; do
        tty=/dev/${node##*/}
        [ -e "$node" ] && [ -c "$tty" ] && break 2
    done
    sleep 0.1
done
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
for index in $(seq 1 171); do printf '\000\125\377'; done > "$dir/sent"
exec 3<>"$tty"
stty raw -echo 115200 <&3
cat "$dir/sent" >&3
dd bs=513 count=1 iflag=fullblock status=none <&3 > "$dir/received"
cmp "$dir/sent" "$dir/received"
)sh");
    }

    void VerifyHostSerialLoopback(const fixture::Server& server)
    {
        const auto instance = server.InstanceId(fixture::Device::Serial);
        const auto command =
            std::format(
                L"$ErrorActionPreference = 'Stop'; $port = Get-CimInstance Win32_SerialPort | Where-Object PNPDeviceID -eq "
                L"'{}'; ",
                instance) +
            L"if ($port) { $tty = [IO.Ports.SerialPort]::new($port.DeviceID, 115200); try { "
            L"$tty.ReadTimeout = 5000; $tty.WriteTimeout = 5000; $tty.Open(); $tty.WriteLine('WSL USB'); $tty.ReadLine() "
            L"} finally { $tty.Dispose() } }";
        const auto echo = wsl::shared::retry::RetryWithTimeout<std::wstring>(
            [&] {
                auto output = LxsstuLaunchPowershellAndCaptureOutput(command).first;
                THROW_HR_IF_MSG(E_PENDING, output.empty(), "Waiting for Windows serial device %ls", instance.c_str());
                return output;
            },
            std::chrono::milliseconds(100),
            std::chrono::seconds(15),
            [] { return wil::ResultFromCaughtException() == E_PENDING; });
        VERIFY_ARE_EQUAL(L"WSL USB\r\n", echo);
    }

    bool IsAnotherDistroRunning()
    {
        const auto running = LxsstuLaunchWslAndCaptureOutput(L"--list --running --quiet").first;
        return !running.empty() && running != LXSS_DISTRO_NAME_TEST_L L"\r\n";
    }

    void CrashWslServiceAndRestartTestDistro()
    {
        const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        VERIFY_IS_NOT_NULL(manager.get());
        const wil::unique_schandle service{OpenServiceW(manager.get(), L"WslService", SERVICE_QUERY_STATUS)};
        VERIFY_IS_NOT_NULL(service.get());

        const auto [state, pid] = GetServiceState(service.get());
        VERIFY_ARE_EQUAL(DWORD{SERVICE_RUNNING}, state);
        const wil::unique_handle process{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)};
        VERIFY_IS_NOT_NULL(process.get());
        THROW_HR_IF(E_CHANGED_STATE, GetServiceState(service.get()).second != pid);
        VERIFY_WIN32_BOOL_SUCCEEDED(TerminateProcess(process.get(), ERROR_PROCESS_ABORTED));
        VERIFY_ARE_EQUAL(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(process.get(), 15000));
        VERIFY_IS_TRUE(WaitForServiceState(service.get(), SERVICE_STOPPED, pid) || GetServiceState(service.get()).second != pid);

        LxsstuLaunchWslAndCaptureOutput(L"-d " LXSS_DISTRO_NAME_TEST_L L" --exec /bin/true");
        const auto [newState, newPid] = GetServiceState(service.get());
        VERIFY_ARE_EQUAL(DWORD{SERVICE_RUNNING}, newState);
        VERIFY_ARE_NOT_EQUAL(pid, newPid);
    }

} // namespace

class UsbTests
{
    WSL_TEST_CLASS(UsbTests)

    TEST_METHOD(PortIds)
    {
        const usb::PortId port{L"2.1"};
        VERIFY_IS_TRUE((port.Parts == std::vector<uint32_t>{2, 1}));
        VERIFY_ARE_EQUAL(std::wstring(L"2.1"), port.ToString());
        const usb::PortId nested{L"2.1.3.4"};
        VERIFY_IS_TRUE((nested.Parts == std::vector<uint32_t>{2, 1, 3, 4}));
        VERIFY_ARE_EQUAL(std::wstring(L"2.1.3.4"), nested.ToString());
        VERIFY_IS_TRUE(nested == usb::PortId{nested.ToString()});
        VERIFY_IS_TRUE(usb::PortId{L"02.01.003.004"} == nested);
        VERIFY_IS_FALSE(port == nested);
        VERIFY_ARE_EQUAL(999u, usb::PortId{L"1.999"}.Parts.back());
        VERIFY_ARE_EQUAL(1000u, usb::PortId{L"1.1000"}.Parts.back());
        VERIFY_ARE_EQUAL(4294967295u, usb::PortId{L"1.4294967295"}.Parts.back());
        VERIFY_ARE_EQUAL(4294967295u, usb::PortId{L"4294967295.1"}.Parts.front());
        VERIFY_ARE_EQUAL(std::wstring(L"uxd_port_001"), usb::uxd::GetUxdPortRegistryValueName(1));
        VERIFY_THROWS(usb::uxd::GetUxdPortRegistryValueName(1000), wil::ResultException);
        for (const auto* invalid :
             {L"",
              L"2",
              L"2-1",
              L"2:1",
              L"0.1",
              L"1.0",
              L"-1.2",
              L"1..2",
              L"1.2 ",
              L"4294967296.1",
              L".1",
              L"1.",
              L"1.2.",
              L"1.-2",
              L"1.2-3",
              L"1.2.0",
              L"1.4294967296"})
        {
            VERIFY_THROWS(usb::PortId{invalid}, wil::ResultException);
        }
    }

    WSL2_TEST_METHOD(ListHidesUnsupportedDevices)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);

        const fixture::Server server;
        const fixture::ScopedImport serial(*cli, server, fixture::Device::Serial);
        const fixture::ScopedImport audio(*cli, server, fixture::Device::Audio);
        const auto serialPort = WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached");
        const auto audioPort = WaitForDeviceStateAndGetPortId(server, fixture::Device::Audio, L"Unsupported");
        const auto ports = GetPortIdsFromDefaultList();
        VERIFY_ARE_EQUAL(1, std::ranges::count(ports, serialPort));
        VERIFY_ARE_EQUAL(0, std::ranges::count(ports, audioPort));
    }

    WSL2_TEST_METHOD(SerialAttachUseDetach)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        WslKeepAlive keepAlive(nullptr, LXSS_DISTRO_NAME_TEST_L);
        const fixture::Server server;
        const fixture::ScopedImport serial(*cli, server, fixture::Device::Serial);
        const auto port = WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached");

        auto attachment = AttachSerialDeviceToWsl(server, port);
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Attached"));
        VerifyGuestSerialLoopback(server);

        LxsstuLaunchWslAndCaptureOutput(std::format(L"--usb --detach {}", port));
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached"));
        WaitUntilDeviceIsAbsentInGuest(server, fixture::Device::Serial);
        VerifyHostSerialLoopback(server);
        attachment.release();
    }

    WSL2_TEST_METHOD(UnsupportedAudioAttachIsRejected)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        const fixture::Server server;
        const fixture::ScopedImport audio(*cli, server, fixture::Device::Audio);
        const auto port = WaitForDeviceStateAndGetPortId(server, fixture::Device::Audio, L"Unsupported");

        ValidateOutput(
            std::format(L"--usb --attach {}", port).c_str(),
            FormatErrorMessage(wsl::shared::Localization::MessageUsbAudioVideoNotSupported(), L"Wsl/Service/ERROR_NOT_SUPPORTED"));
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Audio, L"Unsupported"));
        VERIFY_ARE_EQUAL(0, std::ranges::count(GetPortIdsFromDefaultList(), port));
        WaitUntilDeviceIsAbsentInGuest(server, fixture::Device::Audio);
    }

    WSL2_TEST_METHOD(UnplugWhileAttachedThenReconnect)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        WslKeepAlive keepAlive(nullptr, LXSS_DISTRO_NAME_TEST_L);
        const fixture::Server server;
        std::optional<fixture::ScopedImport> serial{std::in_place, *cli, server, fixture::Device::Serial};
        auto attachment =
            AttachSerialDeviceToWsl(server, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached"));
        VerifyGuestSerialLoopback(server);

        serial.reset();
        wsl::shared::retry::RetryWithTimeout<void>(
            [&] {
                THROW_HR_IF_MSG(
                    E_PENDING, FindDeviceInVerboseList(server, fixture::Device::Serial).has_value(), "Waiting for USB removal");
            },
            std::chrono::milliseconds(100),
            std::chrono::seconds(15),
            [] { return wil::ResultFromCaughtException() == E_PENDING; });
        WaitUntilDeviceIsAbsentInGuest(server, fixture::Device::Serial);
        attachment.release();

        serial.emplace(*cli, server, fixture::Device::Serial);
        const auto reattached =
            AttachSerialDeviceToWsl(server, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached"));
        VerifyGuestSerialLoopback(server);
    }

    WSL2_TEST_METHOD(NonElevatedUserCanListButCannotChangeAttachment)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        WslKeepAlive keepAlive(nullptr, LXSS_DISTRO_NAME_TEST_L);
        const fixture::Server server;
        const fixture::ScopedImport serial(*cli, server, fixture::Device::Serial);
        const auto token = GetNonElevatedToken();
        const auto port = WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached", token.get());
        const auto verifyDenied = [&](LPCWSTR operation) {
            const auto [output, error] =
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--usb {} {}", operation, port), -1, nullptr, token.get());
            VERIFY_IS_TRUE(output.find(L"ERROR_ELEVATION_REQUIRED") != std::wstring::npos, output.c_str());
        };

        verifyDenied(L"--attach");
        const auto attachment = AttachSerialDeviceToWsl(server, port);
        verifyDenied(L"--detach");
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Attached", token.get()));
        VerifyGuestSerialLoopback(server);
    }

    WSL2_TEST_METHOD(ExplicitShutdownReleasesAttachment)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        if (IsAnotherDistroRunning())
        {
            LogSkipped("Shutdown would stop a distribution other than test_distro.");
            return;
        }
        const fixture::Server server;
        const fixture::ScopedImport serial(*cli, server, fixture::Device::Serial);
        const auto port = WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached");
        auto attachment = AttachSerialDeviceToWsl(server, port);
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Attached"));

        WslShutdown();
        VerifyHostSerialLoopback(server);
        WaitUntilDeviceIsAbsentInGuest(server, fixture::Device::Serial);
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached"));
        attachment.release();
    }

    WSL2_TEST_METHOD(ServiceCrashReturnsAttachedDeviceToHostOnRestart)
    {
        WSL_GET_USBIP_CLI_IF_SUPPORTED_OR_SKIP(cli);
        if (IsAnotherDistroRunning() || std::ranges::any_of(SvcComm{}.EnumerateUsbDevices(), [](const auto& device) {
                return device.State == usb::DeviceState::Attached || device.State == usb::DeviceState::Unavailable;
            }))
        {
            LogSkipped("Crashing WslService would interrupt another distribution or an existing USB reservation.");
            return;
        }

        LxsstuLaunchWslAndCaptureOutput(L"-d " LXSS_DISTRO_NAME_TEST_L L" --exec /bin/true");
        const fixture::Server server;
        const fixture::ScopedImport serial(*cli, server, fixture::Device::Serial);
        const auto port = WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached");
        auto attachment = AttachSerialDeviceToWsl(server, port);
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Attached"));

        CrashWslServiceAndRestartTestDistro();
        VerifyHostSerialLoopback(server);
        VERIFY_ARE_EQUAL(port, WaitForDeviceStateAndGetPortId(server, fixture::Device::Serial, L"Unattached"));
        attachment.release();
    }
};

} // namespace UsbTests