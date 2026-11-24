// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "UsbIpTestClient.hpp"
#include "SubProcess.h"
#include "retryshared.h"
#include <cfgmgr32.h>
#include <sstream>

namespace wsl::test::usbip {
namespace {

    std::wstring RunClient(const std::filesystem::path& cli, const std::wstring& arguments)
    {
        wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

        const auto command = std::format(L"\"{}\" {}", cli.native(), arguments);
        wsl::windows::common::SubProcess process(cli.c_str(), command.c_str());
        process.SetJobObject(job.get());
        auto output = process.RunAndCaptureOutput(30000);
        THROW_HR_IF_MSG(
            E_FAIL,
            output.ExitCode != 0,
            "usbip %ls exited with %lu: %ls %ls",
            arguments.c_str(),
            output.ExitCode,
            output.Stdout.c_str(),
            output.Stderr.c_str());
        return std::move(output.Stdout);
    }

} // namespace

std::optional<std::filesystem::path> FindClient()
{
    const auto cli = wsl::windows::common::filesystem::GetKnownFolderPath(FOLDERID_ProgramFiles, 0) / L"USBip" / L"usbip.exe";
    if (!std::filesystem::exists(cli))
    {
        return {};
    }
    return cli;
}

ScopedImport::ScopedImport(const std::filesystem::path& cli, const Server& server, Device device) :
    m_cli(cli), m_server(server), m_device(device), m_busId(server.ReserveImport(device))
{
    auto rollback = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { Detach(); });
    const auto bus = wsl::shared::string::MultiByteToWide(m_busId);
    RunClient(m_cli, std::format(L"--tcp-port {} attach --remote 127.0.0.1 --bus-id {} --once", server.Port(), bus));

    auto instance = server.InstanceId(device);
    wsl::shared::retry::RetryWithTimeout<void>(
        [&] {
            DEVINST node{};
            const auto result = CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL);
            THROW_WIN32_IF_MSG(ERROR_NOT_FOUND, result == CR_NO_SUCH_DEVNODE, "Waiting for virtual USB device %ls", instance.c_str());
            THROW_IF_WIN32_ERROR(CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE));
        },
        std::chrono::milliseconds(100),
        std::chrono::seconds(20),
        [] { return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND); });
    rollback.release();
}

void ScopedImport::Detach() const
{
    m_server.WithdrawImport(m_device, m_busId);
    const auto bus = wsl::shared::string::MultiByteToWide(m_busId);
    const auto uri = std::format(L"usbip://127.0.0.1:{}/{}", m_server.Port(), bus);
    auto stopRetries = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        RunClient(m_cli, std::format(L"--tcp-port {} attach --remote 127.0.0.1 --bus-id {} --stop", m_server.Port(), bus));
    });
    wsl::shared::retry::RetryWithTimeout<void>(
        [&] {
            std::wistringstream ports(RunClient(m_cli, L"port"));
            for (std::wstring field; ports >> field;)
            {
                THROW_HR_IF_MSG(E_PENDING, field == uri, "Waiting for import %ls to disappear", uri.c_str());
            }
        },
        std::chrono::milliseconds(100),
        std::chrono::seconds(15),
        [] { return wil::ResultFromCaughtException() == E_PENDING; });
}

ScopedImport::~ScopedImport()
try
{
    Detach();
}
CATCH_LOG()

} // namespace wsl::test::usbip