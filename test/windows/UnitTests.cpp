/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UnitTests.cpp

Abstract:

    This file contains unit tests for WSL.

--*/

#include "precomp.h"

#include "Common.h"
#include <AclAPI.h>
#include <fstream>
#include <filesystem>
#include "wslservice.h"
#include "registry.hpp"
#include "helpers.hpp"
#include "svccomm.hpp"
#include "lxfsshares.h"
#include <userenv.h>
#include <nlohmann/json.hpp>
#include "Distribution.h"
#include "WslCoreConfigInterface.h"
#include "CommandLine.h"

#define LXSST_TEST_USERNAME L"kerneltest"

#define LXSST_LXFS_TEST_DIR L"lxfstest"
#define LXSST_LXFS_MKDIR_COMMAND_LINE \
    L"/bin/bash -c \"mkdir /" LXSST_LXFS_TEST_DIR "; chown 1000:1001 /" LXSST_LXFS_TEST_DIR L"\""
#define LXSST_LXFS_CLEANUP_COMMAND_LINE L"/bin/bash -c \"rm -rf /" LXSST_LXFS_TEST_DIR L"\""
#define LXSST_LXFS_TEST_SUB_DIR L"testdir"

#define LXSST_FSTAB_BACKUP_COMMAND_LINE L"/bin/bash -c 'cp /etc/fstab /etc/fstab.bak'"
#define LXSST_FSTAB_SETUP_COMMAND_LINE L"/bin/bash -c 'echo C:\\\\ /mnt/c drvfs metadata 0 0 >> /etc/fstab'"
#define LXSST_FSTAB_CLEANUP_COMMAND_LINE L"/bin/bash -c \"cp /etc/fstab.bak /etc/fstab\""

#define LXSST_TESTS_INSTALL_COMMAND_LINE L"/bin/bash -c 'cd /data/test; ./build_tests.sh'"

#define LXSST_IMPORT_DISTRO_TEST_DIR L"C:\\importtest\\"

#define LXSST_UID_ROOT 0
#define LXSST_GID_ROOT 0
#define LXSST_USERNAME_ROOT L"root"

#define LXSS_OOBE_COMPLETE_NAME L"OOBEComplete"

constexpr auto c_testDistributionEndpoint = L"http://127.0.0.1:12345/";
constexpr auto c_testDistributionJson =
    LR"({
\"Distributions\":[
    {
        \"Name\": \"Debian\",
        \"FriendlyName\": \"Debian\",
        \"StoreAppId\": \"Dummy\",
        \"Amd64\": true,
        \"Arm64\": true,
        \"Amd64PackageUrl\": null,
        \"Arm64PackageUrl\": null,
        \"PackageFamilyName\": \"Dummy\"
    }
]})";

using wsl::windows::common::wslutil::GetSystemErrorString;

extern std::wstring g_testDistroPath;

namespace UnitTests {
class UnitTests
{
    WSL_TEST_CLASS(UnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);

        // Build the unit tests on the Linux side
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(LXSST_TESTS_INSTALL_COMMAND_LINE), (DWORD)0);

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuLaunchWsl(LXSST_LXFS_CLEANUP_COMMAND_LINE);
        LxsstuUninitialize(FALSE);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        LxssLogKernelOutput();
        return true;
    }

    // Note: This test should run first since other test cases create files extended attributes, which causes bdstar to emit warnings during export.
    TEST_METHOD(ExportDistro)
    {
        constexpr auto tarPath = L"exported-test-distro.tar";
        constexpr auto vhdPath = L"exported-test-distro.vhdx";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            LOG_IF_WIN32_BOOL_FALSE(DeleteFile(tarPath));
            LOG_IF_WIN32_BOOL_FALSE(DeleteFile(vhdPath));
        });

        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--export {} {}", LXSS_DISTRO_NAME_TEST_L, tarPath));

            VERIFY_ARE_EQUAL(out, L"The operation completed successfully. \r\n");
            VERIFY_ARE_EQUAL(err, L"");
        }

        // Validate that the file is a valid tar
        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"bash -c 'tar tf {} | grep -iF /root/.bashrc'", tarPath));
            VERIFY_ARE_EQUAL(out, L"./root/.bashrc\n");
            VERIFY_ARE_EQUAL(err, L"");
        }

        // Validate that gzip compression works
        {
            auto [out, err] =
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--export {} {} --format tar.gz", LXSS_DISTRO_NAME_TEST_L, tarPath));

            VERIFY_ARE_EQUAL(out, L"The operation completed successfully. \r\n");
            VERIFY_ARE_EQUAL(err, L"");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"gzip -t {}", tarPath)), 0L);
        }

        // Verify that xzip compression works
        {
            auto [out, err] =
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--export {} {} --format tar.xz", LXSS_DISTRO_NAME_TEST_L, tarPath));

            VERIFY_ARE_EQUAL(out, L"The operation completed successfully. \r\n");
            VERIFY_ARE_EQUAL(err, L"");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"xz -t {}", tarPath)), 0L);
        }

        // Validate that exporting as vhd works
        if (LxsstuVmMode())
        {
            WslShutdown(); // TODO: detach disk when distribution is stopped to remove this requirement.

            auto [out, err] =
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--export {} {} --format vhd", LXSS_DISTRO_NAME_TEST_L, vhdPath));

            VERIFY_ARE_EQUAL(out, L"The operation completed successfully. \r\n");
            VERIFY_ARE_EQUAL(err, L"");

            auto [vhdType, _] = LxsstuLaunchPowershellAndCaptureOutput(std::format(L"(Get-VHD '{}').VhdType", vhdPath));
            VERIFY_ARE_EQUAL(vhdType, L"Dynamic\r\n");
        }
        else
        {
            auto [out, err] =
                LxsstuLaunchWslAndCaptureOutput(std::format(L"--export {} {} --format vhd", LXSS_DISTRO_NAME_TEST_L, vhdPath), -1);

            VERIFY_ARE_EQUAL(out, L"This operation is only supported by WSL2.\r\nError code: Wsl/Service/WSL_E_WSL2_NEEDED\r\n");
            VERIFY_ARE_EQUAL(err, L"");
        }
    }

    TEST_METHOD(SystemdSafeMode)
    {
        WSL2_TEST_ONLY();

        SKIP_TEST_UNSTABLE(); // TODO: Re-enable when this issue is solved in main.

        auto revert = EnableSystemd();

        // generate a new test config with safe mode enabled
        WslConfigChange config(LxssGenerateTestConfig({.safeMode = true}));

        // verify that even though systemd is enabled, safe mode prevents it from executing
        VERIFY_IS_FALSE(IsSystemdRunning(L"--system", 1));

        config.Update(L"");

        // disable safe mode and verify that it systemd runs
        VERIFY_IS_TRUE(IsSystemdRunning(L"--system"));
    }

    TEST_METHOD(SystemdDisabled)
    {
        WSL2_TEST_ONLY();

        // tests that systemd does not run without the wsl.conf option enabled
        // run and check the output of systemctl --system
        VERIFY_IS_FALSE(IsSystemdRunning(L"--system", 1));
    }

    TEST_METHOD(SystemdSystem)
    {
        WSL2_TEST_ONLY();

        auto cleanup = wil::scope_exit([] {
            // clean up wsl.conf file
            const std::wstring disableSystemdCmd(LXSST_REMOVE_DISTRO_CONF_COMMAND_LINE);
            LxsstuLaunchWsl(disableSystemdCmd);
            TerminateDistribution();
        });

        auto revert = EnableSystemd();
        VERIFY_IS_TRUE(IsSystemdRunning(L"--system"));
    }

    TEST_METHOD(SystemdUser)
    {
        WSL2_TEST_ONLY();

        // enable systemd before creating the user.
        // if not called first, the runtime directories needed for --user will not have been created
        auto cleanup = EnableSystemd();

        // create test user and run test as that user
        ULONG TestUid;
        ULONG TestGid;
        CreateUser(LXSST_TEST_USERNAME, &TestUid, &TestGid);
        auto userCleanup = wil::scope_exit([]() { LxsstuLaunchWsl(L"userdel " LXSST_TEST_USERNAME); });

        auto validateUserSesssion = [&]() {
            // verify that the user service is running
            const std::wstring isServiceActiveCmd =
                std::format(L"-u {} systemctl is-active user@{}.service ; exit 0", LXSST_TEST_USERNAME, TestUid);
            std::wstring out;
            std::wstring err;

            try
            {
                std::tie(out, err) = LxsstuLaunchWslAndCaptureOutput(isServiceActiveCmd.data());
            }
            CATCH_LOG();

            Trim(out);

            if (out.compare(L"active") != 0)
            {
                LogError(
                    "Unexpected output from systemd: %ls. Stderr: %ls, cmd: %ls", out.c_str(), err.c_str(), isServiceActiveCmd.c_str());
                VERIFY_FAIL();
            }

            // Verify that /run/user/<uid> is a writable tmpfs mount visible in both mount namespaces.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"touch /run/user/" + std::to_wstring(TestUid) + L"/dummy-test-file"), 0u);
            auto command = L"mount | grep -iF 'tmpfs on /run/user/" + std::to_wstring(TestUid) + L" type tmpfs (rw'";
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(command), 0u);

            const auto nonElevatedToken = GetNonElevatedToken();
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(command, nullptr, nullptr, nullptr, nonElevatedToken.get()), 0u);
        };

        // Validate user sessions state with gui apps disabled.
        {
            validateUserSesssion();

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"echo $DISPLAY", LXSST_TEST_USERNAME));
            VERIFY_ARE_EQUAL(out, L"\n");
        }

        // Validate user sessions state with gui apps enabled.
        {
            WslConfigChange config(LxssGenerateTestConfig({.guiApplications = true}));

            validateUserSesssion();
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"echo $DISPLAY", LXSST_TEST_USERNAME));
            VERIFY_ARE_EQUAL(out, L":0\n");
        }

        // Create a 'broken' /run/user and validate that the warning is correctly displayed.
        {
            TerminateDistribution();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"chmod 000 /run/user"), 0L);

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"-u {} echo OK", LXSST_TEST_USERNAME));

            VERIFY_ARE_EQUAL(out, L"OK\n");
            VERIFY_ARE_EQUAL(
                err, L"wsl: Failed to start the systemd user session for 'kerneltest'. See journalctl for more details.\n");
        }
    }

    static bool IsSystemdRunning(const std::wstring& SystemdScope, int ExpectedExitCode = 0)
    {
        // run and check the output of systemctl --system
        const auto systemctlCmd = std::format(L"systemctl '{}' is-system-running ; exit 0", SystemdScope);
        std::wstring out;
        std::wstring error;

        // capture the output of systemctl and trim for good measure
        try
        {
            std::tie(out, error) = LxsstuLaunchWslAndCaptureOutput(systemctlCmd.c_str(), ExpectedExitCode);
        }
        CATCH_LOG()
        Trim(out);

        // ensure that systemd is either running in a degraded or running state
        if ((out.compare(L"degraded") == 0) || (out.compare(L"running") == 0))
        {
            return true;
        }
        LogInfo(
            "Error when checking if systemd is running: %ls (scope: %ls, stderr: %ls)", out.c_str(), SystemdScope.c_str(), error.c_str());
        return false;
    }

    TEST_METHOD(SystemdNoClearTmpUnit)
    {
        WSL2_TEST_ONLY();

        // ensures that we don't leave state on exit
        auto cleanup = EnableSystemd("initTimeout=0");

        // Wait for systemd to be started
        VERIFY_NO_THROW(wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(E_UNEXPECTED, !IsSystemdRunning(L"--system")); }, std::chrono::seconds(1), std::chrono::minutes(1)));

        // Validate that the X11 socket has not been deleted
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -d /tmp/.X11-unix"), 0L);
    }

    TEST_METHOD(SystemdBinfmtIsRestored)
    {
        WSL2_TEST_ONLY();

        // Override WSL's binfmt interpreter
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo ':WSLInterop:M::MZ::/bin/echo:PF' > /usr/lib/binfmt.d/dummy.conf"), 0L);

        auto cleanupBinfmt = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            LxsstuLaunchWsl(L"rm /usr/lib/binfmt.d/dummy.conf");
            WslShutdown(); // Required since this test registers a custom binfmt interpreter.
        });

        {
            // Enable systemd (restarts distro).
            auto cleanupSystemd = EnableSystemd();

            auto validateBinfmt = []() {
                // Validate that WSL's binfmt interpreter is still in place.
                auto [cmdOutput, _] = LxsstuLaunchWslAndCaptureOutput(L"cmd.exe /c echo ok");
                VERIFY_ARE_EQUAL(cmdOutput, L"ok\r\n");
            };

            validateBinfmt();

            // Validate that this still works after restarting the distribution.
            TerminateDistribution();
            validateBinfmt();

            // Validate that stopping or restarting systemd-binfmt doesn't break interop.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"systemctl stop systemd-binfmt.service"), 0u);
            validateBinfmt();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"systemctl restart systemd-binfmt.service"), 0u);
            validateBinfmt();

            // Validate that the unit is regenerated after a daemon-reload.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"systemctl daemon-reload && systemctl restart systemd-binfmt.service"), 0u);
            validateBinfmt();
        }

        {
            // Enable systemd (restarts distro).
            auto cleanupSystemd = EnableSystemd("protectBinfmt=false");

            // Validate that WSL's binfmt interpreter is overriden
            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"cmd.exe /c echo ok");
            VERIFY_IS_TRUE(wsl::shared::string::IsEqual(output, L"/mnt/c/Windows/system32/cmd.exe cmd.exe /c echo ok\n", true));
        }
    }

    TEST_METHOD(Dup)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests dup", L"Dup"));
    }

    TEST_METHOD(Epoll)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests epoll", L"Epoll"));
    }

    TEST_METHOD(EventFd)
    {

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests eventfd", L"EventFd"));
    }

    TEST_METHOD(Flock)
    {

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests flock", L"Flock"));
    }

    TEST_METHOD(Fork)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests fork", L"Fork"));
    }

    TEST_METHOD(FsCommonLxFs)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests fscommon", L"fscommon_lxfs"));
    }

    TEST_METHOD(GetSetId)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests get_set_id", L"get_set_id"));
    }

    TEST_METHOD(Inotify)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests inotify", L"INOTIFY"));
    }

#if !defined(_ARM64_)

    TEST_METHOD(ResourceLimits)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests resourcelimits", L"resourcelimits"));
    }

    TEST_METHOD(Select)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests select", L"Select"));
    }

#endif

    TEST_METHOD(Madvise)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests madvise", L"madvise"));
    }

    TEST_METHOD(Mprotect)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests mprotect", L"mprotect"));
    }

    TEST_METHOD(Pipe)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests pipe", L"Pipe"));
    }

    TEST_METHOD(Sched)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests sched", L"sched"));
    }

    TEST_METHOD(SocketNonblocking)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests socket_nonblock", L"socket_nonblocking"));
    }

    TEST_METHOD(Splice)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests splice", L"Splice"));
    }

    TEST_METHOD(Sysfs)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests sysfs", L"SysFs"));
    }

    TEST_METHOD(Tty)
    {
        WSL1_TEST_ONLY();

        auto OriginalHandles = UseOriginalStdHandles();

        auto Restore = wil::scope_exit([&OriginalHandles]() { RestoreTestStdHandles(OriginalHandles); });

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests tty", L"tty"));
    }

    TEST_METHOD(Utimensat)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests utimensat", L"Utimensat"));
    }

    TEST_METHOD(WaitPid)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests waitpid", L"WaitPid"));
    }

    TEST_METHOD(Brk)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests brk", L"brk"));
    }

    TEST_METHOD(Mremap)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests mremap", L"mremap"));
    }

    TEST_METHOD(VfsAccess)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests vfsaccess", L"vfsaccess"));
    }

    TEST_METHOD(DevPt)
    {
        WSL1_TEST_ONLY();

        auto OriginalHandles = UseOriginalStdHandles();

        auto Restore = wil::scope_exit([&OriginalHandles]() { RestoreTestStdHandles(OriginalHandles); });

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests dev_pt", L"dev_pt"));

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests dev_pt_2", L"dev_pt_2"));
    }

    TEST_METHOD(Timer)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests timer", L"timer"));
    }

    TEST_METHOD(SysInfo)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests sysinfo", L"Sysinfo"));
    }

    TEST_METHOD(TimerFd)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests timerfd", L"timerfd"));
    }

    TEST_METHOD(Ioprio)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests ioprio", L"Ioprio"));
    }

    TEST_METHOD(Interop)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests interop", L"interop"));

        //
        // Run wsl.exe with a very long command line. This ensures that the buffer
        // resizing logic that is used by the WSL init daemon is able to correctly
        // handle very long messages.
        //
        // N.B. /bin/true ignores all arguments and always returns 0.
        //

        std::wstring Command{L"/bin/true "};
        Command += std::wstring(0x1000, L'x');
        VERIFY_IS_TRUE(LxsstuLaunchWsl(Command.c_str()) == 0);

        // Validate that windows executable can run from the linux filesystem. See: https://github.com/microsoft/WSL/issues/10812
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"cp /mnt/c/Program\\ Files/WSL/wsl.exe /tmp"), 0L);
        auto [out, _] =
            LxsstuLaunchWslAndCaptureOutput(L"WSLENV=WSL_UTF8 WSL_UTF8=1 WSL_INTEROP=/run/WSL/1_interop /tmp/wsl.exe --version");

        VERIFY_IS_TRUE(out.find(TEXT(WSL_PACKAGE_VERSION)) != std::string::npos);
    }

    static std::wstring FormUserCommandLine(_In_ const std::wstring& Username, _In_ ULONG Uid, _In_ ULONG Gid)
    {
        return std::format(L"/data/test/wsl_unit_tests user {} {} {}", Username, Uid, Gid);
    }

    TEST_METHOD(User)
    {
        //
        // Create a test user and run the test as that user.
        //

        ULONG TestUid;
        ULONG TestGid;
        CreateUser(LXSST_TEST_USERNAME, &TestUid, &TestGid);
        std::wstring CommandLine = FormUserCommandLine(LXSST_TEST_USERNAME, TestUid, TestGid);
        LogInfo("Running test as user %s", LXSST_TEST_USERNAME);
        VERIFY_NO_THROW(LxsstuRunTest(CommandLine.c_str(), L"user", LXSST_TEST_USERNAME));

        //
        // Add the user to 64 more groups to make sure > 32 groups is supported.
        //

        {
            DistroFileChange groups(L"/etc/group", true);
            CommandLine = std::format(L"-- for i in $(seq 1 64); do groupadd group$i; usermod -a -G group$i {}; done", LXSST_TEST_USERNAME);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(CommandLine), (DWORD)0);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"{} {} {}", WSL_USER_ARG_LONG, LXSST_TEST_USERNAME, "echo success")), (DWORD)0);
        }

        //
        // Run the test as root.
        //

        ULONG RootUid;
        ULONG RootGid;
        CreateUser(LXSST_USERNAME_ROOT, &RootUid, &RootGid);
        CommandLine = FormUserCommandLine(LXSST_USERNAME_ROOT, LXSST_UID_ROOT, LXSST_GID_ROOT);
        LogInfo("Running test as user %s", LXSST_USERNAME_ROOT);
        VERIFY_NO_THROW(LxsstuRunTest(CommandLine.c_str(), L"user", LXSST_USERNAME_ROOT));

        //
        // Set the default user to the newly created user.
        //
        // N.B. Modifying the default UID should cause the instance to be recreated and the plan9 server launched as the default user.
        //

        const auto wslSupport =
            wil::CoCreateInstance<LxssUserSession, IWslSupport>(CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING | CLSCTX_ENABLE_AAA);

        ULONG Version;
        ULONG DefaultUid;
        wil::unique_cotaskmem_array_ptr<wil::unique_cotaskmem_ansistring> DefaultEnvironment{};
        ULONG WslFlags;
        VERIFY_SUCCEEDED(wslSupport->GetDistributionConfiguration(
            LXSS_DISTRO_NAME_TEST_L, &Version, &DefaultUid, DefaultEnvironment.size_address<ULONG>(), &DefaultEnvironment, &WslFlags));

        VERIFY_SUCCEEDED(wslSupport->SetDistributionConfiguration(LXSS_DISTRO_NAME_TEST_L, TestUid, WslFlags));
        auto cleanup = wil::scope_exit([&] {
            try
            {
                VERIFY_SUCCEEDED(wslSupport->SetDistributionConfiguration(LXSS_DISTRO_NAME_TEST_L, DefaultUid, WslFlags));
            }
            catch (...)
            {
                LogError("Error while restoring default user");
            }
        });

        //
        // Create a new file using the 9p server.
        //

        const std::wstring Path = L"\\\\wsl.localhost\\" LXSS_DISTRO_NAME_TEST_L L"\\data\\test\\default_user_test";
        const wil::unique_hfile File(CreateFile(
            Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));

        if (!File)
        {
            LogError("Failed to create file, error=%lu", GetLastError());
            VERIFY_FAIL();
        }

        //
        // Ensure the new file was created with the correct uid.
        //

        VERIFY_ARE_EQUAL(
            LxsstuLaunchWsl(L"stat -c %U /data/test/default_user_test | grep -iF kerneltest", nullptr, nullptr, nullptr, nullptr), 0u);
    }

    TEST_METHOD(Execve)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests execve", L"Execve"));
    }

    TEST_METHOD(Xattr)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests xattr", L"xattr"));
    }

    TEST_METHOD(Namespace)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests namespace", L"Namespace"));
    }

    TEST_METHOD(BinFmt)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests binfmt", L"BinFmt"));

        //
        // Perform a shutdown since the binfmt test modifies the binfmt config.
        //

        WslShutdown();
    }

    TEST_METHOD(Cgroup)
    {
        //
        // For WSL1, run the cgroup unit test. For WSL2, ensure the cgroupv2 filesystem is mounted in the expected location.
        //

        if (!LxsstuVmMode())
        {
            VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests cgroup", L"cgroup"));
        }
        else
        {
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(
                    L"mount | grep -iF 'cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,nsdelegate)'", nullptr, nullptr, nullptr, nullptr),
                0u);
        }
    }

    TEST_METHOD(Netlink)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests netlink", L"Netlink"));
    }

    TEST_METHOD(Random)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests random", L"random"));
    }

    TEST_METHOD(Keymgmt)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests keymgmt", L"Keymgmt"));
    }

    TEST_METHOD(Shm)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests shm", L"shm"));
    }

    TEST_METHOD(Sem)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests sem", L"sem"));
    }

    TEST_METHOD(Ttys)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests ttys", L"Ttys"));
    }

    TEST_METHOD(OverlayFs)
    {
        WSL1_TEST_ONLY();

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests overlayfs", L"OverlayFs"));
    }

    TEST_METHOD(Auxv)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests auxv", L"auxv"));
    }

    TEST_METHOD(WslInfo)
    {
        if (LxsstuVmMode())
        {
            // Ensure the `-n` option to not print newline works by validating newline counts.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | wc -l | grep 1"), 0u);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode -n | wc -l | grep 0"), 0u);

            // Ensure various wslinfo functionaly works as expected.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'nat'"), 0u);

            WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::None}));
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'none'"), 0u);

            if (AreExperimentalNetworkingFeaturesSupported() && IsHyperVFirewallSupported())
            {
                config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'mirrored'"), 0u);
            }

            for (const auto enabled : {true, false})
            {
                config.Update(LxssGenerateTestConfig({.guiApplications = enabled}));

#ifdef WSL_DEV_INSTALL_PATH

                VERIFY_ARE_EQUAL(
                    LxsstuLaunchWsl(std::format(L"wslinfo --msal-proxy-path | grep -iF $(wslpath '{}')", TEXT(WSL_DEV_INSTALL_PATH))), 0u);

#else

                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --msal-proxy-path | grep -iF '/mnt/c/Program Files/WSL/msal.wsl.proxy.exe'"), 0u);

#endif
            }
        }
        else
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'wsl1'"), 0u);
        }

        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"wslinfo --version");
            VERIFY_ARE_EQUAL(out, std::format(L"{}\n", WSL_PACKAGE_VERSION));
            VERIFY_ARE_EQUAL(err, L"");
        }

        {
            // Ensure the old version query command still works.
            const auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"wslinfo --wsl-version");
            VERIFY_ARE_EQUAL(out, std::format(L"{}\n", WSL_PACKAGE_VERSION));
            VERIFY_ARE_EQUAL(err, L"");
        }

        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"wslinfo --invalid", 1);
            VERIFY_ARE_EQUAL(out, L"");
            VERIFY_ARE_EQUAL(
                err,
                L"Invalid command line argument: --invalid\nPlease use 'wslinfo --help' to get a list of supported "
                L"arguments.\n");
        }
    }

    TEST_METHOD(WslPath)
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests wslpath", L"wslpath"));
    }

    TEST_METHOD(FsTab)
    {
        //
        // Revert the fstab file and restart the instance so everything is back in
        // the default state after this test.
        //

        auto cleanup = wil::scope_exit([&] {
            try
            {
                LxsstuLaunchWsl(LXSST_FSTAB_CLEANUP_COMMAND_LINE);
                TerminateDistribution();
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"/bin/true"), 0u);
            }
            catch (...)
            {
                LogError("Error while cleaning up the fstab");
            }
        });

        //
        // Create an entry in the /etc/fstab file to explicitly mount C:.
        //

        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(LXSST_FSTAB_BACKUP_COMMAND_LINE));
        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(LXSST_FSTAB_SETUP_COMMAND_LINE));
        TerminateDistribution();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"/bin/true"), 0u);

        //
        // The test will make sure /mnt/c is mounted with the options specified in
        // /etc/fstab, and that it's mounted only once.
        //

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests fstab", L"fstab"));
    }

    TEST_METHOD(X11SocketOverTmpMount)
    {
        if (!LxsstuVmMode())
        {
            return;
        }

        auto cleanup = wil::scope_exit([&] {
            try
            {
                LxsstuLaunchWsl(LXSST_FSTAB_CLEANUP_COMMAND_LINE);
                TerminateDistribution();
            }
            catch (...)
            {
                LogError("Error while cleaning up the fstab");
            }
        });

        WslConfigChange configChange(LxssGenerateTestConfig({.guiApplications = true}));

        //
        // Create an entry in the /etc/fstab file to add a tmpfs over /tmp.
        //

        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(LXSST_FSTAB_BACKUP_COMMAND_LINE));
        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(L"echo 'tmpfs /tmp tmpfs rw,nodev,nosuid,size=50M 0 0' > /etc/fstab"));
        TerminateDistribution();

        auto ValidateBindMount = [](HANDLE Token) {
            //
            // Validate that the bind mount is present.
            //

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L" mount | grep -iF 'none on /tmp/.X11-unix type tmpfs'", nullptr, nullptr, nullptr, Token), 0u);
        };

        //
        // Verify that /tmp is mounted in both namespaces.
        //

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"mount | grep -iF 'tmpfs on /tmp type tmpfs'", nullptr, nullptr, nullptr, nullptr), 0u);

        const auto nonElevatedToken = GetNonElevatedToken();
        VERIFY_ARE_EQUAL(
            LxsstuLaunchWsl(L"mount | grep -iF 'tmpfs on /tmp type tmpfs'", nullptr, nullptr, nullptr, nonElevatedToken.get()), 0u);

        //
        // Validate that the X11 bind mount is present and valid in both namespaces.
        //

        ValidateBindMount(nullptr);
        ValidateBindMount(nonElevatedToken.get());
    }

    TEST_METHOD(ImportDistro)
    {
        const auto tarFileName = LXSST_IMPORT_DISTRO_TEST_DIR L"test.tar";
        const auto rootfsDirectoryName = LXSST_IMPORT_DISTRO_TEST_DIR L"rootfs";
        const auto vhdFileName = LXSST_IMPORT_DISTRO_TEST_DIR L"ext4.vhdx";
        auto cleanup = wil::scope_exit([&] {
            try
            {
                VERIFY_IS_TRUE(DeleteFileW(tarFileName));
                VERIFY_IS_TRUE(RemoveDirectoryW(rootfsDirectoryName));
                VERIFY_IS_TRUE(DeleteFileW(vhdFileName));
                VERIFY_IS_TRUE(RemoveDirectoryW(LXSST_IMPORT_DISTRO_TEST_DIR));
            }
            catch (...)
            {
                LogError("Error during cleanup")
            }
        });

        //
        // Create a dummy tar file, rootfs folder, and vhdx. These will be used
        // to ensure that the user cannot import a distribution over an existing one
        // even if distro registration registry keys are not present.
        //

        VERIFY_IS_TRUE(CreateDirectoryW(LXSST_IMPORT_DISTRO_TEST_DIR, NULL));
        VERIFY_IS_TRUE(CreateDirectoryW(rootfsDirectoryName, NULL));

        {
            const wil::unique_hfile tarFile{CreateFileW(
                tarFileName, GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)};

            VERIFY_IS_FALSE(!tarFile);

            const wil::unique_hfile vhdFile{CreateFileW(
                vhdFileName, GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)};

            VERIFY_IS_FALSE(!vhdFile);
        }

        auto validateOutput = [](LPCWSTR commandLine, LPCWSTR expectedOutput, DWORD expectedExitCode = -1) {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(commandLine, expectedExitCode);
            VERIFY_ARE_EQUAL(expectedOutput, out);
            VERIFY_ARE_EQUAL(L"", err);
        };

        auto version = LxsstuVmMode() ? 2 : 1;
        auto commandLine = std::format(L"--import dummy {} {} --version {}", LXSST_IMPORT_DISTRO_TEST_DIR, tarFileName, version);
        validateOutput(
            commandLine.c_str(),
            L"The supplied install location is already in use.\r\n"
            L"Error code: Wsl/Service/RegisterDistro/ERROR_FILE_EXISTS\r\n");

        commandLine = std::format(L"--import dummy {} {} --version {}", LXSST_IMPORT_DISTRO_TEST_DIR, vhdFileName, version);
        validateOutput(commandLine.c_str(), L"This looks like a VHDX file. Use --vhd to import a VHDX instead of a tar.\r\n");

        if (!LxsstuVmMode())
        {
            commandLine = std::format(L"--import dummy {} {} --vhd --version 1", LXSST_IMPORT_DISTRO_TEST_DIR, vhdFileName);
            validateOutput(
                commandLine.c_str(),
                L"This operation is only supported by WSL2.\r\n"
                L"Error code: Wsl/Service/RegisterDistro/WSL_E_WSL2_NEEDED\r\n");
        }

        //
        // Create and import a new distro that where /bin/sh is an absolute symlink.
        //

        auto newDistroName = L"symlink_distro";
        auto newDistroTar = L"symlink_distro.tar";
        validateOutput(
            std::format(L"--export {} {}", LXSS_DISTRO_NAME_TEST_L, newDistroTar).c_str(),
            L"The operation completed successfully. \r\n",
            0);

        auto deleteNewDistro = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            VERIFY_IS_TRUE(DeleteFileW(newDistroTar));
            LxsstuLaunchWsl(std::format(L"--unregister {}", newDistroName));
        });

        validateOutput(
            std::format(L"--import {} . {} --version {}", newDistroName, newDistroTar, version).c_str(),
            L"The operation completed successfully. \r\n",
            0);
        validateOutput(std::format(L"-d {} -- ln -f -s /bin/bash /bin/sh", newDistroName).c_str(), L"", 0);
        validateOutput(
            std::format(L"--export {} {}", newDistroName, newDistroTar).c_str(), L"The operation completed successfully. \r\n", 0);
        validateOutput(std::format(L"--unregister {}", newDistroName).c_str(), L"The operation completed successfully. \r\n", 0);
        validateOutput(
            std::format(L"--import {} . {} --version {}", newDistroName, newDistroTar, version).c_str(),
            L"The operation completed successfully. \r\n",
            0);
    }

    TEST_METHOD(ImportDistroInvalidTar)
    {
        const auto commandLine = std::format(
            L"--import dummy {} C:\\windows\\system32\\drivers\\etc\\hosts --version {}", LXSST_IMPORT_DISTRO_TEST_DIR, LxsstuVmMode() ? 2 : 1);

        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(commandLine.c_str(), -1);

        VERIFY_ARE_EQUAL(
            out, L"Importing the distribution failed.\r\nError code: Wsl/Service/RegisterDistro/WSL_E_IMPORT_FAILED\r\n");
        VERIFY_ARE_EQUAL(err, L"bsdtar: Error opening archive: Unrecognized archive format\n");
    }

    TEST_METHOD(AppxDistroDeletion)
    {
        // Create a dummy distro registration
        const auto key = wsl::windows::common::registry::CreateKey(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\{baa405ef-1822-4bbe-84e2-30e4c6330d41}");

        wsl::windows::common::registry::WriteDword(key.get(), nullptr, L"State", 1);
        wsl::windows::common::registry::WriteString(key.get(), nullptr, L"DistributionName", L"DistroToBeDeleted");
        wsl::windows::common::registry::WriteString(
            key.get(), nullptr, L"PackageFamilyName", L"Microsoft.AppThatIsntInstalledForSure.1.0.0.0_8wekyb3d8bbwe");
        wsl::windows::common::registry::WriteDword(key.get(), nullptr, L"Version", 2);

        const auto vhdDir = std::filesystem::current_path();
        wsl::windows::common::registry::WriteString(key.get(), nullptr, L"BasePath", vhdDir.c_str());
        wsl::windows::common::registry::WriteDword(key.get(), nullptr, L"DefaultUid", 0);
        wsl::windows::common::registry::WriteDword(key.get(), nullptr, L"Flags", LXSS_DISTRO_FLAGS_VM_MODE);

        // Create a dummy vhd
        const auto vhdPath = vhdDir.string() + "\\ext4.vhdx";

        wil::unique_handle vhdHandle(CreateFileA(vhdPath.c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
        VERIFY_IS_TRUE(vhdHandle.is_valid());
        vhdHandle.reset();

        wsl::windows::common::SvcComm service;
        auto isDistroListed = [&]() {
            auto distros = service.EnumerateDistributions();

            return std::find_if(distros.begin(), distros.end(), [&](const auto& e) {
                       return wsl::shared::string::IsEqual(e.DistroName, L"DistroToBeDeleted", false);
                   }) != distros.end();
        };

        // The distro should still be there, because the vhd exists.
        VERIFY_IS_TRUE(isDistroListed());

        // Delete the VHD
        VERIFY_IS_TRUE(DeleteFileA(vhdPath.c_str()));

        // Now the distro should be deleted.
        VERIFY_IS_FALSE(isDistroListed());
    }

    // Validate that the default distribution is correctly displayed
    TEST_METHOD(DefaultDistro)
    {
        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--list");

        VERIFY_IS_TRUE(out.find(std::format(L"{} (Default)", LXSS_DISTRO_NAME_TEST_L)) != std::wstring::npos);
        VERIFY_ARE_EQUAL(err, L"");
    }

    // TODO: Add test coverage for the Linux => Windows code paths of $WSLENV
    TEST_METHOD(WslEnv)
    {
        auto validateEnv = [&](const std::map<std::wstring, std::wstring>& inputVariables,
                               const std::map<std::wstring, std::wstring>& expectedOutput) {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                for (const auto& e : inputVariables)
                {
                    THROW_LAST_ERROR_IF(!SetEnvironmentVariable(e.first.c_str(), nullptr));
                }
            });

            for (const auto& e : inputVariables)
            {
                THROW_LAST_ERROR_IF(!SetEnvironmentVariable(e.first.c_str(), e.second.c_str()));
            }

            for (const auto& e : expectedOutput)
            {
                auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"echo -n $" + e.first);

                VERIFY_ARE_EQUAL(e.second, output);
            }
        };

        validateEnv({{L"a", L"b"}, {L"c", L"d"}, {L"WSLENV", L"a/u:c/u"}}, {{L"a", L"b"}, {L"c", L"d"}});
        validateEnv(
            {{L"a", L"C:\\Users"}, {L"b", L"C:\\Users"}, {L"WSLENV", L"a/l:b/p"}},
            {{L"a", L"/mnt/c/Users"}, {L"b", L"/mnt/c/Users"}});

        validateEnv(
            {{L"a", L"C:\\Users;C:\\Windows"},
             {L"b", L"C:\\Users;C:\\Windows"},
             {L"c", L"C:\\Users;C:\\Windows"},
             {L"d", L"C:\\Users;C:\\Windows"},
             {L"WSLENV", L"a/l:b/p:c/pl:d/lp"}},
            {{L"a", L"/mnt/c/Users:/mnt/c/Windows"},
             {L"b", L"/mnt/c/Users:/mnt/c/Windows"},
             {L"c", L"/mnt/c/Users:/mnt/c/Windows"},
             {L"d", L"/mnt/c/Users:/mnt/c/Windows"}});

        validateEnv(
            {{L"a", L"C:\\Users;C:\\Windows\\System32"}, {L"b", L"C:\\Users;C:\\Windows"}, {L"WSLENV", L"a/l:b/l:a/l"}},
            {{L"a", L"/mnt/c/Users:/mnt/c/Windows/System32"}, {L"b", L"/mnt/c/Users:/mnt/c/Windows"}});

        validateEnv(
            {{L"a", L"C:\\Users;C:\\Windows\\System32"}, {L"b", L"C:\\Users;C:\\Windows"}, {L"WSLENV", L"a/u:b/u:a/u"}},
            {{L"a", L"C:\\Users;C:\\Windows\\System32"}, {L"b", L"C:\\Users;C:\\Windows"}});

        validateEnv({{L"a", L"C:\\Users;C:\\Windows\\System32"}, {L"WSLENV", L"a/w"}}, {{L"a", L""}});

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"Empty", nullptr));
            THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"WSLENV", nullptr));
        });

        THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"Empty", L""));
        THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"WSLENV", L"Empty/u"));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"[ -z ${Empty+x} ]"), (DWORD)1);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"[ -z ${SanityCheck+x} ]"), (DWORD)0);
    }

    static void ValidateErrorMessage(
        const std::wstring& Cmd,
        const std::wstring& Message,
        const std::wstring& Code,
        const std::optional<std::wstring>& ExtraConfig = {},
        LPCWSTR EntryPoint = WSL_BINARY_NAME,
        bool ignoreCasing = false)
    {
        std::optional<std::wstring> previousConfig;

        if (ExtraConfig.has_value())
        {
            previousConfig = LxssWriteWslConfig(L"[wsl2]\n" + ExtraConfig.value());
            RestartWslService();
        }

        auto revertConfig = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (previousConfig.has_value())
            {
                LxssWriteWslConfig(previousConfig.value());
                RestartWslService();
            };
        });

        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(
            Cmd.c_str(), wcscmp(EntryPoint, L"bash.exe") == 0 ? 1 : -1, nullptr, nullptr, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, EntryPoint);

        const auto expectedOutput = Message + L"\r\nError code: " + Code + L"\r\n";

        if (!wsl::shared::string::IsEqual(output, expectedOutput, ignoreCasing))
        {
            LogError("Expected error message: '%ls', actual error message: '%ls'", expectedOutput.c_str(), output.c_str());
            VERIFY_FAIL();
        }
    }

    static void VerifyOutput(const std::wstring& Cmd, const std::wstring& ExpectedOutput, int ExpectedExitCode = 0, LPCWSTR EntryPoint = WSL_BINARY_NAME)
    {
        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(
            Cmd.c_str(), ExpectedExitCode, nullptr, nullptr, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, EntryPoint);

        VERIFY_ARE_EQUAL(output, ExpectedOutput);
    }

    TEST_METHOD(ErrorMessages)
    {
        if (LxsstuVmMode()) // wsl --mount and bridged networking only exist in WSL2.
        {
            if (!wsl::shared::Arm64 && wsl::windows::common::helpers::GetWindowsVersion().BuildNumber >= 27653)
            {
                ValidateErrorMessage(
                    L"--mount DoesNotExist",
                    L"Failed to attach disk 'DoesNotExist' to WSL2: The system cannot find the file specified. ",
                    L"Wsl/Service/AttachDisk/MountDisk/HCS/ERROR_FILE_NOT_FOUND");
            }

            ValidateErrorMessage(
                L"--unmount DoesNotExist",
                GetSystemErrorString(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)),
                L"Wsl/Service/DetachDisk/ERROR_FILE_NOT_FOUND");

            ValidateErrorMessage(
                WSL_MANAGE_ARG L" " LXSS_DISTRO_NAME_TEST L" " WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG L" fulse",
                L"fulse is not a valid boolean, <true|false>",
                L"Wsl/E_INVALIDARG");

            const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();
            {
                // Create a distro registration pointing to a vhdx that doesn't exist and validate that the error message reports that correctly.

                const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
                const auto distroKey =
                    wsl::windows::common::registry::CreateKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");
                auto revert = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                    wsl::windows::common::registry::DeleteKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");
                });

                wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"BasePath", L"C:\\DoesNotExit");
                wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"DistributionName", L"DummyBrokenDistro");
                wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"DefaultUid", 0);
                wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Version", LXSS_DISTRO_VERSION_2);
                wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"State", LxssDistributionStateInstalled);
                wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Flags", LXSS_DISTRO_FLAGS_VM_MODE);

                ValidateErrorMessage(
                    L"-d DummyBrokenDistro",
                    L"Failed to attach disk 'C:\\DoesNotExit\\ext4.vhdx' to WSL2: The system cannot find the path "
                    L"specified. ",
                    L"Wsl/Service/CreateInstance/MountDisk/HCS/ERROR_PATH_NOT_FOUND");

                // Purposefully set an incorrect value type to validate registry error handling.
                wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"Version", L"Broken");

                const auto tokenInfo = wil::get_token_information<TOKEN_USER>();
                const auto Sid = std::wstring(wsl::windows::common::wslutil::SidToString(tokenInfo->User.Sid).get());

                //  N.B. casing is ignored because the 'Software' key is sometimes uppercase, sometimes not.
                ValidateErrorMessage(
                    L"-d DummyBrokenDistro",
                    L"An error occurred accessing the registry. Path: '\\REGISTRY\\USER\\" + Sid +
                        L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\{baa405ef-1822-4bbe-84e2-30e4c6330d42}"
                        L"\\Version'."
                        L" "
                        L"Error: Data of this type is not supported. ",
                    L"Wsl/Service/ReadDistroConfig/ERROR_UNSUPPORTED_TYPE",
                    {},
                    L"wsl.exe",
                    true);
            }

            ValidateErrorMessage(
                L"echo ok",
                std::format(L"Invalid mac address 'foo' for key 'wsl2.macAddress' in {}:2", wslConfigPath),
                L"Wsl/Service/CreateInstance/CreateVm/ParseConfig/E_INVALIDARG",
                L"macAddress=foo");
        }
        else
        {
            // wsl.exe --manage --resize requires WSL2.
            ValidateErrorMessage(
                L"--manage test_distro --resize 10GB",
                L"This operation is only supported by WSL2.",
                L"Wsl/Service/WSL_E_WSL2_NEEDED");
        }

        ValidateErrorMessage(
            L"--import a b c", GetSystemErrorString(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)), L"Wsl/ERROR_FILE_NOT_FOUND");

        ValidateErrorMessage(
            L"-d DoesNotExist echo foo",
            L"There is no distribution with the supplied name.",
            L"Wsl/Service/WSL_E_DISTRO_NOT_FOUND");

        ValidateErrorMessage(
            L"--export DoesNotExist FileName",
            L"There is no distribution with the supplied name.",
            L"Wsl/Service/WSL_E_DISTRO_NOT_FOUND");

        ValidateErrorMessage(
            L"--import-in-place DoesNotExist FileName",
            GetSystemErrorString(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)),
            L"Wsl/ERROR_FILE_NOT_FOUND");

        ValidateErrorMessage(
            L"--set-default-version 3",
            GetSystemErrorString(HRESULT_FROM_WIN32(ERROR_VERSION_PARSE_ERROR)),
            L"Wsl/ERROR_VERSION_PARSE_ERROR");

        ValidateErrorMessage(
            L"--manage DoesNotExist --resize 10GB",
            L"There is no distribution with the supplied name.",
            L"Wsl/Service/WSL_E_DISTRO_NOT_FOUND");

        ValidateErrorMessage(L"--manage test_distro --resize foo", L"Invalid size: foo", L"Wsl/E_INVALIDARG");

        ValidateErrorMessage(
            L"--install --distribution debian --no-distribution",
            L"Arguments --no-distribution and --distribution can't be specified at same time.",
            L"Wsl/E_INVALIDARG");

        ValidateErrorMessage(
            L"--install debian --from-file foo --distribution foo",
            L"Arguments --from-file and --distribution can't be specified at same time.",
            L"Wsl/E_INVALIDARG");

        ValidateErrorMessage(
            L"--install foo --fixed-vhd", L"Argument --fixed-vhd requires the --vhd-size argument.", L"Wsl/E_INVALIDARG");

        {
            UniqueWebServer server(c_testDistributionEndpoint, c_testDistributionJson);
            RegistryKeyChange<std::wstring> keyChange(
                HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, wsl::windows::common::distribution::c_distroUrlRegistryValue, c_testDistributionEndpoint);
            ValidateErrorMessage(
                L"--install -d DoesNotExist",
                L"Invalid distribution name: 'DoesNotExist'.\r\nTo get a list of valid distributions, use 'wsl.exe --list "
                L"--online'.",
                L"Wsl/InstallDistro/WSL_E_DISTRO_NOT_FOUND");
        }

        {
            const auto lxssKey = wsl::windows::common::registry::OpenLxssMachineKey(KEY_READ | KEY_SET_VALUE);
            std::optional<std::wstring> revertValue;

            try
            {
                revertValue = wsl::windows::common::registry::ReadString(
                    lxssKey.get(), nullptr, wsl::windows::common::distribution::c_distroUrlRegistryValue);
            }
            catch (...)
            {
                // Expected if the value isn't set
            }

            auto revert = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                if (revertValue.has_value())
                {
                    wsl::windows::common::registry::WriteString(
                        lxssKey.get(), nullptr, wsl::windows::common::distribution::c_distroUrlRegistryValue, revertValue->c_str());
                }
                else
                {
                    wsl::windows::common::registry::DeleteValue(lxssKey.get(), wsl::windows::common::distribution::c_distroUrlRegistryValue);
                }
            });

            wsl::windows::common::registry::WriteString(
                lxssKey.get(), nullptr, wsl::windows::common::distribution::c_distroUrlRegistryValue, L"http://127.0.0.1:6666");

            ValidateErrorMessage(
                L"--install -d ubuntu",
                L"Failed to fetch the list distribution from 'http://127.0.0.1:6666'. " +
                    GetSystemErrorString(HRESULT_FROM_WIN32(WININET_E_CANNOT_CONNECT)),
                L"Wsl/InstallDistro/WININET_E_CANNOT_CONNECT");

            ValidateErrorMessage(
                L"--list --online",
                L"Failed to fetch the list distribution from 'http://127.0.0.1:6666'. " +
                    GetSystemErrorString(HRESULT_FROM_WIN32(WININET_E_CANNOT_CONNECT)),
                L"Wsl/WININET_E_CANNOT_CONNECT");
        }

        ValidateErrorMessage(
            L"/u foo",
            L"There is no distribution with the supplied name.",
            L"WslConfig/Service/WSL_E_DISTRO_NOT_FOUND",
            {},
            L"wslconfig.exe");

        ValidateErrorMessage(
            L"e7bef681-c148-4687-8a0f-8c8be93bac93", // GUID for a distro that's not installed.
            L"There is no distribution with the supplied name.",
            L"Bash/Service/CreateInstance/ReadDistroConfig/WSL_E_DISTRO_NOT_FOUND",
            {},
            L"bash.exe");

        VerifyOutput(L"--install --no-distribution", L"The operation completed successfully. \r\n");

        {
            std::wstring expectedUsageMessage;
            for (auto e : wsl::shared::Localization::MessageWslUsage())
            {
                if (e == L'\n')
                {
                    expectedUsageMessage += L'\r';
                }

                expectedUsageMessage += e;
            }

            VerifyOutput(L"--manage --move .", expectedUsageMessage + L"\r\n", -1);
        }
    }

    TEST_METHOD(CommandLineParsing)
    {
        VerifyOutput(L"echo -n \\\"", L"\"");
        VerifyOutput(L"echo -n \\\'", L"\'");
        VerifyOutput(L"echo -n \" \"", L" ");
        VerifyOutput(L"echo -n $USER", L"root");
        VerifyOutput(L"echo -n \"$USER\"", L"root");
        VerifyOutput(L"echo -n '\"$USER\"'", L"\"$USER\"");
        VerifyOutput(L"echo -n '\\\"$USER\\\"'", L"\\\"$USER\\\"");
        VerifyOutput(L"echo -n '$USER'", L"$USER");
        VerifyOutput(L"echo -n a \" \" b", L"a   b");
        VerifyOutput(L"echo -n a \"\" b", L"a  b");
        VerifyOutput(L"echo -n a b \"\"", L"a b ");
        VerifyOutput(L"echo -n \"a\"\"b\"", L"ab");

        VerifyOutput(L"--exec echo -n \"a\"", L"a");
        VerifyOutput(L"--exec echo -n $USER", L"$USER");
        VerifyOutput(L"--exec echo -n \\\"a\\\"", L"\"a\"");
        VerifyOutput(L"--exec echo -n \\\"a\\\"", L"\"a\"");
        VerifyOutput(L"--exec echo -n \"a\"\"b\"", L"a\"b");
        VerifyOutput(L"--exec echo -n \\\"", L"\"");
    }

    // This test validates that the help messages for wsl.exe and wsl.config are correctly displayed.
    // Notes:
    // - This test will fail if the help messages are changed. If that's the case, simply update the below strings
    // - This test assumes that English is the configured language.
    TEST_METHOD(UsageMessages)
    {
        const std::wstring WslHelpMessage =
            LR"""(Copyright (c) Microsoft Corporation. All rights reserved.
For privacy information about this product please visit https://aka.ms/privacy.

Usage: wsl.exe [Argument] [Options...] [CommandLine]

Arguments for running Linux binaries:

    If no command line is provided, wsl.exe launches the default shell.

    --exec, -e <CommandLine>
        Execute the specified command without using the default Linux shell.

    --shell-type <standard|login|none>
        Execute the specified command with the provided shell type.

    --
        Pass the remaining command line as-is.

Options:
    --cd <Directory>
        Sets the specified directory as the current working directory.
        If ~ is used the Linux user's home path will be used. If the path begins
        with a / character, it will be interpreted as an absolute Linux path.
        Otherwise, the value must be an absolute Windows path.

    --distribution, -d <DistroName>
        Run the specified distribution.

    --distribution-id <DistroGuid>
        Run the specified distribution ID.

    --user, -u <UserName>
        Run as the specified user.

    --system
        Launches a shell for the system distribution.

Arguments for managing Windows Subsystem for Linux:

    --help
        Display usage information.

    --debug-shell
        Open a WSL2 debug shell for diagnostics purposes.

    --install [Distro] [Options...]
        Install a Windows Subsystem for Linux distribution.
        For a list of valid distributions, use 'wsl.exe --list --online'.

        Options:
            --enable-wsl1
                Enable WSL1 support.

            --fixed-vhd
                Create a fixed-size disk to store the distribution.

            --from-file <Path>
                Install a distribution from a local file.

            --legacy
                Use the legacy distribution manifest.

            --location <Location>
                Set the install path for the distribution.

            --name <Name>
                Set the name of the distribution.

            --no-distribution
                Only install the required optional components, does not install a distribution.

            --no-launch, -n
                Do not launch the distribution after install.

            --version <Version>
                Specifies the version to use for the new distribution.

            --vhd-size <MemoryString>
                Specifies the size of the disk to store the distribution.

            --web-download
                Download the distribution from the internet instead of the Microsoft Store.

    --manage <Distro> <Options...>
        Changes distro specific options.

        Options:
            --move <Location>
                Move the distribution to a new location.

            --set-sparse, -s <true|false>
                Set the vhdx of distro to be sparse, allowing disk space to be automatically reclaimed.

            --set-default-user <Username>
                Set the default user of the distribution.

            --resize <MemoryString>
                Resize the disk of the distribution to the specified size.

    --mount <Disk>
        Attaches and mounts a physical or virtual disk in all WSL 2 distributions.

        Options:
            --vhd
                Specifies that <Disk> refers to a virtual hard disk.

            --bare
                Attach the disk to WSL2, but don't mount it.

            --name <Name>
                Mount the disk using a custom name for the mountpoint.

            --type <Type>
                Filesystem to use when mounting a disk, if not specified defaults to ext4.

            --options <Options>
                Additional mount options.

            --partition <Index>
                Index of the partition to mount, if not specified defaults to the whole disk.

    --set-default-version <Version>
        Changes the default install version for new distributions.

    --shutdown
        Immediately terminates all running distributions and the WSL 2
        lightweight utility virtual machine.

        Options:
            --force
                Terminate the WSL 2 virtual machine even if an operation is in progress. Can cause data loss.

    --status
        Show the status of Windows Subsystem for Linux.

    --unmount [Disk]
        Unmounts and detaches a disk from all WSL2 distributions.
        Unmounts and detaches all disks if called without argument.

    --uninstall
        Uninstalls the Windows Subsystem for Linux package from this machine.

    --update
        Update the Windows Subsystem for Linux package.

        Options:
            --pre-release
                Download a pre-release version if available.

    --version, -v
        Display version information.

Arguments for managing distributions in Windows Subsystem for Linux:

    --export <Distro> <FileName> [Options]
        Exports the distribution to a tar file.
        The filename can be - for stdout.

        Options:
            --format <Format>
                Specifies the export format. Supported values: tar, tar.gz, tar.xz, vhd.

    --import <Distro> <InstallLocation> <FileName> [Options]
        Imports the specified tar file as a new distribution.
        The filename can be - for stdin.

        Options:
            --version <Version>
                Specifies the version to use for the new distribution.

            --vhd
                Specifies that the provided file is a .vhdx file, not a tar file.
                This operation makes a copy of the .vhdx file at the specified install location.

    --import-in-place <Distro> <FileName>
        Imports the specified .vhdx file as a new distribution.
        This virtual hard disk must be formatted with the ext4 filesystem type.

    --list, -l [Options]
        Lists distributions.

        Options:
            --all
                List all distributions, including distributions that are
                currently being installed or uninstalled.

            --running
                List only distributions that are currently running.

            --quiet, -q
                Only show distribution names.

            --verbose, -v
                Show detailed information about all distributions.

            --online, -o
                Displays a list of available distributions for install with 'wsl.exe --install'.

    --set-default, -s <Distro>
        Sets the distribution as the default.

    --set-version <Distro> <Version>
        Changes the version of the specified distribution.

    --terminate, -t <Distro>
        Terminates the specified distribution.

    --unregister <Distro>
        Unregisters the distribution and deletes the root filesystem.
)""";

        const std::wstring WslConfigHelpMessage =
            LR"""(Performs administrative operations on Windows Subsystem for Linux

Usage:
    /l, /list [Option]
        Lists registered distributions.
        /all - Optionally list all distributions, including distributions that
               are currently being installed or uninstalled.

        /running - List only distributions that are currently running.

    /s, /setdefault <DistributionName>
        Sets the distribution as the default.

    /t, /terminate <DistributionName>
        Terminates the distribution.

    /u, /unregister <DistributionName>
        Unregisters the distribution and deletes the root filesystem.
)""";

        const std::wstring WslInstallHelpMessage =
            LR"""(Invalid distribution name: 'foo'.
To get a list of valid distributions, use 'wsl.exe --list --online'.
Error code: Wsl/InstallDistro/WSL_E_DISTRO_NOT_FOUND
)""";

        auto AddCrlf = [](const std::wstring& Input) {
            std::wstring MessageWithCrlf;

            for (const auto e : Input)
            {
                if (e == '\n')
                {
                    MessageWithCrlf += '\r';
                }
                MessageWithCrlf += e;
            }

            return MessageWithCrlf;
        };

        // Note: There is no easy way to validate wslg's help message, since it displays a blocking
        // message box before exiting.

        VerifyOutput(L"--help", AddCrlf(WslHelpMessage), -1);
        VerifyOutput(L"--help", AddCrlf(WslConfigHelpMessage), -1, L"wslconfig.exe");

        UniqueWebServer server(c_testDistributionEndpoint, c_testDistributionJson);
        RegistryKeyChange<std::wstring> keyChange(
            HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, wsl::windows::common::distribution::c_distroUrlRegistryValue, c_testDistributionEndpoint);

        VerifyOutput(L"--install foo", AddCrlf(WslInstallHelpMessage), -1);
    }

    TEST_METHOD(TestExistingSwapVhd)
    {
        WSL2_TEST_ONLY();

        // Create a 100MB swap vhdx.
        auto swapVhd = wil::GetCurrentDirectoryW<std::wstring>() + L"\\TestSwap.vhdx";

        VIRTUAL_STORAGE_TYPE storageType{};
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

        CREATE_VIRTUAL_DISK_PARAMETERS createVhdParameters{};
        createVhdParameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        createVhdParameters.Version2.BlockSizeInBytes = 1024 * 1024;
        createVhdParameters.Version2.MaximumSize = 100 * 1024 * 1024;

        wil::unique_hfile vhd{};
        VERIFY_ARE_EQUAL(
            ::CreateVirtualDisk(
                &storageType, swapVhd.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES, 0, &createVhdParameters, nullptr, &vhd),
            0l);

        vhd.reset();

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            WslShutdown();
            DeleteFile(swapVhd.c_str());
        });

        // Update .wslconfig. Update the swapVhd path to replace single backslash
        // with double backslashes so as to be compatible with .wslconfig parsing.
        // The following regex replacement only works as intended if the path contains
        // single backslashes. Negative lookahead can be used to handle paths with double
        // backslashes but then the negative lookbehind case should also be used but the
        // latter is not supported in std::regex.
        swapVhd = std::regex_replace(swapVhd, std::wregex(L"\\\\"), L"\\\\");
        WslConfigChange configChange(LxssGenerateTestConfig() + L"\nswap=256MB\nswapFile=" + swapVhd);

        auto validateSwapSize = [](LPCWSTR Expected) {
            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"swapon | awk 'END {print $3}'");

            VERIFY_ARE_EQUAL(Expected + std::wstring(L"\n"), output);
        };

        validateSwapSize(L"256M");

        // Validate that the vhdx is resized correctly if the swap size changes
        configChange.Update(LxssGenerateTestConfig() + L"\nswap=200MB\nswapFile=" + swapVhd);
        validateSwapSize(L"200M");
    }

    TEST_METHOD(InitDoesntBlockSignals)
    {
        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"grep -iF SigBlk < /proc/1/status");
        VERIFY_ARE_EQUAL(L"SigBlk:\t0000000000000000\n", output);
    }

    TEST_METHOD(InitReadonly)
    {
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L" grep '^rootfs /init rootfs ro,' /proc/self/mounts", nullptr, nullptr, nullptr, nullptr), 0u);
    }

    TEST_METHOD(GpuMounts)
    {
        WSL2_TEST_ONLY();

        auto ValidateGpuMounts = [](HANDLE Token) {
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(
                    L"mount | grep -iF 'none on /usr/lib/wsl/lib type overlay (rw,nosuid,nodev,noatime,lowerdir=/gpu_" TEXT(LXSS_GPU_PACKAGED_LIB_SHARE) L":/gpu_" TEXT(
                        LXSS_GPU_INBOX_LIB_SHARE) L",upperdir=/gpu_lib/rw/upper,workdir=/gpu_lib/rw/work,uuid=on)'",
                    nullptr,
                    nullptr,
                    nullptr,
                    Token),
                0u);

            // Ensure the lib directory is writable.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L" touch /usr/lib/wsl/lib/foo && rm /usr/lib/wsl/lib/foo", nullptr, nullptr, nullptr, Token), 0u);

            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(
                    L"mount | grep -iF '" TEXT(
                        LXSS_GPU_DRIVERS_SHARE) L" on /usr/lib/wsl/drivers type 9p (ro,nosuid,nodev,noatime,aname=" TEXT(LXSS_GPU_DRIVERS_SHARE) L";fmask=222;dmask=222,cache=5,access=client,msize=65536,trans=fd,rfd=8,wfd=8)'",
                    nullptr,
                    nullptr,
                    nullptr,
                    Token),
                0u);
        };

        auto cleanUp = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslShutdown(); });

        // Validate that GPU mounts are present in both namespaces.
        const auto nonElevatedToken = GetNonElevatedToken();
        WslShutdown();
        ValidateGpuMounts(nullptr);
        ValidateGpuMounts(nonElevatedToken.get());

        // Create a new instance with a non-elevated token as the creator.
        WslShutdown();
        ValidateGpuMounts(nonElevatedToken.get());
        ValidateGpuMounts(nullptr);
    }

    TEST_METHOD(InteropCornerCases)
    {
        auto validateInterop = [](const std::wstring& binaryName) {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LxsstuLaunchWsl(L"rm /tmp/'" + binaryName + L"'"); });

            // The "|| echo fail" part is needed because bash will exec instead of forking() of only one non-builtin command is passed.
            // If bash exec's then this test is useless since the binfmt interpreter would not be a child of a process with a weird name.

            const std::wstring commandLine =
                L"cp /bin/bash /tmp/'" + binaryName + L"' && '/tmp/" + binaryName +
                L"' -c 'export WSL_INTEROP=\"\" && echo -n $WSL_INTEROP && cmd.exe /c \"echo ok\" || echo fail'";
            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(commandLine);

            VERIFY_ARE_EQUAL(output, L"ok\r\n");
        };

        validateInterop(L"bash with spaces");
        validateInterop(L"bash )");
        validateInterop(L"bash (");
        validateInterop(L"(bash)");
        validateInterop(L"(bash(");
        validateInterop(L"()");
        validateInterop(L"(");
        validateInterop(L")");
    }

    TEST_METHOD(InteropPid1)
    {
        // Validate that interop works as pid 1.
        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"unshare -pf --wd $(dirname $(which cmd.exe)) cmd.exe /c echo ok");
        VERIFY_ARE_EQUAL(output, L"ok\r\n");
    }

    TEST_METHOD(Hostname)
    {
        auto cleanup = wil::scope_exit([] {
            LxsstuLaunchWsl(LXSST_REMOVE_DISTRO_CONF_COMMAND_LINE);

            TerminateDistribution();
        });

        auto validate = [](const std::string& input, const std::wstring& expectedOutput) {
            LxssWriteWslDistroConfig("[network]\nhostname=" + input);
            TerminateDistribution();

            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"hostname");
            VERIFY_ARE_EQUAL(output, expectedOutput + L"\n");

            output = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/hostname").first;
            VERIFY_ARE_EQUAL(output, expectedOutput + L"\n");
        };

        validate("SimpleHostname", L"SimpleHostname");
        validate("Simple-Hostname", L"Simple-Hostname");
        validate("Simple_Hostname", L"SimpleHostname");
        validate("-hostname", L"hostname");
        validate("--hostname", L"hostname");
        validate("hostname.-", L"hostname");
        validate(".hostname", L"hostname");
        validate("hostname.", L"hostname");
        validate("host.name.", L"host.name");
        validate("host..name", L"host.name");
        validate("host|name", L"hostname");
        validate(".a-", L"a");
        validate(".a-b", L"a-b");
        validate(".", L"localhost");
        validate("-", L"localhost");
        validate("-.-", L"localhost");
        // Validate hostname is limited to 64 characters.
        const std::string longHostName("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
        validate(longHostName, wsl::shared::string::MultiByteToWide(longHostName.substr(0, 64)));
    }

    TEST_METHOD(WslConfWarnings)
    {
        WSL2_TEST_ONLY();

        DistroFileChange configChange(L"/etc/wsl.conf", false);

        auto validateWarnings = [&configChange](const std::wstring& config, const std::wstring& expectedWarnings) {
            configChange.SetContent(config.c_str());

            TerminateDistribution();

            // This loop is here because of a race condition when starting WSL to get the warnings.
            // If a p9rdr distribution startup notification arrives just before wsl.exe calls CreateInstance(),
            // the warnings will be 'consummed' before wsl.exe can read them.
            // To work around that, loop for up to 2 minutes while we don't get any warnings

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

            while (std::chrono::steady_clock::now() < deadline)
            {
                auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"-u root echo ok");
                VERIFY_ARE_EQUAL(L"ok\n", output);

                if (!warnings.empty() || expectedWarnings.empty())
                {
                    VERIFY_ARE_EQUAL(expectedWarnings, warnings);
                    return;
                }

                LogInfo("Received empty warnings, trying again");
                WslShutdown();
            }

            LogError("Timed out waiting for warnings. Expected warnings: %ls", expectedWarnings.c_str());
            VERIFY_FAIL();
        };

        validateWarnings(L"[foo]\na=b", L"wsl: Unknown key 'foo.a' in /etc/wsl.conf:2\r\n");
        validateWarnings(L"a=a\\m", L"wsl: Invalid escaped character: 'm' in /etc/wsl.conf:1\r\n");
        validateWarnings(L"[=b", L"wsl: Invalid section name in /etc/wsl.conf:1\r\n");
        validateWarnings(L"\r\n\r\n[foo]\r\na=b", L"wsl: Unknown key 'foo.a' in /etc/wsl.conf:5\r\n");

        // Validate that CRLF is correctly handled
        {
            configChange.SetContent(L"[network]\r\nhostname=foo\r\n");
            TerminateDistribution();

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"hostname");
            VERIFY_ARE_EQUAL(out, L"foo\n");
            VERIFY_ARE_EQUAL(err, L"");
        }
    }

    TEST_METHOD(Warnings)
    {
        WSL2_TEST_ONLY();

        WslConfigChange configChange(LxssGenerateTestConfig());

        auto validateWarnings = [&configChange](
                                    const std::wstring& config,
                                    const std::wstring& expectedWarnings,
                                    const std::wstring& prefix = LxssGenerateTestConfig(),
                                    bool fnmatch = false) {
            WEX::Logging::Log::Comment(config.c_str());
            WEX::Logging::Log::Comment(expectedWarnings.c_str());
            configChange.Update(prefix + config);

            // This loop is here because of a race condition when starting WSL to get the warnings.
            // If a p9rdr distribution startup notification arrives just before wsl.exe calls CreateInstance(),
            // the warnings will be 'consummed' before wsl.exe can read them.
            // To work around that, loop for up to 2 minutes while we don't get any warnings

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

            while (std::chrono::steady_clock::now() < deadline)
            {
                auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
                VERIFY_ARE_EQUAL(L"ok\n", output);

                if (!warnings.empty() || expectedWarnings.empty())
                {
                    if (fnmatch)
                    {
                        if (!PathMatchSpec(warnings.c_str(), expectedWarnings.c_str()))
                        {
                            LogError("Warning '%ls' didn't match pattern '%ls'", warnings.c_str(), expectedWarnings.c_str());
                            VERIFY_FAIL();
                        }
                    }
                    else
                    {
                        VERIFY_ARE_EQUAL(expectedWarnings, warnings);
                    }
                    return;
                }

                LogInfo("Received empty warnings, trying again");
                WslShutdown();
            }

            LogError("Timed out waiting for warnings. Expected warnings: %ls", expectedWarnings.c_str());
            VERIFY_FAIL();
        };

        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();

        validateWarnings(L"a=b", std::format(L"wsl: Unknown key 'wsl2.a' in {}:21\r\n", wslConfigPath));
        validateWarnings(L"[=b", std::format(L"wsl: Invalid section name in {}:21\r\n", wslConfigPath));

        validateWarnings(
            L"dhcpTimeout=NotANumber",
            std::format(L"wsl: Invalid integer value 'NotANumber' for key 'wsl2.dhcpTimeout' in {}:21\r\n", wslConfigPath));

        validateWarnings(L"ipv6=NotABoolean", std::format(L"wsl: Invalid boolean value 'NotABoolean' for key 'wsl2.ipv6' in {}:21\r\n", wslConfigPath));

        validateWarnings(L"[sectionNotComplete", std::format(L"wsl: Expected ']' in {}:21\r\n", wslConfigPath));
        validateWarnings(L"NoEqual", std::format(L"wsl: Expected '=' in {}:21\r\n", wslConfigPath));
        validateWarnings(
            L"networkingMode=InvalidMode",
            std::format(L"wsl: Invalid value 'InvalidMode' for config key 'wsl2.networkingMode' in {}:2 (Valid values: Bridged, Mirrored, Nat, None, VirtioProxy)\r\n", wslConfigPath),
            L"[wsl2]\n");
        validateWarnings(
            L"networkingMode=a\\m", std::format(L"wsl: Invalid escaped character: 'm' in {}:2\r\n", wslConfigPath), L"[wsl2]\n");

        validateWarnings(
            L"\nswap=200MB\nswapFile=C:\\\\DoesNotExist\\\\swap.vhdx",
            L"wsl: Failed to create the swap disk in 'C:\\DoesNotExist\\swap.vhdx': The system cannot find the path "
            L"specified. \r\n");

        validateWarnings(L"\nswap=/", std::format(L"wsl: Invalid memory string '/' for .wslconfig entry 'wsl2.swap' in {}:22\r\n", wslConfigPath));
        validateWarnings(L"\nswap=0GB", L"");
        validateWarnings(L"\nswap=0foo", std::format(L"wsl: Invalid memory string '0foo' for .wslconfig entry 'wsl2.swap' in {}:22\r\n", wslConfigPath));
        validateWarnings(L"safeMode=true", L"wsl: SAFE MODE ENABLED - many features will be disabled\r\n", L"[wsl2]\n");
        validateWarnings(L"processors=", std::format(L"wsl: Invalid integer value '' for key 'wsl2.processors' in {}:21\r\n", wslConfigPath));
        validateWarnings(L"memory=", std::format(L"wsl: Invalid memory string '' for .wslconfig entry 'wsl2.memory' in {}:21\r\n", wslConfigPath));
        validateWarnings(L"debugConsole=", std::format(L"wsl: Invalid boolean value '' for key 'wsl2.debugConsole' in {}:21\r\n", wslConfigPath));
        validateWarnings(
            L"networkingMode=",
            std::format(L"wsl: Invalid value '' for config key 'wsl2.networkingMode' in {}:21 (Valid values: Bridged, Mirrored, Nat, None, VirtioProxy)\r\n", wslConfigPath));

        validateWarnings(
            L"ipv6=true\nipv6=false",
            std::format(L"wsl: Duplicated config key 'wsl2.ipv6' in {}:22 (Conflicting key: 'wsl2.ipv6' in {}:21)\r\n", wslConfigPath, wslConfigPath));

        validateWarnings(
            L"networkingMode=NAT\n[experimental]\nnetworkingMode=Mirrored",
            std::format(L"wsl: Duplicated config key 'experimental.networkingMode' in {}:4 (Conflicting key: 'wsl2.networkingMode' in {}:2)\r\n", wslConfigPath, wslConfigPath),
            L"[wsl2]\n");

        validateWarnings(
            L"networkingMode=bridged",
            L"wsl: Bridged networking requires wsl2.vmSwitch to be set.\r\n"
            L"Error code: CreateInstance/CreateVm/ConfigureNetworking/WSL_E_VMSWITCH_NOT_SET\r\n"
            L"wsl: Failed to configure network (networkingMode Bridged), falling back to networkingMode None.\r\n",
            L"[wsl2]\n");

        validateWarnings(
            L"networkingMode=bridged\nvmSwitch=DoesNotExist",
            L"wsl: The VmSwitch 'DoesNotExist' was not found. Available switches:*\r\n"
            L"Error code: CreateInstance/CreateVm/ConfigureNetworking/WSL_E_VMSWITCH_NOT_FOUND\r\n"
            L"wsl: Failed to configure network (networkingMode Bridged), falling back to networkingMode None.\r\n",
            L"[wsl2]\n",
            true);

        if (!AreExperimentalNetworkingFeaturesSupported())
        {
            validateWarnings(
                L"[experimental]\nnetworkingMode=mirrored",
                L"wsl: Experimental networking features are not supported, falling back to default settings\r\n",
                L"[wsl2]\n");

            validateWarnings(
                L"[experimental]\ndnsTunneling=true",
                L"wsl: Experimental networking features are not supported, falling back to default settings\r\n",
                L"[wsl2]\n");

            validateWarnings(
                L"[experimental]\nfirewall=true",
                L"wsl: Experimental networking features are not supported, falling back to default settings\r\n",
                L"[wsl2]\n");
        }
        else
        {
            if (TryLoadDnsResolverMethods())
            {
                // Verify DNS tunneling settings are parsed correctly
                validateWarnings(L"[experimental]\ndnsTunneling=true\nbestEffortDnsParsing=true", L"");
                validateWarnings(L"[experimental]\ndnsTunneling=true\ndnsTunnelingIpAddress=10.255.255.1", L"");

                validateWarnings(
                    L"[experimental]\ndnsTunneling=true\ndnsTunnelingIpAddress=1.2.3",
                    std::format(L"wsl: Invalid IP value '1.2.3' for key 'experimental.dnsTunnelingIpAddress' in {}:23\r\n", wslConfigPath));
            }
        }

        validateWarnings(
            L"[experimental]\nignoredPorts=NotANumber",
            std::format(L"wsl: Invalid integer value 'NotANumber' for key 'experimental.ignoredPorts' in {}:22\r\n", wslConfigPath));

        validateWarnings(
            L"[experimental]\nignoredPorts=65536",
            std::format(L"wsl: Invalid integer value '65536' for key 'experimental.ignoredPorts' in {}:22\r\n", wslConfigPath));

        // Verify that the vhdSize setting is parsed correctly.
        validateWarnings(L"[wsl2]\ndefaultVhdSize=64GB\n", L"");

        auto maxProcessorCount = wsl::windows::common::wslutil::GetLogicalProcessorCount();
        validateWarnings(
            std::format(L"processors={}", maxProcessorCount + 1).c_str(),
            std::format(L"wsl: wsl2.processors cannot exceed the number of logical processors on the system ({} > {})\r\n", maxProcessorCount + 1, maxProcessorCount));

        // Exclusively open .wslconfig to make it unreadable
        const wil::unique_handle wslConfig{
            CreateFile(wslConfigPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_NOT_NULL(wslConfig);

        WslShutdown();
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
        VERIFY_ARE_EQUAL(L"ok\n", output);

        VERIFY_ARE_EQUAL(
            std::format(L"wsl: Failed to open config file {}, The process cannot access the file because it is being used by another process. \r\n", wslConfigPath),
            warnings);

        {
            DistroFileChange fstab(L"/etc/fstab");
            fstab.SetContent(L"invalid fs tab content");
            TerminateDistribution();

            std::tie(output, warnings) = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
            VERIFY_ARE_EQUAL(L"ok\n", output);
            VERIFY_ARE_EQUAL(L"wsl: Processing /etc/fstab with mount -a failed.\n", warnings);
        }

        // Validate that WSL_DISABLE_WARNINGS silence the stderr output
        ScopedEnvVariable disableWarnings(L"WSL_DISABLE_WARNINGS", L"1");
        WslShutdown();

        std::tie(output, warnings) = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
        VERIFY_ARE_EQUAL(L"ok\n", output);
        VERIFY_ARE_EQUAL(L"", warnings);
    }

    TEST_METHOD(Processors)
    {
        WSL2_TEST_ONLY();

        WslConfigChange configChange(LxssGenerateTestConfig() + L"\nprocessors=1");

        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"nproc --all");
        VERIFY_ARE_EQUAL(L"1\n", output);
        VERIFY_ARE_EQUAL(L"", warnings);
    }

    TEST_METHOD(GuiApplications)
    {
        WSL2_TEST_ONLY();

        auto validateEnvironment = [&](bool systemdEnabled) {
            WslConfigChange configChange(LxssGenerateTestConfig({.guiApplications = true}));

            // Validate that running the system distro works.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system true"), 0L);

            // Validate that $DISPLAY and $WAYLAND_DISPLAY are set
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"env | grep DISPLAY="), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"env | grep WAYLAND_DISPLAY="), 0L);

            // Validate the X11 socket is in the expected location and that we can connect to it.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -d /tmp/.X11-unix"), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"socat - UNIX-CONNECT:/tmp/.X11-unix/X0 < /dev/null"), 0L);

            // Validate the runtime dir exists and the wayland-0 socket is in the expected location.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"env | grep XDG_RUNTIME_DIR="), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -d $XDG_RUNTIME_DIR"), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -S $XDG_RUNTIME_DIR/wayland-0"), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/wayland-0 < /dev/null"), 0L);

            // Validate that WSLg can be disabled.
            configChange.Update(LxssGenerateTestConfig({.guiApplications = false}));

            // Validate that WSL starts successfully
            auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
            VERIFY_ARE_EQUAL(L"ok\n", output);
            VERIFY_ARE_EQUAL(L"", warnings);

            // Validate that WSLg-related environment variables are not present.
            //
            // N.B. XDG_RUNTIME_DIR is set when systemd is enabled even if GUI apps are disabled.
            std::vector<std::wstring> variables = {L"$DISPLAY", L"$WAYLAND_DISPLAY"};
            if (!systemdEnabled)
            {
                variables.emplace_back(L"$XDG_RUNTIME_DIR");
            }

            for (const auto& variable : variables)
            {
                std::tie(output, warnings) = LxsstuLaunchWslAndCaptureOutput(L"echo -n " + variable);
                VERIFY_ARE_EQUAL(L"", output);
                VERIFY_ARE_EQUAL(L"", warnings);
            }

            // Validate that wsl --system does not start
            std::tie(output, warnings) = LxsstuLaunchWslAndCaptureOutput(L"--system echo not ok", -1);

            const std::wstring configPath = wsl::windows::common::helpers::GetWslConfigPath();
            const auto expectedOutput =
                L"GUI application support is disabled via " + configPath +
                L" or /etc/wsl.conf.\r\nError code: Wsl/Service/CreateInstance/WSL_E_GUI_APPLICATIONS_DISABLED\r\n";

            VERIFY_ARE_EQUAL(output, expectedOutput);
            VERIFY_ARE_EQUAL(L"", warnings);
        };

        LogInfo("Validate WSLg state with systemd disabled.");
        validateEnvironment(false);

        LogInfo("Validate WSLg state with systemd enabled.");
        auto revert = EnableSystemd();
        VERIFY_IS_TRUE(IsSystemdRunning(L"--system"));
        validateEnvironment(true);
    }

    TEST_METHOD(GuiApplicationsSystemd)
    {
        WSL2_TEST_ONLY();

        DistroFileChange wslConf(L"/etc/wsl.conf", false);
        wslConf.SetContent(L"[boot]\nsystemd=true\n");
        WslConfigChange config{LxssGenerateTestConfig({.guiApplications = true})};

        auto validateSocketExists = [](bool exists) {
            LxsstuLaunchWsl(L"ls -a /tmp/.X11-unix/");
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e /tmp/.X11-unix/X0"), exists ? 0L : 1L);
        };

        // Validate that wslg.service restores the socket if it's deleted.
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -f /run/systemd/generator/wslg.service"), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e /run/systemd/generator/default.target.wants/wslg.service"), 0L);

            validateSocketExists(true);

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"umount /tmp/.X11-unix"), 0L);

            validateSocketExists(false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"systemctl restart wslg.service"), 0L);
            validateSocketExists(true);
        }

        // Validate that the unit isn't create when GUI apps are disabled
        {
            config.Update(LxssGenerateTestConfig({.guiApplications = false}));
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e /run/systemd/generator/wslg.service"), 1L);
        }

        // Validate that the unit isn't create when GUI apps are disabled inside the distro.
        {
            wslConf.SetContent(L"[boot]\nsystemd=true\n[general]\nguiApplications=false");
            TerminateDistribution();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e /run/systemd/generator/wslg.service"), 1L);
        }
    }

    TEST_METHOD(RegistryKeys)
    {
        auto openKey = [&](LPCWSTR keyName) {
            LogInfo("OpenKey(HKEY_LOCAL_MACHINE, %ls, KEY_READ)", keyName);
            return wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, keyName, KEY_READ);
        };

        // Keys that are created by the optional component and the service.
        const std::vector<LPCWSTR> inboxKeys{
            L"SOFTWARE\\Classes\\CLSID\\{B2B4A4D1-2754-4140-A2EB-9A76D9D7CDC6}",
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\{B2B4A4D1-2754-4140-A2EB-"
            L"9A76D9D7CDC6}",
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\IdListAliasTranslations\\WSL",
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\IdListAliasTranslations\\WSLLegacy",
            L"SOFTWARE\\Classes\\Directory\\shell\\WSL",
            L"SOFTWARE\\Classes\\Directory\\Background\\shell\\WSL",
            L"SOFTWARE\\Classes\\Drive\\shell\\WSL"};

        for (const auto* keyName : inboxKeys)
        {
            auto key = openKey(keyName);
            VERIFY_IS_TRUE(!!key);
        }

        // Keys that are only created by the MSI.
        const std::vector<LPCWSTR> serviceKeys{
            L"SOFTWARE\\Microsoft\\Terminal Server Client\\Default\\OptionalAddIns\\WSLDVC_PACKAGE",
            L"SOFTWARE\\Classes\\CLSID\\{7e6ad219-d1b3-42d5-b8ee-d96324e64ff6}",
            L"SOFTWARE\\Classes\\AppID\\{7F82AD86-755B-4870-86B1-D2E68DFE8A49}"};

        for (const auto* keyName : serviceKeys)
        {
            auto key = openKey(keyName);
            VERIFY_IS_TRUE(!!key);
        }
    }

    TEST_METHOD(BinariesAreSigned)
    {
        if (!wsl::shared::OfficialBuild)
        {
            LogSkipped("Build is not signed, skipping test");
            return;
        }

        auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        size_t signedFiles = 0;

        for (const auto& e : std::filesystem::recursive_directory_iterator(installPath.value()))
        {
            if (wsl::windows::common::string::IsPathComponentEqual(e.path().extension().native(), L".dll") ||
                wsl::windows::common::string::IsPathComponentEqual(e.path().extension().native(), L".exe"))
            {
                LogInfo("Validating signature for: %ls", e.path().c_str());

                wsl::windows::common::wslutil::ValidateFileSignature(e.path().c_str());
                signedFiles++;
            }
        }

        // Sanity check
        VERIFY_ARE_NOT_EQUAL(signedFiles, 0);
    }

    TEST_METHOD(CorruptedVhd)
    {
        WSL2_TEST_ONLY();

        // Create a 100MB vhd without a filesystem.
        auto distroPath = std::filesystem::weakly_canonical(wil::GetCurrentDirectoryW<std::wstring>());
        auto vhdPath = distroPath / L"CorruptedTest.vhdx";

        VIRTUAL_STORAGE_TYPE storageType{};
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

        CREATE_VIRTUAL_DISK_PARAMETERS createVhdParameters{};
        createVhdParameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        createVhdParameters.Version2.BlockSizeInBytes = 1024 * 1024;
        createVhdParameters.Version2.MaximumSize = 100 * 1024 * 1024;

        wil::unique_hfile vhd{};
        VERIFY_ARE_EQUAL(
            ::CreateVirtualDisk(
                &storageType, vhdPath.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES, 0, &createVhdParameters, nullptr, &vhd),
            0l);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            vhd.reset();
            DeleteFileW(vhdPath.c_str());
        });

        auto validateOutput = [&](const std::wstring& command, const std::wstring& expectedOutput) {
            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(command.c_str(), -1);
            VERIFY_ARE_EQUAL(output, expectedOutput);
        };

        // Attempt to import a vhd with an open handle.
        validateOutput(
            std::format(L"--import-in-place test-distro-corrupted \"{}\"", vhdPath.wstring()),
            std::format(
                L"Failed to attach disk '\\\\?\\{}' to WSL2: The process cannot access the file because it is being used by "
                L"another process. \r\n"
                L"Error code: Wsl/Service/RegisterDistro/MountDisk/HCS/ERROR_SHARING_VIOLATION\r\n",
                vhdPath.wstring()));

        vhd.reset();

        // Create a broken distribution registration
        {
            const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
            const auto distroKey =
                wsl::windows::common::registry::CreateKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");

            auto revert = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                wsl::windows::common::registry::DeleteKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");
            });

            wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"BasePath", distroPath.c_str());
            wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"VhdFileName", L"CorruptedTest.vhdx");
            wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"DistributionName", L"BrokenDistro");
            wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"DefaultUid", 0);
            wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Version", LXSS_DISTRO_VERSION_2);
            wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"State", LxssDistributionStateInstalled);
            wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Flags", LXSS_DISTRO_FLAGS_VM_MODE);

            // Validate that starting the distribution fails with the correct error code.
            validateOutput(
                L"-d BrokenDistro echo ok",
                L"The distribution failed to start because its virtual disk is corrupted.\r\n"
                L"Error code: Wsl/Service/CreateInstance/WSL_E_DISK_CORRUPTED\r\n");

            // Validate that trying to export the distribution fails with the correct error code.
            validateOutput(
                L"--export BrokenDistro dummy.tar",
                L"The distribution failed to start because its virtual disk is corrupted.\r\n"
                L"Error code: Wsl/Service/WSL_E_DISK_CORRUPTED\r\n");

            // Shutdown WSL to force the disk to detach.
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--shutdown"), 0L);
        }

        // Import a corrupted vhd.
        validateOutput(
            std::format(L"--import-in-place test-distro-corrupted \"{}\"", vhdPath.wstring()),
            L"The distribution failed to start because its virtual disk is corrupted.\r\n"
            L"Error code: Wsl/Service/RegisterDistro/WSL_E_DISK_CORRUPTED\r\n");

        // Ensure the VHD can be deleted to make sure it was properly ejected from the VM.
        VERIFY_ARE_EQUAL(DeleteFileW(vhdPath.c_str()), TRUE);
    }

    static void ValidateDistributionShortcut(LPCWSTR DistroName, HANDLE ExpectedIcon)
    {
        auto distroKey = OpenDistributionKey(DistroName);
        auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
        auto shellLink = wil::CoCreateInstance<IShellLink>(CLSID_ShellLink);
        auto startMenu = wsl::windows::common::filesystem::GetKnownFolderPath(FOLDERID_StartMenu, KF_FLAG_CREATE);

        // Validate that the shortcut is actually in the start menu
        VERIFY_IS_TRUE(shortcutPath.find(startMenu) != std::string::npos);

        Microsoft::WRL::ComPtr<IPersistFile> storage;
        VERIFY_SUCCEEDED(shellLink->QueryInterface(IID_IPersistFile, &storage));

        VERIFY_SUCCEEDED(storage->Load(shortcutPath.c_str(), 0));

        std::wstring target(MAX_PATH, '\0');

        WIN32_FIND_DATA findData{};
        VERIFY_SUCCEEDED(shellLink->GetPath(target.data(), static_cast<int>(target.size()), &findData, SLGP_RAWPATH));
        target.resize(wcslen(target.c_str()));

        static auto wslExePath = wsl::windows::common::wslutil::GetMsiPackagePath().value() + L"wsl.exe";
        VERIFY_ARE_EQUAL(target, wslExePath);

        std::wstring arguments(MAX_PATH, '\0');
        VERIFY_SUCCEEDED(shellLink->GetArguments(arguments.data(), static_cast<int>(arguments.size())));
        arguments.resize(wcslen(arguments.c_str()));

        auto distroId = GetDistributionId(DistroName);
        VERIFY_IS_TRUE(distroId.has_value());

        VERIFY_ARE_EQUAL(
            std::format(L"{} {} {} {}", WSL_DISTRIBUTION_ID_ARG, wsl::shared::string::GuidToString<wchar_t>(distroId.value()), WSL_CHANGE_DIRECTORY_ARG, WSL_CWD_HOME),
            arguments);

        std::wstring iconLocation(MAX_PATH, '\0');
        int id{};
        THROW_IF_FAILED(shellLink->GetIconLocation(iconLocation.data(), static_cast<int>(iconLocation.size()), &id));
        iconLocation.resize(wcslen(iconLocation.c_str()));

        if (ExpectedIcon == nullptr)
        {
            VERIFY_ARE_EQUAL(iconLocation, wslExePath);
        }
        else
        {
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            // Validate that the icon is under the distribution folder.
            VERIFY_IS_TRUE(iconLocation.find(basePath) != std::string::npos);

            // Validate that the icon has the content we expect.
            wil::unique_handle distroIcon{CreateFile(iconLocation.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            VERIFY_ARE_EQUAL(GetFileSize(ExpectedIcon, nullptr), GetFileSize(distroIcon.get(), nullptr));
        }
    }

    static std::pair<nlohmann::json, std::wstring> ValidateDistributionTerminalProfile(const std::wstring& DistroName, bool defaultIcon)
    {
        using namespace wsl::windows::common::wslutil;
        using namespace wsl::windows::common::string;

        auto distroKey = OpenDistributionKey(DistroName.c_str());
        auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");

        auto distroId = GetDistributionId(DistroName.c_str());
        VERIFY_IS_TRUE(distroId.has_value());

        auto distroIdString = wsl::shared::string::GuidToString<wchar_t>(distroId.value());
        auto distributionProfileId =
            wsl::shared::string::GuidToString<wchar_t>(CreateV5Uuid(WslTerminalNamespace, std::as_bytes(std::span{distroIdString})));

        auto profilePath = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"Microsoft" / L"Windows Terminal" /
                           L"Fragments" / L"Microsoft.WSL" / (distributionProfileId + L".json");

        std::ifstream file{profilePath};
        VERIFY_IS_TRUE(file.good());

        nlohmann::json json;
        VERIFY_IS_TRUE((file >> json).good());

        VERIFY_IS_TRUE(json.is_object());

        auto profiles = json.find("profiles");
        VERIFY_ARE_NOT_EQUAL(profiles, json.end());
        VERIFY_IS_TRUE(profiles->is_array());

        VERIFY_IS_TRUE(profiles->size() >= 2);
        const auto profileHide = profiles->at(0);

        auto expectedHideGuid = wsl::shared::string::GuidToString<wchar_t>(
            CreateV5Uuid(GeneratedProfilesTerminalNamespace, std::as_bytes(std::span{DistroName})));
        VERIFY_ARE_EQUAL(profileHide["updates"], wsl::shared::string::WideToMultiByte(expectedHideGuid));
        VERIFY_ARE_EQUAL(profileHide["hidden"], true);

        const auto launchProfile = profiles->at(1);

        auto expectedId =
            wsl::shared::string::GuidToString<wchar_t>(CreateV5Uuid(WslTerminalNamespace, std::as_bytes(std::span{distroIdString})));
        VERIFY_ARE_EQUAL(launchProfile["guid"].get<std::string>(), wsl::shared::string::WideToMultiByte(expectedId));
        VERIFY_ARE_EQUAL(launchProfile["name"].get<std::string>(), wsl::shared::string::WideToMultiByte(DistroName));
        VERIFY_ARE_EQUAL(launchProfile["pathTranslationStyle"].get<std::string>(), "wsl");

        std::wstring systemDir;
        wil::GetSystemDirectoryW(systemDir);

        VERIFY_ARE_EQUAL(
            std::format("{}\\{} {} {} {} {}", systemDir, WSL_BINARY_NAME, WSL_DISTRIBUTION_ID_ARG, distroIdString, WSL_CHANGE_DIRECTORY_ARG, WSL_CWD_HOME),
            launchProfile["commandline"].get<std::string>());

        auto iconLocation = wsl::shared::string::MultiByteToWide(launchProfile["icon"].get<std::string>());
        if (defaultIcon)
        {
            static auto wslExePath = wsl::windows::common::wslutil::GetMsiPackagePath().value() + L"wsl.exe";
            VERIFY_ARE_EQUAL(iconLocation, wslExePath);
        }
        else
        {
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            // Validate that the icon is under the distribution folder.
            VERIFY_IS_TRUE(iconLocation.find(basePath) == 0);
        }

        return std::make_pair(json, profilePath);
    }

    TEST_METHOD(ConvertDistro)
    {
        std::wstring originalVersion;
        std::wstring targetVersion;
        if (LxsstuVmMode())
        {
            originalVersion = L"2";
            targetVersion = L"1";
        }
        else
        {
            originalVersion = L"1";
            targetVersion = L"2";
        }

        auto cleanup =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LxsstuLaunchWsl(L"--set-version test_distro " + originalVersion); });

        // Convert the test distribuiton to the target version and back to the original.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--set-version test_distro " + targetVersion), 0u);
        ValidateDistributionShortcut(LXSS_DISTRO_NAME_TEST_L, nullptr);
        ValidateDistributionTerminalProfile(LXSS_DISTRO_NAME_TEST_L, true);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--set-version test_distro " + originalVersion), 0u);
        ValidateDistributionShortcut(LXSS_DISTRO_NAME_TEST_L, nullptr);
        ValidateDistributionTerminalProfile(LXSS_DISTRO_NAME_TEST_L, true);

        // Do not convert the test distribution if it is already in the original version.
        cleanup.release();
    }

    TEST_METHOD(ManualDistroShutdown)
    {
        WSL2_TEST_ONLY();

        // Terminate a distribution from within WSL. This command should be terminated by the VM terminating
        LxsstuLaunchWsl(L"echo foo > /dev/shm/bar ; reboot -f ; sleep 1d");

        // Wait for distribution to be terminated to avoid running the next command as it shuts down
        auto pred = []() {
            const auto commandLine = LxssGenerateWslCommandLine(L"--list --running");
            wsl::windows::common::SubProcess process(nullptr, commandLine.c_str());

            // Don't check the exit code since that command returns -1 when no distros are running.
            const auto output = process.RunAndCaptureOutput();
            THROW_HR_IF(E_ABORT, output.Stdout.find(LXSS_DISTRO_NAME_TEST_L) != std::string::npos);
        };

        wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::seconds(1), std::chrono::minutes(2));

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"test -f /dev/shm/bar2  || echo -n ok");
        VERIFY_ARE_EQUAL(out, L"ok");
    }

    TEST_METHOD(KernelModules)
    {
        WSL2_TEST_ONLY();

        // Get the kernel version and stip off everything after the first dash.
        std::wstring kernelVersion{TEXT(KERNEL_VERSION)};
        auto position = kernelVersion.find_first_of(L"-");
        if (position != kernelVersion.npos)
        {
            kernelVersion = kernelVersion.substr(0, position);
        }

        kernelVersion += L"-microsoft-standard-WSL2";

        // Ensure the kernel modules folder is mounted correctly.
        std::wstring command = std::format(
            L"mount | grep -iF 'none on /usr/lib/modules/{} type overlay "
            L"(rw,nosuid,nodev,noatime,lowerdir=/modules,upperdir=/lib/modules/{}/rw/upper,workdir=/lib/modules/{}/rw/"
            L"work,uuid=on)'",
            kernelVersion,
            kernelVersion,
            kernelVersion);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(command.c_str(), nullptr, nullptr, nullptr, nullptr), 0u);

        // Update .wslconfig and ensure an error is displayed if non-existent kernel or modules is specified.
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();
        const std::wstring nonExistentFile = L"DoesNotExist";
        WslConfigChange configChange(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str()}));
        ValidateOutput(
            L"echo ok",
            std::format(
                L"{}\r\nError code: Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND\r\n",
                wsl::shared::Localization::MessageCustomKernelNotFound(wslConfigPath, nonExistentFile)),
            L"");

        configChange.Update(LxssGenerateTestConfig({.kernelModules = nonExistentFile.c_str()}));
        ValidateOutput(
            L"echo ok",
            std::format(
                L"{}\r\nError code: Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND\r\n",
                wsl::shared::Localization::MessageCustomKernelModulesNotFound(wslConfigPath, nonExistentFile)),
            L"");

#ifdef WSL_DEV_INSTALL_PATH

        std::wstring kernelPath = WSL_DEV_INSTALL_PATH L"/kernel";
        std::wstring kernelModulesPath = WSL_DEV_INSTALL_PATH L"/modules.vhd";

#else

        auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        std::filesystem::path wslInstallPath(installPath.value());

        std::wstring kernelPath = wslInstallPath / "tools" / "kernel";
        std::wstring kernelModulesPath = wslInstallPath / "tools" / "modules.vhd";

#endif

        // Verify that no modules are mounted for a custom kernel with no modules specified.
        kernelPath = std::regex_replace(kernelPath, std::wregex(L"\\\\"), L"\\\\");
        configChange.Update(LxssGenerateTestConfig({.kernel = kernelPath.c_str()}));
        ValidateOutput(command.c_str(), L"", L"", 1);

        // Verify the error message if custom kernel modules are used with the default kernel.
        kernelModulesPath = std::regex_replace(kernelModulesPath, std::wregex(L"\\\\"), L"\\\\");
        configChange.Update(LxssGenerateTestConfig({.kernelModules = kernelModulesPath.c_str()}));
        ValidateOutput(
            L"echo ok",
            std::format(
                L"{}\r\nError code: Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND\r\n",
                wsl::shared::Localization::MessageMismatchedKernelModulesError()),
            L"");

        configChange.Update(LxssGenerateTestConfig());

        // Validate that tun is loaded by default.
        ValidateOutput(L"grep -i '^tun' /proc/modules | wc -l", L"1\n", L"", 0);

        // Validate a VM can boot with no extra additional kernel modules.
        configChange.Update(LxssGenerateTestConfig({.loadDefaultKernelModules = false}));
        ValidateOutput(L"grep -i '^tun' /proc/modules | wc -l", L"0\n", L"", 0);

        // Validate that the user can pass additional modules to load at boot.
        ValidateOutput(L"grep -iE '^(usb_storage|dm_crypt)' /proc/modules  | wc -l", L"0\n", L"", 0);

        configChange.Update(LxssGenerateTestConfig({.loadKernelModules = L"usb_storage,dm_crypt"}));
        ValidateOutput(L"grep -iE '^(usb_storage|dm_crypt)' /proc/modules  | wc -l", L"2\n", L"", 0);

        // Validate that failing to load a module shows a warning in dmesg.
        configChange.Update(LxssGenerateTestConfig({.loadKernelModules = L"not-found"}));
        ValidateOutput(L"dmesg | grep -iF \"failed to load module 'not-found'\" | wc -l", L"1\n", L"", 0);
    }

    TEST_METHOD(CrashCollection)
    {
        WSL2_TEST_ONLY();

        const auto folder = std::filesystem::absolute(L"test-crash-dumps");

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code error;
            std::filesystem::remove_all(folder, error);
        });

        auto countCrashes = [&]() {
            std::error_code error;
            return std::distance(std::filesystem::directory_iterator{folder, error}, std::filesystem::directory_iterator{});
        };

        auto waitForCrashes = [&](int expected) {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() { THROW_HR_IF(E_UNEXPECTED, countCrashes() < expected); }, std::chrono::seconds(1), std::chrono::minutes(2));

            VERIFY_ARE_EQUAL(countCrashes(), expected);
        };

        auto crash = []() { LxsstuLaunchWsl(L"kill -SEGV $$"); };

        WslConfigChange change(LxssGenerateTestConfig({.crashDumpCount = 2, .CrashDumpFolder = folder.wstring()}));

        VERIFY_ARE_EQUAL(countCrashes(), 0);

        crash();
        waitForCrashes(1);

        crash();
        waitForCrashes(2);

        crash();
        waitForCrashes(2);

        // Create a dummy file and validate that the file limit logic doesn't remove it.
        std::filesystem::remove_all(folder);
        std::filesystem::create_directory(folder);
        std::ofstream(folder / "dummy").close();

        crash();
        waitForCrashes(2);

        crash();
        waitForCrashes(3);

        crash();
        waitForCrashes(3);

        VERIFY_IS_TRUE(std::filesystem::exists(folder / "dummy"));
    }

    // UnitTests Private Methods

    static VOID VerifyCaseSensitiveDirectory(_In_ PCWSTR RelativePath)
    {

        const std::wstring Path = LxsstuGetLxssDirectory() + L"\\" + RelativePath;
        const wil::unique_hfile Directory{CreateFileW(
            Path.c_str(),
            FILE_READ_ATTRIBUTES,
            (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
            nullptr,
            OPEN_EXISTING,
            (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT),
            nullptr)};

        THROW_LAST_ERROR_IF(!Directory);
        IO_STATUS_BLOCK IoStatus;
        FILE_CASE_SENSITIVE_INFORMATION CaseInfo;
        THROW_IF_NTSTATUS_FAILED(NtQueryInformationFile(Directory.get(), &IoStatus, &CaseInfo, sizeof(CaseInfo), FileCaseSensitiveInformation));

        VERIFY_ARE_EQUAL(CaseInfo.Flags, (ULONG)FILE_CS_FLAG_CASE_SENSITIVE_DIR);
    }

    TEST_METHOD(Move)
    {
        constexpr auto name = L"move-test-distro";
        constexpr auto testFolder = L"move-test-test-folder";

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--import {} . \"{}\" --version 2", name, g_testDistroPath)), 0L);

        auto cleanupName = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            LxsstuLaunchWsl(std::format(L"--unregister {}", name));
            std::filesystem::remove_all(testFolder);
        });

        auto validateDistro = []() {
            auto [cmdOutput, _] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
            VERIFY_ARE_EQUAL(cmdOutput, L"ok\n");
        };

        // Move the distro to a different folder (relative path)
        {
            WslShutdown();
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--manage {} --move {}", name, testFolder)), 0L);

            // Validate that the distribution still starts
            validateDistro();
            VERIFY_IS_TRUE(std::filesystem::exists(std::format(L"{}\\ext4.vhdx", testFolder)));
        }

        auto absolutePath = std::filesystem::weakly_canonical(".").wstring();

        // Move the distro to a different folder (absolute path)
        {
            WslShutdown();
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--manage {} --move {}", name, absolutePath)), 0L);

            // Validate that the distribution still starts
            validateDistro();
            VERIFY_IS_TRUE(std::filesystem::exists(std::format(L"{}\\ext4.vhdx", absolutePath)));
        }

        // Try to move the distribution to a folder that's already in use
        {
            WslShutdown();

            wil::unique_cotaskmem_string path;
            THROW_IF_FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path));
            auto targetPath = std::format(L"{}\\lxss", path.get());
            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--manage {} --move {}", name, targetPath), -1);

            VERIFY_ARE_EQUAL(
                out,
                L"The supplied install location is already in use.\r\nError code: "
                L"Wsl/Service/MoveDistro/ERROR_FILE_EXISTS\r\n");
            // Validate that the distribution still starts and that the vhd hasn't moved.
            validateDistro();
            VERIFY_IS_TRUE(std::filesystem::exists(std::format(L"{}\\ext4.vhdx", absolutePath)));
        }

        // Try to move the distribution to an invalid path
        {
            WslShutdown();

            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--manage {} --move :", name), -1);

            VERIFY_ARE_EQUAL(
                out,
                L"The filename, directory name, or volume label syntax is incorrect. \r\nError code: "
                L"Wsl/Service/MoveDistro/ERROR_INVALID_NAME\r\n");
            // Validate that the distribution still starts and that the vhd hasn't moved.
            validateDistro();
            VERIFY_IS_TRUE(std::filesystem::exists(std::format(L"{}\\ext4.vhdx", absolutePath)));
        }
    }

    TEST_METHOD(Resize)
    {
        WSL2_TEST_ONLY();

        constexpr auto name = L"resize-test-distro";

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--import {} . \"{}\" --version 2", name, g_testDistroPath)), 0L);
        WslShutdown();

        auto cleanupName =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(std::format(L"--unregister {}", name)); });

        auto validateDistro = [](LPCWSTR size, LPCWSTR expectedSize, LPCWSTR expectedError = nullptr) {
            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--manage {} --resize {}", name, size), expectedError ? -1 : 0);
            if (expectedError)
            {
                VERIFY_ARE_EQUAL(expectedError, out);
                return;
            }

            std::tie(out, _) = LxsstuLaunchWslAndCaptureOutput(std::format(L"-d {} df -h / --output=size | sed 1d", name));
            VERIFY_ARE_EQUAL(std::format(L" {}\n", expectedSize), out);
            WslShutdown();
        };

        validateDistro(L"1500G", L"1.5T");
        validateDistro(L"500G", L"492G");
        validateDistro(L"1M", nullptr, L"Failed to resize disk.\r\nError code: Wsl/Service/E_FAIL\r\n");

        {
            WslKeepAlive keepAlive;
            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"--manage test_distro --resize 1500GB", -1);
            VERIFY_ARE_EQUAL(
                L"The operation could not be completed because the vhdx is currently in use. To force WSL to stop use: "
                L"wsl.exe "
                L"--shutdown\r\nError code: Wsl/Service/WSL_E_DISTRO_NOT_STOPPED\r\n",
                out);
        }
    }

    TEST_METHOD(FileOffsets)
    {
        WSL2_TEST_ONLY();

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(L"output.txt"); });

        std::ofstream file("output.txt");
        VERIFY_IS_TRUE(file.good() && file << "previous content\n");
        file.close();

        std::wstring cmd(L"C:\\windows\\system32\\cmd.exe /c \"wsl.exe echo ok >> output.txt && type output.txt\"");
        auto [output, _] = LxsstuLaunchCommandAndCaptureOutput(cmd.data());

        VERIFY_ARE_EQUAL(output, L"previous content\r\nok\n");
    }

    TEST_METHOD(GlobalFlagsOverride)
    {
        auto isDriveMountingEnabled = []() { return LxsstuLaunchWsl(L"test -d /mnt/c/Windows") == 0; };

        VERIFY_IS_TRUE(isDriveMountingEnabled());

        {
            RegistryKeyChange<DWORD> key(HKEY_LOCAL_MACHINE, LXSS_SERVICE_REGISTRY_PATH, L"DistributionFlags", ~LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING);

            TerminateDistribution();
            VERIFY_IS_FALSE(isDriveMountingEnabled());
        }

        TerminateDistribution();
        VERIFY_IS_TRUE(isDriveMountingEnabled());
    }

    TEST_METHOD(WriteWslConfig)
    {
        WSL2_TEST_ONLY();
        WSL_SETTINGS_TEST();

        auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        std::filesystem::path wslInstallPath(installPath.value());
        std::filesystem::path libWslDllPath = wslInstallPath / "libwsl.dll";
        VERIFY_IS_TRUE(std::filesystem::exists(libWslDllPath));

        LxssDynamicFunction<decltype(GetWslConfigFilePath)> getWslConfigFilePath(libWslDllPath.c_str(), "GetWslConfigFilePath");
        LxssDynamicFunction<decltype(CreateWslConfig)> createWslConfig(libWslDllPath.c_str(), "CreateWslConfig");
        LxssDynamicFunction<decltype(FreeWslConfig)> freeWslConfig(libWslDllPath.c_str(), "FreeWslConfig");
        LxssDynamicFunction<decltype(GetWslConfigSetting)> getWslConfigSetting(libWslDllPath.c_str(), "GetWslConfigSetting");
        LxssDynamicFunction<decltype(SetWslConfigSetting)> setWslConfigSetting(libWslDllPath.c_str(), "SetWslConfigSetting");

        // Delete the test config file. The original has already been saved as part of module setup.
        auto wslConfigFilePath = getenv("userprofile") + std::string("\\.wslconfig");
        if (std::filesystem::exists(wslConfigFilePath))
        {
            std::error_code ec{};
            VERIFY_IS_TRUE(std::filesystem::remove(wslConfigFilePath, ec));
        }

        auto apiWslConfigFilePath = getWslConfigFilePath();
        VERIFY_IS_TRUE(std::filesystem::path(wslConfigFilePath) == std::filesystem::path(apiWslConfigFilePath));

        // Cleanup any leftover config files.
        auto cleanup = wil::scope_exit([apiWslConfigFilePath] { std::filesystem::remove(apiWslConfigFilePath); });

        auto wslConfigDefaults = createWslConfig(nullptr);
        VERIFY_IS_NOT_NULL(wslConfigDefaults);
        auto wslConfig = createWslConfig(apiWslConfigFilePath);
        VERIFY_IS_NOT_NULL(wslConfig);

        freeWslConfig(wslConfigDefaults);
        freeWslConfig(wslConfig);

        WslConfigSetting wslConfigSettingWriteOut;
        WslConfigSetting wslConfigSettingReadIn;

        auto testLoop = [&](auto& testPlan, auto& updateWslConfigSettingWriteOutValue, auto& verifyWslConfigSettingValueReadEqual) {
            wslConfigSettingWriteOut = wslConfigSettingReadIn = WslConfigSetting{};
            for (const auto testEntry : testPlan)
            {
                wslConfigSettingWriteOut = testEntry.first;
                for (const auto& test : testEntry.second)
                {
                    const auto& writeValue = test.first;
                    const auto& expectedValue = test.second;
                    {
                        // This scenario tests writing a value to the config file and reading it back. If the write succeeded,
                        // the written value will be cached in the WslConfig object. The read will then return the cached value.
                        wslConfig = createWslConfig(apiWslConfigFilePath);
                        VERIFY_IS_NOT_NULL(wslConfig);
                        auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });

                        updateWslConfigSettingWriteOutValue(wslConfigSettingWriteOut, writeValue);

                        VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSettingWriteOut), ERROR_SUCCESS);
                        wslConfigSettingReadIn = getWslConfigSetting(wslConfig, wslConfigSettingWriteOut.ConfigEntry);
                        VERIFY_ARE_EQUAL(wslConfigSettingReadIn.ConfigEntry, wslConfigSettingWriteOut.ConfigEntry);
                        verifyWslConfigSettingValueReadEqual(wslConfigSettingReadIn, expectedValue);
                    }
                    {
                        // This scenario tests reading a value from the config file. Specifically, it will parse in the
                        // written value to the wsl config file from the previous scenario. This validates parsing the value
                        // from the file (e.g. that it was written correctly and then parsed as expected).
                        wslConfig = createWslConfig(apiWslConfigFilePath);
                        auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });
                        wslConfigSettingReadIn = getWslConfigSetting(wslConfig, wslConfigSettingWriteOut.ConfigEntry);
                        VERIFY_ARE_EQUAL(wslConfigSettingReadIn.ConfigEntry, wslConfigSettingWriteOut.ConfigEntry);
                        verifyWslConfigSettingValueReadEqual(wslConfigSettingReadIn, expectedValue);
                    }
                }
            }
        };

        {
            // Enable NetworkingMode::Mirrored for IgnoredPorts to be set correctly upon parsing.
            WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));

            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<PCWSTR, PCWSTR>> filePathsToTest{
                {L"C:\\DoesNotExit\\ext4.vhdx", L"C:\\DoesNotExit\\ext4.vhdx"},
                {L"\\DoesNotExit\\ext4.vhdx", L"\\DoesNotExit\\ext4.vhdx"},
                {L"", L""},
            };

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<PCWSTR, PCWSTR>>>> wslConfigSettingStringTestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::SwapFilePath},
                    filePathsToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::IgnoredPorts},
                    {
                        {L"1,2,300,4455,65535", L"1,2,300,4455,65535"},
                        {L"10,20,-100,p", L"10,20"},
                        {L"100,200,notaport", L"100,200"},
                        {L"1000,2000;3.4", L"1000,2000"},
                        {L"10000, 20000,        30000,40000        ,50000", L"10000,20000,30000,40000,50000"},
                        {L"", L""},
                        {L"notaport", L""},
                        {L"-5555", L""},
                        {L"C:\\DoesNotExit\\ext4.vhdx", L""},
                    },
                },
                {
                    {.ConfigEntry = WslConfigEntry::KernelPath},
                    filePathsToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::SystemDistroPath},
                    filePathsToTest,
                },
            };

            auto updateWslConfigSettingWriteOutStringValue = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.StringValue = writeValue;
            };

            auto verifyWslConfigSettingReadStringValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(std::wstring_view(wslConfigSettingReadIn.StringValue), std::wstring_view(expectedValue));
            };

            testLoop(wslConfigSettingStringTestPlan, updateWslConfigSettingWriteOutStringValue, verifyWslConfigSettingReadStringValueEqual);
        }

        {
            wslConfigSettingWriteOut = wslConfigSettingReadIn = WslConfigSetting{};
            wslConfigSettingWriteOut.ConfigEntry = WslConfigEntry::NoEntry;

            wslConfig = createWslConfig(apiWslConfigFilePath);
            VERIFY_IS_NOT_NULL(wslConfig);
            auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });

            wslConfigSettingReadIn = getWslConfigSetting(wslConfig, wslConfigSettingWriteOut.ConfigEntry);
            VERIFY_ARE_EQUAL(wslConfigSettingReadIn.ConfigEntry, wslConfigSettingWriteOut.ConfigEntry);
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        {
            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<int, int>> timeoutValuesToTest{
                {-132445, -132445},
                {0, 0},
                {1, 1},
                {13456, 13456},
                {100000000, 100000000},
            };

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<int, int>>>> wslConfigSettingInt32TestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::ProcessorCount},
                    {
                        {-123443, systemInfo.dwNumberOfProcessors},
                        {-1, systemInfo.dwNumberOfProcessors},
                        {1, 1},
                        {2, std::min(2, static_cast<int>(systemInfo.dwNumberOfProcessors))},
                        {systemInfo.dwNumberOfProcessors, systemInfo.dwNumberOfProcessors},
                        {1234, systemInfo.dwNumberOfProcessors},
                    },
                },
                {
                    {.ConfigEntry = WslConfigEntry::InitialAutoProxyTimeout},
                    timeoutValuesToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::VMIdleTimeout},
                    timeoutValuesToTest,
                },
            };

            auto updateWslConfigSettingWriteOutInt32Value = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.Int32Value = writeValue;
            };

            auto verifyWslConfigSettingReadInt32ValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(wslConfigSettingReadIn.Int32Value, expectedValue);
            };

            testLoop(wslConfigSettingInt32TestPlan, updateWslConfigSettingWriteOutInt32Value, verifyWslConfigSettingReadInt32ValueEqual);
        }

        {
            MEMORYSTATUSEX memInfo{sizeof(MEMORYSTATUSEX)};
            THROW_IF_WIN32_BOOL_FALSE(GlobalMemoryStatusEx(&memInfo));
            const auto minimumMemorySizeBytes = 256 * _1MB;
            const auto maximumMemorySizeBytes = memInfo.ullTotalPhys;

            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<unsigned long long, unsigned long long>> fileSizesBytesToTest{
                {0, 0}, {1, 1}, {13456, 13456}, {100000000, 100000000}, {9223372036854775807, 9223372036854775807}};

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<unsigned long long, unsigned long long>>>> wslConfigSettingUInt64TestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::MemorySizeBytes},
                    {
                        {0, maximumMemorySizeBytes / 2},
                        {minimumMemorySizeBytes / 2, minimumMemorySizeBytes},
                        {minimumMemorySizeBytes, minimumMemorySizeBytes},
                        {maximumMemorySizeBytes / 2, maximumMemorySizeBytes / 2},
                        {maximumMemorySizeBytes, maximumMemorySizeBytes},
                        {maximumMemorySizeBytes * 2, maximumMemorySizeBytes},
                    },
                },
                {
                    {.ConfigEntry = WslConfigEntry::SwapSizeBytes},
                    fileSizesBytesToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::VhdSizeBytes},
                    fileSizesBytesToTest,
                },
            };

            auto updateWslConfigSettingWriteOutUInt64Value = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.UInt64Value = writeValue;
            };

            auto verifyWslConfigSettingReadUInt64ValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(wslConfigSettingReadIn.UInt64Value, expectedValue);
            };

            testLoop(wslConfigSettingUInt64TestPlan, updateWslConfigSettingWriteOutUInt64Value, verifyWslConfigSettingReadUInt64ValueEqual);
        }

        {
            // Enable NetworkingMode::Mirrored for IgnoredPorts to be set correctly upon parsing.
            WslConfigChange config(LxssGenerateTestConfig());

            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<bool, bool>> booleansToTest{{false, false}, {true, true}};

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<bool, bool>>>> wslConfigSettingBooleanTestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::FirewallEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::LocalhostForwardingEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::HostAddressLoopbackEnabled},
                    // This setting is only enabled when NetworkingMode != Mirrored.
                    {{false, false}, {true, false}},
                },
                {
                    {.ConfigEntry = WslConfigEntry::AutoProxyEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::DNSProxyEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::DNSTunellingEnabled},
                    // This setting is only enabled when NetworkingMode != Nat && NetworkingMode != Mirrored
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::BestEffortDNSParsingEnabled},
                    // This setting is only enabled when DNSTunellingEnabled = true
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::GUIApplicationsEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::NestedVirtualizationEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::SafeModeEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::SparseVHDEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::DebugConsoleEnabled},
                    booleansToTest,
                },
                {
                    {.ConfigEntry = WslConfigEntry::HardwarePerformanceCountersEnabled},
                    // This setting is disabled when SafeModeEnabled = true.
                    // Since testing SafeModeEnabled is tested earlier and left as
                    // true (.wslconfig is re-used), this setting should be false.
                    {{false, false}, {true, false}},
                },
            };

            auto updateWslConfigSettingWriteOutBooleanValue = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.BoolValue = writeValue;
            };

            auto verifyWslConfigSettingReadBooleanValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(wslConfigSettingReadIn.BoolValue, expectedValue);
            };

            testLoop(wslConfigSettingBooleanTestPlan, updateWslConfigSettingWriteOutBooleanValue, verifyWslConfigSettingReadBooleanValueEqual);
        }

        {
            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<NetworkingConfiguration, NetworkingConfiguration>> networkingConfigurationsToTest{
                {NetworkingConfiguration::None, NetworkingConfiguration::None},
                {NetworkingConfiguration::Nat, NetworkingConfiguration::Nat},
                {NetworkingConfiguration::Bridged, NetworkingConfiguration::Bridged},
                {NetworkingConfiguration::Mirrored, NetworkingConfiguration::Mirrored},
                {NetworkingConfiguration::VirtioProxy, NetworkingConfiguration::VirtioProxy},
            };

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<NetworkingConfiguration, NetworkingConfiguration>>>> wslConfigSettingNetworkingConfigurationTestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::Networking},
                    networkingConfigurationsToTest,
                },
            };

            auto updateWslConfigSettingWriteOutNetworkingConfigurationValue = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.NetworkingConfigurationValue = writeValue;
            };

            auto verifyWslConfigSettingReadNetworkingConfigurationValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(expectedValue, wslConfigSettingReadIn.NetworkingConfigurationValue);
            };

            testLoop(
                wslConfigSettingNetworkingConfigurationTestPlan,
                updateWslConfigSettingWriteOutNetworkingConfigurationValue,
                verifyWslConfigSettingReadNetworkingConfigurationValueEqual);
        }

        {
            // std::pair[0] = Written value, std::pair[1] = Actual/Expected value
            static const std::vector<std::pair<MemoryReclaimConfiguration, MemoryReclaimConfiguration>> memoryReclaimModesToTest{
                {MemoryReclaimConfiguration::Disabled, MemoryReclaimConfiguration::Disabled},
                {MemoryReclaimConfiguration::Gradual, MemoryReclaimConfiguration::Gradual},
                {MemoryReclaimConfiguration::DropCache, MemoryReclaimConfiguration::DropCache},
            };

            // tuple: WslConfigSetting, expectedValue, actualValue
            std::vector<std::pair<WslConfigSetting, std::vector<std::pair<MemoryReclaimConfiguration, MemoryReclaimConfiguration>>>> wslConfigSettingMemoryReclaimModeTestPlan{
                {
                    {.ConfigEntry = WslConfigEntry::AutoMemoryReclaim},
                    memoryReclaimModesToTest,
                },
            };

            auto updateWslConfigSettingWriteOutMemoryReclaimModeValue = [](auto& wslConfigSettingWriteOut, auto& writeValue) {
                wslConfigSettingWriteOut.MemoryReclaimModeValue = writeValue;
            };

            auto verifyWslConfigSettingReadMemoryReclaimModeValueEqual = [](auto& wslConfigSettingReadIn, auto& expectedValue) {
                VERIFY_ARE_EQUAL(wslConfigSettingReadIn.MemoryReclaimModeValue, expectedValue);
            };

            testLoop(wslConfigSettingMemoryReclaimModeTestPlan, updateWslConfigSettingWriteOutMemoryReclaimModeValue, verifyWslConfigSettingReadMemoryReclaimModeValueEqual);
        }

        {
            std::wstring customWslConfigContentOut{
                LR"(
[wsl2] # trailing section comment
vmIdleTimeout=200          # property trailing comment
vmIdleTimeout=20000          # property trailing comment
vmIdleTimeout=20000          # property trailing comment
mountDeviceTimeout=120\
000
kernelBootTimeout=120000

# property comment
swapfile=E:\\wsl-b\
uild\\src\\win\
dows\\wslc\
ore\\lib\\swap.vhdx # multi-line property with trailing comment
telemetry=false
safeMode=false
guiApplications=true
earlyBootLogging=false
# comment 1
# comment 2
# \t \b
virtio9p=true # property trailing comment, ensure new property is appended to the section while preserving this comment

# section comment
[experimental]
autoProxy=false

[wsl2]

# end comment
)"};

            WslConfigChange config(customWslConfigContentOut);

            wslConfig = createWslConfig(apiWslConfigFilePath);
            VERIFY_IS_NOT_NULL(wslConfig);
            auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });

            // The config contains multiple vmIdleTimeout entries. The first one should be updated/written.
            wslConfigSettingWriteOut = WslConfigSetting{};
            wslConfigSettingWriteOut.ConfigEntry = WslConfigEntry::VMIdleTimeout;
            wslConfigSettingWriteOut.Int32Value = 1234;

            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSettingWriteOut), ERROR_SUCCESS);

            // Replace the swapfile path, which is a multi-line property with a trailing comment.
            // The multi-line value should be replaced with the new value and trailing comment preserved.
            wslConfigSettingWriteOut.ConfigEntry = WslConfigEntry::SwapFilePath;
            wslConfigSettingWriteOut.StringValue = LR"(C:\DoesNotExist\swap.vhdx)";

            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSettingWriteOut), ERROR_SUCCESS);

            // Write out a new setting that doesn't exist in the original config but its' section
            // does. The new setting should be appended to that section. There are two cases here::
            wslConfigSettingWriteOut.ConfigEntry = WslConfigEntry::HardwarePerformanceCountersEnabled;
            wslConfigSettingWriteOut.BoolValue = true;

            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSettingWriteOut), ERROR_SUCCESS);

            wslConfigSettingWriteOut.ConfigEntry = WslConfigEntry::AutoMemoryReclaim;
            wslConfigSettingWriteOut.MemoryReclaimModeValue = MemoryReclaimConfiguration::Gradual;

            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSettingWriteOut), ERROR_SUCCESS);

            std::wstring customWslConfigContentExpected{
                LR"(
[wsl2] # trailing section comment
vmIdleTimeout=1234          # property trailing comment
vmIdleTimeout=20000          # property trailing comment
vmIdleTimeout=20000          # property trailing comment
mountDeviceTimeout=120\
000
kernelBootTimeout=120000

# property comment
swapfile=C:\\DoesNotExist\\swap.vhdx # multi-line property with trailing comment
telemetry=false
safeMode=false
guiApplications=true
earlyBootLogging=false
# comment 1
# comment 2
# \t \b
virtio9p=true # property trailing comment, ensure new property is appended to the section while preserving this comment

# section comment
[experimental]
autoProxy=false
autoMemoryReclaim=Gradual

[wsl2]

# end comment
)"};

            std::wifstream configRead(apiWslConfigFilePath);
            auto customWslConfigContentActual = std::wstring{std::istreambuf_iterator<wchar_t>(configRead), {}};
            configRead.close();
            VERIFY_ARE_EQUAL(customWslConfigContentExpected, customWslConfigContentActual);
        }

        {
            // This test contains an invalid line ('babyshark') in the wsl2 section.
            // The line should be preserved and no additional spacing/lines should be added.
            std::wstring customWslConfigContentOut{
                LR"(
[wsl2]
memory=32G
processors=12
hostAddressLoopback=false
dnsTunneling=true
defaultVhdSize=1099511627776
babyshark
localhostForwarding=true
autoProxy=false
)"};

            WslConfigChange config(customWslConfigContentOut);

            wslConfig = createWslConfig(apiWslConfigFilePath);
            VERIFY_IS_NOT_NULL(wslConfig);
            auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });

            auto wslConfigSetting = getWslConfigSetting(wslConfig, WslConfigEntry::AutoProxyEnabled);
            const auto autoProxyEnabled = false;
            VERIFY_ARE_EQUAL(wslConfigSetting.BoolValue, autoProxyEnabled);

            wslConfigSetting.BoolValue = !autoProxyEnabled;
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigSetting), ERROR_SUCCESS);

            std::wstring customWslConfigContentExpected{
                LR"(
[wsl2]
memory=32G
processors=12
hostAddressLoopback=false
dnsTunneling=true
defaultVhdSize=1099511627776
babyshark
localhostForwarding=true
)"};

            std::wifstream configRead(apiWslConfigFilePath);
            auto customWslConfigContentActual = std::wstring{std::istreambuf_iterator<wchar_t>(configRead), {}};
            configRead.close();
            VERIFY_ARE_EQUAL(customWslConfigContentActual, customWslConfigContentExpected);
        }

        {
            // This test verifies removal of a setting from the .wslconfig when a default value for the particular setting is
            // set. This gives wsl control over the default value.
            std::wstring customWslConfigContentOut{
                LR"(
[wsl2]
memory=32G
processors=12 # property trailing comment
hostAddressLoopback=false
dnsTunneling=true
defaultVhdSize=1099511627776
localhostForwarding=true
autoProxy=false
)"};

            WslConfigChange config(customWslConfigContentOut);

            wslConfig = createWslConfig(apiWslConfigFilePath);
            VERIFY_IS_NOT_NULL(wslConfig);
            auto cleanupWslConfig = wil::scope_exit([&] { freeWslConfig(wslConfig); });

            wslConfigDefaults = createWslConfig(nullptr);
            VERIFY_IS_NOT_NULL(wslConfigDefaults);
            auto cleanupWslConfigDefaults = wil::scope_exit([&] { freeWslConfig(wslConfigDefaults); });

            // This setting should be removed from the .wslconfig file.
            auto wslConfigDefaultSettingMemorySize = getWslConfigSetting(wslConfigDefaults, WslConfigEntry::MemorySizeBytes);
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigDefaultSettingMemorySize), ERROR_SUCCESS);

            // This setting should be removed from the .wslconfig file but trailing comment preserved.
            auto wslConfigDefaultSettingProcessorCount = getWslConfigSetting(wslConfigDefaults, WslConfigEntry::ProcessorCount);
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigDefaultSettingProcessorCount), ERROR_SUCCESS);

            // This setting should be preserved with an updated value in the .wslconfig file.
            auto wslConfigDefaultSettingVhdSize = getWslConfigSetting(wslConfigDefaults, WslConfigEntry::VhdSizeBytes);
            wslConfigDefaultSettingVhdSize.UInt64Value -= 1;
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigDefaultSettingVhdSize), ERROR_SUCCESS);

            // This setting should be removed from the .wslconfig file.
            auto wslConfigDefaultSettingAutoProxy = getWslConfigSetting(wslConfigDefaults, WslConfigEntry::AutoProxyEnabled);
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigDefaultSettingAutoProxy), ERROR_SUCCESS);

            // This setting should not be written to the .wslconfig file.
            auto wslConfigDefaultSettingGuiApplications = getWslConfigSetting(wslConfigDefaults, WslConfigEntry::GUIApplicationsEnabled);
            VERIFY_ARE_EQUAL(setWslConfigSetting(wslConfig, wslConfigDefaultSettingGuiApplications), ERROR_SUCCESS);

            std::wstring customWslConfigContentExpected{
                LR"(
[wsl2]
# property trailing comment
hostAddressLoopback=false
dnsTunneling=true
defaultVhdSize=1099511627775
localhostForwarding=true
)"};

            std::wifstream configRead(apiWslConfigFilePath);
            auto customWslConfigContentActual = std::wstring{std::istreambuf_iterator<wchar_t>(configRead), {}};
            configRead.close();
            VERIFY_ARE_EQUAL(customWslConfigContentActual, customWslConfigContentExpected);
        }
    }

    TEST_METHOD(LaunchWslSettingsFromProtocol)
    {
        WSL_SETTINGS_TEST();

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        SHELLEXECUTEINFOW execInfo{};
        execInfo.cbSize = sizeof(execInfo);
        execInfo.fMask = SEE_MASK_CLASSNAME | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
        execInfo.lpClass = L"wsl-settings";
        execInfo.lpFile = L"wsl-settings://";
        execInfo.nShow = SW_HIDE;

        VERIFY_WIN32_BOOL_SUCCEEDED(ShellExecuteExW(&execInfo));
        const wil::unique_process_handle process{execInfo.hProcess};
        VERIFY_IS_NOT_NULL(process.get());

        auto killProcess = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&process]() {
            if (process)
            {
                LOG_IF_WIN32_BOOL_FALSE(TerminateProcess(process.get(), 0));
            }
        });

        const auto moduleFileName = wil::GetModuleFileNameExW<std::wstring>(process.get(), nullptr);
        const auto findExeName = moduleFileName.find(L"wslsettings.exe");
        VERIFY_ARE_NOT_EQUAL(findExeName, std::wstring::npos);
    }

    TEST_METHOD(ManageDefaultUid)
    {
        const auto distroKey = OpenDistributionKey(LXSS_DISTRO_NAME_TEST_L);

        auto assertDefaultUid = [&](ULONG ExpectedUid) {
            const auto uid = wsl::windows::common::registry::ReadDword(distroKey.get(), nullptr, L"DefaultUid", 0);

            VERIFY_ARE_EQUAL(ExpectedUid, uid);

            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"id -u");
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            {
                out.pop_back();
            }

            VERIFY_ARE_EQUAL(out, std::to_wstring(ExpectedUid));
        };

        assertDefaultUid(0);

        auto validateUidChange =
            [&](const std::wstring& User, ULONG expectedDefaultUid, LPCWSTR ExpectedOutput, const std::wstring& ExpectedError, int ExpectedExitCode) {
                auto [out, err] = LxsstuLaunchWslAndCaptureOutput(
                    std::format(L"--manage {} --set-default-user {}", LXSS_DISTRO_NAME_TEST_L, User), ExpectedExitCode);

                VERIFY_ARE_EQUAL(out, ExpectedOutput);
                VERIFY_ARE_EQUAL(err, ExpectedError);

                assertDefaultUid(expectedDefaultUid);
            };

        validateUidChange(L"root", 0, L"The operation completed successfully. \r\n", L"", 0);

        constexpr auto TestUser = L"testuser";

        auto cleanup =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(std::format(L"-u root userdel -f {}", TestUser)); });

        ULONG Uid{};
        ULONG Gid{};
        CreateUser(TestUser, &Uid, &Gid);
        VERIFY_ARE_NOT_EQUAL(Uid, 0);

        validateUidChange(L"testuser", Uid, L"The operation completed successfully. \r\n", L"", 0);
        validateUidChange(L"root", 0, L"The operation completed successfully. \r\n", L"", 0);

        const std::wstring invalidUser = L"DoesntExist";
        validateUidChange(invalidUser, 0, L"", L"/usr/bin/id: \u2018" + invalidUser + L"\u2019: no such user\n", 1);

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"--manage doesntexist --set-default-user root", -1);

        VERIFY_ARE_EQUAL(
            out, L"There is no distribution with the supplied name.\r\nError code: Wsl/Service/WSL_E_DISTRO_NOT_FOUND\r\n");
    }

    TEST_METHOD(PostDistroRegistrationSettingsOOBE)
    {
        WSL_SETTINGS_TEST();

        wsl::windows::common::SvcComm service;
        const auto distros = service.EnumerateDistributions();
        if (distros.size() != 1)
        {
            LogSkipped("Test distro as the only distro is required to run this test.");
            return;
        }

        const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        // Test setup should set OOBEComplete
        VERIFY_ARE_EQUAL(bool(wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, false)), true);

        // Delete the OOBEComplete reg value to simulate OOBE not being complete
        wsl::windows::common::registry::DeleteValue(lxssKey.get(), LXSS_OOBE_COMPLETE_NAME);

        // Restore the OOBEComplete reg value in case of failure
        auto restoreOOBEComplete = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            wsl::windows::common::registry::WriteDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, true);
        });

        constexpr auto wslSettingsWindowName = L"Welcome to Windows Subsystem for Linux";
        VERIFY_ARE_EQUAL(FindWindowEx(nullptr, nullptr, nullptr, wslSettingsWindowName), nullptr);

        auto testDistro = distros.front();
        VERIFY_IS_TRUE(wsl::shared::string::IsEqual(testDistro.DistroName, LXSS_DISTRO_NAME_TEST_L, false));
        // Get the original BasePath in order to restore the test distro as before.
        auto guidStringWithBraces = wsl::shared::string::GuidToString<wchar_t>(testDistro.DistroGuid);
        auto testDistroBasePath =
            wsl::windows::common::registry::ReadString(lxssKey.get(), guidStringWithBraces.c_str(), L"BasePath", L"");
        VERIFY_ARE_NOT_EQUAL(testDistroBasePath, L"");

        if (LxsstuVmMode())
        {
            const auto testDistroVhdPath = std::filesystem::path(testDistroBasePath) / LXSS_VM_MODE_VHD_NAME;
            VERIFY_IS_TRUE(std::filesystem::exists(testDistroVhdPath));
            const auto testDistroVhdPathExported = std::filesystem::path(testDistroBasePath) / L"exported.vhdx";

            WslShutdown();
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(std::format(L"--export {} \"{}\" --vhd", testDistro.DistroName, testDistroVhdPathExported.c_str())), 0u);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--unregister {}", testDistro.DistroName)), 0u);
            VERIFY_IS_FALSE(std::filesystem::exists(testDistroVhdPath));
            VERIFY_IS_TRUE(service.EnumerateDistributions().empty());

            std::error_code ec{};
            std::filesystem::rename(testDistroVhdPathExported, testDistroVhdPath, ec);

            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(std::format(L"--import-in-place {} \"{}\"", testDistro.DistroName, testDistroVhdPath.c_str())), 0L);
        }
        else
        {
            const auto testDistroRootfsPath = std::filesystem::path(testDistroBasePath) / LXSS_ROOTFS_DIRECTORY;
            VERIFY_IS_TRUE(std::filesystem::exists(testDistroRootfsPath));
            const auto testDistroExported = std::filesystem::path(testDistroBasePath) / L"exported.tar";
            auto deleteTar = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { DeleteFile(testDistroExported.c_str()); });

            WslShutdown();
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export {} \"{}\"", testDistro.DistroName, testDistroExported.c_str())), 0u);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--unregister {}", testDistro.DistroName)), 0u);
            VERIFY_IS_FALSE(std::filesystem::exists(testDistroRootfsPath));
            VERIFY_IS_TRUE(service.EnumerateDistributions().empty());
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(std::format(
                    L"--import {} \"{}\" \"{}\" --version 1", testDistro.DistroName, testDistroBasePath, testDistroExported.c_str())),
                0L);
        }

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--set-default {}", testDistro.DistroName)), 0);

        VERIFY_ARE_EQUAL(service.EnumerateDistributions().size(), 1);
        HWND wslSettingsWindow{};
        const auto findWslSettingsWindowAttempts = 60;
        for (auto attempt = 0; attempt < findWslSettingsWindowAttempts; ++attempt)
        {
            wslSettingsWindow = FindWindowEx(nullptr, nullptr, nullptr, wslSettingsWindowName);
            if (wslSettingsWindow)
            {
                break;
            }

            Sleep(500);
        }

        VERIFY_ARE_NOT_EQUAL(wslSettingsWindow, nullptr);
        SendMessage(wslSettingsWindow, WM_CLOSE, 0, 0);
        VERIFY_ARE_EQUAL(bool(wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, false)), true);
    }

    TEST_METHOD(VersionFlavorParsing)
    {
        DWORD currentVersion = LxsstuVmMode() ? 2 : 1;
        DWORD convertVersion = LxsstuVmMode() ? 1 : 2;

        const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();

        auto validateFlavorVersion = [&](LPCWSTR Distro, LPCWSTR ExpectedFlavor, LPCWSTR ExpectedVersion) {
            const auto testDistroId = GetDistributionId(Distro);
            VERIFY_IS_TRUE(testDistroId.has_value());

            const auto distroId = wsl::shared::string::GuidToString<wchar_t>(testDistroId.value());

            TerminateDistribution(Distro);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"-d {} cat /etc/os-release || true", Distro).c_str()), 0L);

            const auto flavor = wsl::windows::common::registry::ReadString(lxssKey.get(), distroId.c_str(), L"Flavor", L"");
            const auto version = wsl::windows::common::registry::ReadString(lxssKey.get(), distroId.c_str(), L"OsVersion", L"");

            VERIFY_ARE_EQUAL(ExpectedFlavor, flavor);
            VERIFY_ARE_EQUAL(ExpectedVersion, version);
        };

        validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"debian", L"12");

        constexpr auto testTar = L"exported-distro.tar";
        constexpr auto tmpDistroName = L"tmpdistro";

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            DeleteFile(testTar);
            LxsstuLaunchWsl(std::format(L"--unregister {}", tmpDistroName));
        });

        DistroFileChange osRelease(L"/etc/os-release");

        {
            osRelease.SetContent(
                LR"(
ID=Distro
VERSION_ID=Version
            )");

            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"Distro", L"Version");
        }

        {
            osRelease.SetContent(
                LR"(
DISTRO_I=Wrong
ID="DistroWithQuotes"
VERSION_ID="VersionWithQuotes"
Something else
            )");

            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"DistroWithQuotes", L"VersionWithQuotes");
        }

        {
            osRelease.SetContent(
                LR"(
ID="InvalidFormat!"
VERSION_ID="ValidFormat"
            )");

            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"DistroWithQuotes", L"ValidFormat");
        }

        {
            osRelease.SetContent(
                LR"(
ID="Distro-_.,"
VERSION_ID="ValidFormat"
            )");

            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"Distro-_.,", L"ValidFormat");
        }

        {
            osRelease.SetContent(
                LR"(
ID="Invalid|Format"
VERSION_ID="Invalid|Format"
            )");

            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"Distro-_.,", L"ValidFormat");
        }

        {
            osRelease.Delete(); // Nothing should happen if the file is deleted, but the distro should still work.
            validateFlavorVersion(LXSS_DISTRO_NAME_TEST_L, L"Distro-_.,", L"ValidFormat");
        }

        // Validate that importing a distro without os-release works.
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export {} {}", LXSS_DISTRO_NAME_TEST_L, testTar).c_str()), 0L);
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(std::format(L"--import {} . {} --version {}", tmpDistroName, testTar, currentVersion).c_str()), 0L);

            validateFlavorVersion(tmpDistroName, L"", L"");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"-d {} echo -e 'VERSION_ID=v' > /etc/os-release", tmpDistroName).c_str()), 0L);
            validateFlavorVersion(tmpDistroName, L"", L"v");
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--unregister {}", tmpDistroName).c_str()), 0L);
        }

        // Validate that importing and then converting also behaves correctly when there's no os-release
        {
            VERIFY_ARE_EQUAL(
                LxsstuLaunchWsl(std::format(L"--import {} . {} --version {}", tmpDistroName, testTar, convertVersion).c_str()), 0L);
            validateFlavorVersion(tmpDistroName, L"", L"");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--set-version {} {}", tmpDistroName, currentVersion).c_str()), 0L);

            validateFlavorVersion(tmpDistroName, L"", L"");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"-d {} echo -e 'VERSION_ID=v2' > /etc/os-release", tmpDistroName).c_str()), 0L);
            validateFlavorVersion(tmpDistroName, L"", L"v2");
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--unregister {}", tmpDistroName).c_str()), 0L);
        }

        // Verify that importing a distribution with an os-release as then converting works as well
        VERIFY_ARE_EQUAL(
            LxsstuLaunchWsl(std::format(L"--import {} . {} --version {}", tmpDistroName, g_testDistroPath, convertVersion).c_str()), 0L);
        validateFlavorVersion(tmpDistroName, L"debian", L"12");

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--set-version {} {}", tmpDistroName, currentVersion).c_str()), 0L);
        validateFlavorVersion(tmpDistroName, L"debian", L"12");
    }

    TEST_METHOD(DistributionId)
    {
        using namespace wsl::windows::common::string;
        const auto testDistroId = GetDistributionId(LXSS_DISTRO_NAME_TEST_L);
        VERIFY_IS_TRUE(testDistroId.has_value());

        auto validateOutput = [](const std::wstring& Cmd, LPCWSTR ExpectedOutput, int ExitCode = 0) {
            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(Cmd, ExitCode);

            VERIFY_ARE_EQUAL(out, ExpectedOutput);
        };

        validateOutput(
            std::format(
                L"--distribution-id {} echo -n OK",
                wsl::shared::string::GuidToString<wchar_t>(testDistroId.value(), wsl::shared::string::GuidToStringFlags::None)),
            L"OK");

        validateOutput(
            std::format(
                L"--distribution-id {} echo -n OK",
                wsl::shared::string::GuidToString<wchar_t>(testDistroId.value(), wsl::shared::string::GuidToStringFlags::AddBraces)),
            L"OK");

        validateOutput(
            std::format(
                L"--distribution-id {} echo -n OK",
                wsl::shared::string::GuidToString<wchar_t>(testDistroId.value(), wsl::shared::string::GuidToStringFlags::Uppercase)),
            L"OK");

        validateOutput(L"--distribution-id InvalidGuid", L"The parameter is incorrect. \r\nError code: Wsl/E_INVALIDARG\r\n", -1);
        validateOutput(
            L"--distribution-id  {C13B2B63-F9D5-4840-8105-F6ABECCF46CA}",
            L"There is no distribution with the supplied name.\r\nError code: "
            L"Wsl/Service/CreateInstance/ReadDistroConfig/WSL_E_DISTRO_NOT_FOUND\r\n",
            -1);
    }

    TEST_METHOD(ModernOOBE)
    {
        const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        const auto testDistroId = GetDistributionId(LXSS_DISTRO_NAME_TEST_L);
        VERIFY_IS_TRUE(testDistroId.has_value());
        const auto testDistroIdString = wsl::shared::string::GuidToString<wchar_t>(testDistroId.value());

        DistroFileChange distributionconf(L"/etc/wsl-distribution.conf", false);
        distributionconf.SetContent(L"[oobe]\ncommand = /bin/bash -c 'echo OOBE'\n");

        RegistryKeyChange<DWORD> runOOBE(lxssKey.get(), testDistroIdString.c_str(), L"RunOOBE", 1);
        const RegistryKeyChange<DWORD> defaultUid(lxssKey.get(), testDistroIdString.c_str(), L"DefaultUid", 0);

        auto validateOutput = [](LPCWSTR Cmd, LPCWSTR ExpectedOutput, LPCWSTR ExpectedWarnings = L"", DWORD ExpectedExitCode = 0) {
            auto [read, write] = CreateSubprocessPipe(true, false);
            write.reset();

            wsl::windows::common::SubProcess process(nullptr, LxssGenerateWslCommandLine(Cmd).c_str());
            process.SetStdHandles(read.get(), nullptr, nullptr);

            const auto output = process.RunAndCaptureOutput();

            VERIFY_ARE_EQUAL(ExpectedExitCode, output.ExitCode);

            VERIFY_ARE_EQUAL(ExpectedOutput, output.Stdout);
            VERIFY_ARE_EQUAL(ExpectedWarnings, output.Stderr);
        };

        {
            TerminateDistribution();

            // Non-interactive commands shouldn't trigger OOBE
            validateOutput(L"echo no oobe", L"no oobe\n");
            VERIFY_ARE_EQUAL(runOOBE.Get(), 1);

            // Interactive shell should trigger OOBE
            validateOutput(nullptr, L"OOBE\n");
            VERIFY_ARE_EQUAL(runOOBE.Get(), 0);

            // OOBE should only trigger once
            validateOutput(L"", L"");
        }

        {
            runOOBE.Set(1);
            distributionconf.SetContent(L"[oobe]\ncommand = /bin/bash -c 'echo failed OOBE && exit 1'\n");

            TerminateDistribution();

            constexpr auto expectedStdErr = L"OOBE command \"/bin/bash -c 'echo failed OOBE && exit 1'\" failed, exiting\n";

            validateOutput(nullptr, L"failed OOBE\n", expectedStdErr, 1);
            VERIFY_ARE_EQUAL(runOOBE.Get(), 1);

            // Failed OOBE command should be retried
            TerminateDistribution();
            validateOutput(nullptr, L"failed OOBE\n", expectedStdErr, 1);
            VERIFY_ARE_EQUAL(runOOBE.Get(), 1);
        }

        {
            runOOBE.Set(1);
            distributionconf.SetContent(
                L"[oobe]\ncommand = /bin/bash -c 'echo OOBE && useradd -u 1010 -m -s /bin/bash user'\n defaultUid = 1010\n");

            TerminateDistribution();

            validateOutput(nullptr, L"OOBE\n");
            VERIFY_ARE_EQUAL(runOOBE.Get(), 0);

            // Validate that DefaultUid was set
            validateOutput(L"id -u", L"1010\n");
            VERIFY_ARE_EQUAL(defaultUid.Get(), 1010);
        }

        // Verify that the default UID isn't changed if it's not present in wsl-distribution.conf.
        {
            runOOBE.Set(1);

            distributionconf.SetContent(L"[oobe]\ncommand = /bin/bash -c 'echo OOBE'");
            TerminateDistribution();

            validateOutput(nullptr, L"OOBE\n");
            VERIFY_ARE_EQUAL(defaultUid.Get(), 1010);
        }

        // Verify that OOBE doesn't run if a distribution is installed via wsl --import
        {
            constexpr auto testDir = L"test-oobe-import";
            constexpr auto testDistroName = L"test-oobe-import";

            std::filesystem::create_directory(testDir);
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() {
                LxsstuLaunchWsl(std::format(L"--unregister {}", testDistroName));
                std::error_code error;
                std::filesystem::remove_all(testDir, error);
            });

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export {} {}/exported.tar", LXSS_DISTRO_NAME_TEST_L, testDir)), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--import {} {} {}/exported.tar", testDistroName, testDir, testDistroName)), 0L);

            const auto distroKey = OpenDistributionKey(testDistroName);

            VERIFY_ARE_EQUAL(wsl::windows::common::registry::ReadDword(distroKey.get(), nullptr, L"RunOOBE", 1), 0);
            validateOutput(nullptr, L"");
        }

        // Make sure the defaultUid is reset for next test case.
        TerminateDistribution();
    }

    static void ValidateDistributionStarts(LPCWSTR Name)
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(std::format(L"-d {} echo -n OK", Name));
        VERIFY_ARE_EQUAL(out, L"OK");
    }

    TEST_METHOD(InstallWithBrokenDefault)
    {
        // This test case validates that a broken 'DefaultDistribution' value doesn't prevent installing new distributions.

        // Create a broken default
        RegistryKeyChange defaultDistro(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
            L"DefaultDistribution",
            std::wstring{L"{1DB260CB-912D-432A-B898-518DFD0F374E}"});

        // Validate that installing a new distribution succeeds.
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(L"--unregister test_new_default"); });

        VERIFY_ARE_EQUAL(
            LxsstuLaunchWsl(std::format(L"--install --from-file \"{}\" --no-launch --name test_new_default", g_testDistroPath)), 0L);

        auto [out, error] = LxsstuLaunchWslAndCaptureOutput(L"-d test_new_default echo OK");
        VERIFY_ARE_EQUAL(out, L"OK\n");
        VERIFY_ARE_EQUAL(error, L"");

        // Verify that the default distribution is updated
        const auto key = wsl::windows::common::registry::OpenLxssUserKey();

        const auto defaultValue = wsl::windows::common::registry::ReadString(key.get(), nullptr, L"DefaultDistribution");

        VERIFY_ARE_EQUAL(GetDistributionId(L"test_new_default").value_or(GUID_NULL), wsl::shared::string::ToGuid(defaultValue));
    }

    TEST_METHOD(ModernInstall)
    {
        using namespace wsl::windows::common::wslutil;
        using namespace wsl::windows::common::string;
        constexpr auto IconPath = L"test-icon.ico";

        auto CreateTarFromManifest = [](LPCWSTR Manifest, LPCWSTR TarName) {
            DistroFileChange distributionconf(L"/etc/wsl-distribution.conf", false);
            distributionconf.SetContent(Manifest);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export test_distro {}", TarName)), 0L);
        };

        auto InstallFromTar =
            [](LPCWSTR TarName, LPCWSTR ExtraArgs = L"", int ExpectedExitCode = 0, LPCWSTR ExpectedOutput = nullptr, LPCWSTR ExpextedWarnings = nullptr) {
                auto [out, err] = LxsstuLaunchWslAndCaptureOutput(
                    std::format(L"--install --no-launch --from-file {} {}", TarName, ExtraArgs), ExpectedExitCode);

                if (ExpectedOutput != nullptr)
                {
                    VERIFY_ARE_EQUAL(ExpectedOutput, out);
                }

                if (ExpextedWarnings != nullptr)
                {
                    VERIFY_ARE_EQUAL(ExpextedWarnings, err);
                }
            };

        auto installLocation = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installLocation.has_value());

        auto wslExePath = installLocation.value() + L"wsl.exe";

        wil::unique_hmodule wslExe{LoadLibrary(wslExePath.c_str())};
        VERIFY_IS_TRUE(!!wslExe);

        auto resource = FindResource(wslExe.get(), MAKEINTRESOURCE(1), RT_ICON);
        VERIFY_IS_TRUE(resource != nullptr);

        auto loadedResource = LoadResource(wslExe.get(), resource);
        const void* iconAddress = LockResource(loadedResource);

        wil::unique_handle icon{CreateFile(IconPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
        VERIFY_IS_TRUE(!!icon);

        DWORD bytes{};
        VERIFY_IS_TRUE(WriteFile(icon.get(), iconAddress, SizeofResource(wslExe.get(), resource), &bytes, nullptr));
        LogInfo("Created icon %ls (%lu bytes)", IconPath, bytes);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"cp '{}' /icon.ico", IconPath)), 0L);

        // Distribution with default name and icon
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                LxsstuLaunchWsl(L"--unregister test-default-name");
                DeleteFile(L"distro-default-name-icon.tar");
            });

            CreateTarFromManifest(
                L"[shortcut]\nicon = /icon.ico\n[oobe]\ndefaultName = test-default-name", L"distro-default-name-icon.tar");

            //
            // Validate that the distribution icon path is also correct when installing via wsl --import.
            //

            {
                constexpr auto distroName = L"TestCustomLocation";

                auto currentDirectory = std::filesystem::absolute(std::filesystem::current_path()).wstring();
                for (const auto& location : {currentDirectory, std::wstring(L".")})
                {
                    auto cleanup = wil::scope_exit_log(
                        WI_DIAGNOSTICS_INFO, [&]() { LxsstuLaunchWsl(std::format(L"--unregister {}", distroName)); });

                    VERIFY_ARE_EQUAL(
                        LxsstuLaunchWsl(
                            std::format(L"--import {} \"{}\" {}", distroName, location, "distro-default-name-icon.tar")),
                        0L);

                    auto [json, profile_path] = ValidateDistributionTerminalProfile(distroName, false);
                    VERIFY_ARE_EQUAL(
                        json["profiles"][1]["icon"].get<std::string>(), (std::filesystem::absolute(".") / "shortcut.ico").string());
                }
            }

            InstallFromTar(L"distro-default-name-icon.tar");
            ValidateDistributionStarts(L"test-default-name");

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(L"test-default-name");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            ValidateDistributionShortcut(L"test-default-name", icon.get());
            auto [json, profile_path] = ValidateDistributionTerminalProfile(L"test-default-name", false);

            VERIFY_IS_TRUE(std::filesystem::exists(profile_path));
            cleanup.reset();

            // Terminal profile should be removed when the distribution is unregistered.
            VERIFY_IS_FALSE(std::filesystem::exists(profile_path));

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution with default name and no icon
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                LxsstuLaunchWsl(L"--unregister test-default-name");
                DeleteFile(L"distro-default-name-no-icon.tar");
            });

            CreateTarFromManifest(L"\n[oobe]\ndefaultName = test-default-name", L"distro-default-name-no-icon.tar");
            InstallFromTar(L"distro-default-name-no-icon.tar");
            ValidateDistributionStarts(L"test-default-name");

            // Validate that the distribution was installed under the right name and icon
            auto distroKey = OpenDistributionKey(L"test-default-name");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));
            ValidateDistributionShortcut(L"test-default-name", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution with no default name
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                LxsstuLaunchWsl(L"--unregister test-distro-no-default-name");
                DeleteFile(L"distro-no-default-name.tar");
            });

            CreateTarFromManifest(L"", L"distro-no-default-name.tar");

            // Import should fail without --name
            constexpr auto expectedOutput =
                L"Installing: distro-no-default-name.tar\r\n\
This distribution doesn't contain a default name. Use --name to chose the distribution name.\r\n\
Error code: Wsl/Service/RegisterDistro/WSL_E_DISTRIBUTION_NAME_NEEDED\r\n";

            InstallFromTar(L"distro-no-default-name.tar", L"", -1, expectedOutput);

            // And suceed with --name
            InstallFromTar(L"distro-no-default-name.tar", L"--name test-distro-no-default-name");
            ValidateDistributionStarts(L"test-distro-no-default-name");

            auto distroKey = OpenDistributionKey(L"test-distro-no-default-name");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));
            ValidateDistributionShortcut(L"test-distro-no-default-name", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution specifying a VHD size.
        auto InstallWithVhdSize = [&](bool FixedVhd) {
            constexpr auto distroName = L"distro-vhd-size";
            constexpr auto tarFileName = L"distro-vhd-size.tar";
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
                DeleteFile(tarFileName);
            });

            CreateTarFromManifest(std::format(L"[shortcut]\nicon = /icon.ico\n[oobe]\ndefaultName = {}", distroName).c_str(), tarFileName);

            InstallFromTar(tarFileName, std::format(L"--vhd-size 1GB {}", FixedVhd ? L"--fixed-vhd" : L"").c_str());
            ValidateDistributionStarts(distroName);

            // Terminate the VM to make sure the VHD is not in use.
            WslShutdown();

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            ValidateDistributionShortcut(distroName, icon.get());
            auto [json, profile_path] = ValidateDistributionTerminalProfile(distroName, false);

            VERIFY_IS_TRUE(std::filesystem::exists(profile_path));

            // Verify that the is the correct type.
            {
                std::filesystem::path vhdFilePath = std::filesystem::path(basePath) / LXSS_VM_MODE_VHD_NAME;
                VIRTUAL_STORAGE_TYPE storageType{};
                storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
                storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;
                wil::unique_handle disk;
                THROW_IF_WIN32_ERROR(OpenVirtualDisk(
                    &storageType, vhdFilePath.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE, nullptr, &disk));

                GET_VIRTUAL_DISK_INFO diskInfo{};
                diskInfo.Version = GET_VIRTUAL_DISK_INFO_VIRTUAL_STORAGE_TYPE;
                ULONG diskInfoSize = sizeof(diskInfo);
                THROW_IF_WIN32_ERROR(GetVirtualDiskInformation(disk.get(), &diskInfoSize, &diskInfo, nullptr));

                VERIFY_IS_TRUE(diskInfo.VirtualStorageType.DeviceId == VIRTUAL_STORAGE_TYPE_DEVICE_VHDX);

                diskInfo.Version = GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE;
                diskInfoSize = sizeof(diskInfo);
                THROW_IF_WIN32_ERROR(GetVirtualDiskInformation(disk.get(), &diskInfoSize, &diskInfo, nullptr));

                VERIFY_ARE_EQUAL(FixedVhd, diskInfo.ProviderSubtype == 2);
            }

            // Unregister the distribution.
            cleanup.reset();

            // Terminal profile should be removed when the distribution is unregistered.
            VERIFY_IS_FALSE(std::filesystem::exists(profile_path));

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        };

        InstallWithVhdSize(false);
        InstallWithVhdSize(true);

        // Distribution imported in place
        if (LxsstuVmMode())
        {
            auto CreateVhdFromManifest = [](LPCWSTR Manifest, LPCWSTR VhdName) {
                DistroFileChange distributionconf(L"/etc/wsl-distribution.conf", false);
                distributionconf.SetContent(Manifest);
                WslShutdown();
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export test_distro {} --format vhd", VhdName)), 0L);
            };

            auto InstallFromVhd =
                [](LPCWSTR DistroName, LPCWSTR VhdName, int ExpectedExitCode = 0, LPCWSTR ExpectedOutput = nullptr, LPCWSTR ExpextedWarnings = nullptr) {
                    auto [out, err] =
                        LxsstuLaunchWslAndCaptureOutput(std::format(L"--import-in-place {} {}", DistroName, VhdName), ExpectedExitCode);

                    if (ExpectedOutput != nullptr)
                    {
                        VERIFY_ARE_EQUAL(ExpectedOutput, out);
                    }

                    if (ExpextedWarnings != nullptr)
                    {
                        VERIFY_ARE_EQUAL(ExpextedWarnings, err);
                    }
                };

            const auto distroName = L"distro-import-in-place";
            const auto vhdName = L"distro-import-in-place.vhdx";
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName).c_str());
                DeleteFileW(vhdName);
            });

            CreateVhdFromManifest(L"", vhdName);

            InstallFromVhd(distroName, vhdName);
            ValidateDistributionStarts(distroName);

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));
            ValidateDistributionShortcut(distroName, nullptr);
            auto [json, profile_path] = ValidateDistributionTerminalProfile(distroName, true);

            VERIFY_IS_TRUE(std::filesystem::exists(profile_path));
            cleanup.reset();

            // Terminal profile should be removed when the distribution is unregistered.
            VERIFY_IS_FALSE(std::filesystem::exists(profile_path));

            // Validate that the shortcut is gone
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
        }

        // Distribution with overriden default location
        {
            auto cleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(L"--unregister test-overriden-default-location"); });

            auto currentPath = std::filesystem::current_path();
            WslConfigChange wslconfig(std::format(L"[general]\ndistributionInstallPath = {}", EscapePath(currentPath.wstring())));

            InstallFromTar(g_testDistroPath.c_str(), L"--name test-overriden-default-location");
            ValidateDistributionStarts(L"test-overriden-default-location");

            auto distroKey = OpenDistributionKey(L"test-overriden-default-location");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            // Validate that the distribution was created in the correct path
            VERIFY_ARE_EQUAL(std::filesystem::path(basePath).parent_path().string(), currentPath.string());

            ValidateDistributionShortcut(L"test-overriden-default-location", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution installed in a custom location

        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(L"--unregister test-custom-location"); });

            InstallFromTar(g_testDistroPath.c_str(), L"--name test-custom-location --location test-distro-folder");
            ValidateDistributionStarts(L"test-custom-location");

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(L"test-custom-location");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));
            VERIFY_ARE_EQUAL(std::filesystem::absolute("test-distro-folder").wstring(), basePath);

            ValidateDistributionShortcut(L"test-custom-location", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution installed from stdin
        {

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(L"--unregister test-install-stdin"); });

            wil::unique_handle importTar{
                CreateFile(g_testDistroPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, HANDLE_FLAG_INHERIT, nullptr)};

            VERIFY_IS_TRUE(!!importTar);

            VERIFY_IS_TRUE(SetHandleInformation(importTar.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--install --no-launch --from-file - --name test-install-stdin", importTar.get()), 0L);

            ValidateDistributionStarts(L"test-install-stdin");

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(L"test-install-stdin");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            ValidateDistributionShortcut(L"test-install-stdin", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution default name conflicts with already installed distribution
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(L"conflict.tar"); });

            CreateTarFromManifest(L"[oobe]\ndefaultName = test_distro", L"conflict.tar");

            constexpr auto expectedOutput =
                L"Installing: conflict.tar\r\n\
A distribution with the supplied name already exists. Use --name to chose a different name.\r\n\
Error code: Wsl/Service/RegisterDistro/ERROR_ALREADY_EXISTS\r\n";

            InstallFromTar(L"conflict.tar", L"", -1, expectedOutput);
        }

        // Distribution default name is invalid
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(L"invalid.tar"); });

            CreateTarFromManifest(L"[oobe]\ndefaultName = invalid!", L"invalid.tar");

            constexpr auto expectedOutput =
                L"Installing: invalid.tar\r\n\
Invalid distribution name: \"invalid!\".\r\n\
Error code: Wsl/Service/RegisterDistro/E_INVALIDARG\r\n";

            InstallFromTar(L"invalid.tar", L"", -1, expectedOutput);
        }

        // Distribution icon file is too big
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(L"big-icon.tar");
                LxsstuLaunchWsl(L"--unregister big-icon");
            });

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"fallocate /icon.ico -l 20MB"), 0L);

            CreateTarFromManifest(L"[shortcut]\nicon = /icon.ico", L"big-icon.tar");

            WslKeepAlive keepAlive;
            InstallFromTar(L"big-icon.tar", L"--name big-icon");
            ValidateDistributionStarts(L"big-icon");

            if (LxsstuVmMode())
            {
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"dmesg | grep -iz 'File.*is too big' > /dev/null"), 0L);
            }

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(L"big-icon");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            ValidateDistributionShortcut(L"big-icon", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution icon file doesn't exist
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(L"icon-not-found.tar");
                LxsstuLaunchWsl(L"--unregister icon-not-found");
            });

            CreateTarFromManifest(L"[shortcut]\nicon = /does-not-exist.ico", L"icon-not-found.tar");

            InstallFromTar(L"icon-not-found.tar", L"--name icon-not-found");
            ValidateDistributionStarts(L"icon-not-found");

            // Validate that the distribution was installed under the right name
            auto distroKey = OpenDistributionKey(L"icon-not-found");
            VERIFY_IS_TRUE(!!distroKey);

            auto shortcutPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L"");
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");

            VERIFY_IS_TRUE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_TRUE(std::filesystem::exists(basePath));

            ValidateDistributionShortcut(L"icon-not-found", nullptr);

            cleanup.reset();

            // Validate that the base path is removed and that the shortcut is gone*
            VERIFY_IS_FALSE(std::filesystem::exists(shortcutPath));
            VERIFY_IS_FALSE(std::filesystem::exists(basePath));
        }

        // Distribution with a custom terminal profile
        {
            constexpr auto distroName = L"custom-terminal-profile";
            constexpr auto tarName = L"custom-terminal-profile.tar";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(tarName);
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
            });

            DistroFileChange profileTemplate(L"/terminal.json", false);

            constexpr auto templateContent =
                LR"(
            {
                "profiles": [{"custom-field": "custom-value"}],
                "schemes": [{"name": "my-scheme"}]
            })";

            profileTemplate.SetContent(templateContent);

            CreateTarFromManifest(L"[windowsterminal]\nprofileTemplate = /terminal.json", tarName);

            InstallFromTar(tarName, std::format(L"--name {}", distroName).c_str());
            ValidateDistributionStarts(distroName);

            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");
            auto [json, profile_path] = ValidateDistributionTerminalProfile(distroName, true);

            VERIFY_ARE_EQUAL(json["profiles"][1]["custom-field"].get<std::string>(), "custom-value");
            VERIFY_ARE_EQUAL(json["schemes"][0]["name"].get<std::string>(), "my-scheme");

            VERIFY_IS_TRUE(std::filesystem::exists(profile_path));
            cleanup.reset();

            // Terminal profile should be removed when the distribution is unregistered.
            VERIFY_IS_FALSE(std::filesystem::exists(profile_path));
        }

        // Distribution with an invalid terminal profile json
        {
            constexpr auto distroName = L"custom-terminal-profile-bad-json";
            constexpr auto tarName = L"custom-terminal-profile-bad-json.tar";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(tarName);
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
            });

            DistroFileChange profileTemplate(L"/terminal.json", false);
            profileTemplate.SetContent(L"bad-json");

            CreateTarFromManifest(L"[windowsterminal]\nprofileTemplate = /terminal.json", tarName);

            // Validate the invalid json blob generates a warning.
            InstallFromTar(
                tarName,
                std::format(L"--name {}", distroName).c_str(),
                0,
                nullptr,
                L"wsl: Failed to parse terminal profile while registering distribution: [json.exception.parse_error.101] "
                L"parse "
                L"error at line 1, column 1: syntax error while parsing value - invalid literal; last read: 'b'\r\n");

            ValidateDistributionStarts(distroName);
        }

        // Distribution with a a pre-existing hide profile.
        {
            constexpr auto distroName = L"custom-terminal-profile-hide";
            constexpr auto tarName = L"custom-terminal-profile-hide.tar";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(tarName);
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
            });

            auto profileGuid = wsl::shared::string::GuidToString<wchar_t>(
                CreateV5Uuid(GeneratedProfilesTerminalNamespace, std::as_bytes(std::span{std::wstring_view{distroName}})));

            auto content = std::format(
                LR"({{"profiles": [{{ "updates": "{}", "hidden": true, "custom": true}}, {{"name": "my-profile"}}]}})", profileGuid);

            DistroFileChange profileTemplate(L"/terminal.json", false);
            profileTemplate.SetContent(content.c_str());

            CreateTarFromManifest(L"[windowsterminal]\nprofileTemplate = /terminal.json", tarName);
            InstallFromTar(tarName, std::format(L"--name {}", distroName).c_str());

            ValidateDistributionStarts(distroName);

            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            // Validate that the default terminal profile is still generated.
            auto basePath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath", L"");
            auto [json, profile_path] = ValidateDistributionTerminalProfile(distroName, true);
            VERIFY_ARE_EQUAL(json["profiles"][0]["custom"].get<bool>(), true);
            VERIFY_ARE_EQUAL(json["profiles"].size(), 2);

            VERIFY_IS_TRUE(std::filesystem::exists(profile_path));

            VERIFY_ARE_EQUAL(
                profile_path, wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"TerminalProfilePath", L""));

            cleanup.reset();

            // Terminal profile should be removed when the distribution is unregistered.
            VERIFY_IS_FALSE(std::filesystem::exists(profile_path));
        }

        // Distribution opting-out of terminal profile generation
        {
            constexpr auto distroName = L"no-terminal-profile";
            constexpr auto tarName = L"no-terminal-profile.tar";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(tarName);
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
            });

            CreateTarFromManifest(L"[windowsterminal]\nenabled = false", tarName);

            InstallFromTar(tarName, std::format(L"--name {}", distroName).c_str());

            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            // Validate that no terminal profile is generated.
            VERIFY_ARE_EQUAL(
                L"", wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"TerminalProfilePath", L""));
        }

        // Distribution opting-out of shortcut generation
        {
            constexpr auto distroName = L"no-shortcut";
            constexpr auto tarName = L"no-shortcut.tar";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                DeleteFile(tarName);
                LxsstuLaunchWsl(std::format(L"--unregister {}", distroName));
            });

            CreateTarFromManifest(L"[shortcut]\nenabled = false", tarName);

            InstallFromTar(tarName, std::format(L"--name {}", distroName).c_str());

            auto distroKey = OpenDistributionKey(distroName);
            VERIFY_IS_TRUE(!!distroKey);

            // Validate that no terminal profile is generated.
            VERIFY_ARE_EQUAL(L"", wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"ShortcutPath", L""));
        }
    }

    static auto SetManifest(const std::string& Content, bool Append = false)
    {
        auto file = wsl::windows::common::filesystem::TempFile(GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING);
        VERIFY_IS_TRUE(WriteFile(file.Handle.get(), Content.c_str(), static_cast<DWORD>(Content.size()), nullptr, nullptr));

        RegistryKeyChange<std::wstring> manifestOverride{
            HKEY_LOCAL_MACHINE,
            LXSS_REGISTRY_PATH,
            Append ? wsl::windows::common::distribution::c_distroUrlAppendRegistryValue : wsl::windows::common::distribution::c_distroUrlRegistryValue,
            L"file://" + file.Path.wstring()};

        return std::make_pair(std::move(file), std::move(manifestOverride));
    }

    static void ValidateInstall(const std::wstring& cmd, LPCWSTR ExpectedOutput = nullptr)
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--install --no-launch {}", cmd));

        if (ExpectedOutput != nullptr)
        {
            VERIFY_ARE_EQUAL(ExpectedOutput, out);
        }
    }

    static void ValidateInstallError(
        const std::wstring& cmd, const std::wstring& expectedOutput, const std::wstring& expectedWarnings = L"")
    {
        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(cmd, -1);

        VERIFY_ARE_EQUAL(expectedOutput, out);
        VERIFY_ARE_EQUAL(expectedWarnings, err);
    }

    static void UnregisterDistribution(LPCWSTR Name)
    {
        LxsstuLaunchWsl(std::format(L"--unregister {}", Name));
    }

    TEST_METHOD(FileUrl)
    {
        auto check = [](LPCWSTR Input, const std::optional<std::filesystem::path>& ExpectedOutput) {
            const auto output = wsl::windows::common::filesystem::TryGetPathFromFileUrl(Input);

            VERIFY_ARE_EQUAL(output.has_value(), ExpectedOutput.has_value());

            if (output.has_value())
            {
                VERIFY_ARE_EQUAL(output.value(), ExpectedOutput.value());
            }
        };

        check(L"file://C:/File", L"C:\\File");
        check(L"file://C:\\File", L"C:\\File");
        check(L"file:///C:\\File", L"C:\\File");
        check(L"file:///RelativeFile", L"RelativeFile");
        check(L"file:///RelativeFile\\SubPath/SubPath", L"RelativeFile\\SubPath\\SubPath");
        check(L"notfile:///C:\\File", {});
    }

    TEST_METHOD(MacAddressParsing)
    {
        using namespace wsl::shared::string;
        auto testParse = [](const std::wstring& Input, const std::optional<MacAddress>& ExpectedOutput, char separator = '\0') {
            const auto result = wsl::shared::string::ParseMacAddressNoThrow<wchar_t>(Input, separator);

            VERIFY_ARE_EQUAL(result.has_value(), ExpectedOutput.has_value());
            if (result.has_value())
            {
                VERIFY_ARE_EQUAL(ExpectedOutput.value(), result.value());
            }
        };

        testParse(L"", {});
        testParse(L"-", {});
        testParse(L"00:00:00:00:00:0", {});
        testParse(L"00::00:00:00:00:00", {});
        testParse(L"000:00:00:00:00:00", {});
        testParse(L"000:00:00:00:00:0g", {});
        testParse(L"00:00:00:00:00:00", {{0, 0, 0, 0, 0, 0}});
        testParse(L"01:23:45:67:89:AB", {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}});
        testParse(L"01-23-45-67-89-AB", {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}});
        testParse(L"01-23-45-67-89-AB", {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}}, '-');
        testParse(L"01-23-45-67-89-AB", {}, ':');
        testParse(L"01-23-45-67-89:AB", {});
        testParse(L"01,23,45,67,89,AB", {});

        VERIFY_ARE_EQUAL(wsl::shared::string::FormatMacAddress({0x01, 0x23, 0x45, 0x67, 0x89, 0xab}, '-'), "01-23-45-67-89-AB");
        VERIFY_ARE_EQUAL(wsl::shared::string::FormatMacAddress({0x01, 0x23, 0x45, 0x67, 0x89, 0xab}, ':'), "01:23:45:67:89:AB");

        VERIFY_ARE_EQUAL(
            wsl::shared::string::FormatMacAddress({0x01, 0x23, 0x45, 0x67, 0x89, 0xab}, L'-'),
            wsl::shared::string::MultiByteToWide("01-23-45-67-89-AB"));
        VERIFY_ARE_EQUAL(
            wsl::shared::string::FormatMacAddress({0x01, 0x23, 0x45, 0x67, 0x89, 0xab}, L':'),
            wsl::shared::string::MultiByteToWide("01:23:45:67:89:AB"));
    }

    TEST_METHOD(ModernDistroInstall)
    {
        auto tarPath = "file://" + wsl::shared::string::WideToMultiByte(EscapePath(g_testDistroPath));

        wil::unique_handle tarHandle{CreateFile(g_testDistroPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!tarHandle);

        auto tarHash = wsl::shared::string::WideToMultiByte(
            wsl::windows::common::string::BytesToHex(wsl::windows::common::wslutil::HashFile(tarHandle.get(), CALG_SHA_256)));

        // Install a modern distribution
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Default": true,
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ]
    }}}})",
                tarPath,
                tarHash);

            auto restore = SetManifest(manifest);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { UnregisterDistribution(L"debian-12"); });

            ValidateInstall(L"debian --no-launch --name debian-12");

            ValidateDistributionStarts(L"debian-12");

            UnregisterDistribution(L"debian-12");

            ValidateInstall(L"debian-12 --no-launch --name debian-12");
            ValidateDistributionStarts(L"debian-12");

            ValidateInstallError(
                L"--install DoesNotExists",
                L"Invalid distribution name: 'DoesNotExists'.\r\n\
To get a list of valid distributions, use 'wsl.exe --list --online'.\r\n\
Error code: Wsl/InstallDistro/WSL_E_DISTRO_NOT_FOUND\r\n");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister debian-12"), 0L);

            // Verify that name matching is not case sensitive on the version.
            ValidateInstall(L"Debian-12 --no-launch --name debian-12");
            ValidateDistributionStarts(L"debian-12");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister debian-12"), 0L);

            // Verify that name matching is not case sensitive on the flavor.
            ValidateInstall(L"Debian --no-launch --name debian-12");
            ValidateDistributionStarts(L"debian-12");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister debian-12"), 0L);

            // Validate an install with a vhd size.
            ValidateInstall(L"Debian --no-launch --name debian-12 --vhd-size 1GB");
            ValidateDistributionStarts(L"debian-12");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister debian-12"), 0L);

            // Validate an install with a vhd size and fixed vhd.
            ValidateInstall(L"Debian --no-launch --name debian-12 --vhd-size 1GB --fixed-vhd");
            ValidateDistributionStarts(L"debian-12");
        }

        // Validate that default works correctly
        {
            auto manifest = std::format(
                R"({{
    "Default": "debian",
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-nondefault",
                "FriendlyName": "",
                "Amd64Url": {{
                    "Url": "",
                    "Sha256": ""
                }}
            }},
            {{
                "Name": "debian-default",
                "FriendlyName": "DebianFriendlyName",
                "Default": true,
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ],

 "ubuntu": [
            {{
                "Name": "ubuntu-nondefault",
                "FriendlyName": "",
                "Amd64Url": {{
                    "Url": "",
                    "Sha256": ""
                }}
            }},
            {{
                "Name": "ubuntu-default",
                "FriendlyName": "UbuntuFriendlyName",
                "Default": true,
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ]
    }}}})",
                tarPath,
                tarHash,
                tarPath,
                tarHash);

            auto restore = SetManifest(manifest);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                UnregisterDistribution(L"debian-default");
                UnregisterDistribution(L"ubuntu-default");
            });

            ValidateInstall(
                L"--no-launch --name debian-default",
                L"Installing: DebianFriendlyName\r\n\
Distribution successfully installed. It can be launched via 'wsl.exe -d debian-default'\r\n");

            ValidateDistributionStarts(L"debian-default");

            ValidateInstall(
                L"ubuntu --no-launch --name ubuntu-default",
                L"Installing: UbuntuFriendlyName\r\n\
Distribution successfully installed. It can be launched via 'wsl.exe -d ubuntu-default'\r\n");

            ValidateDistributionStarts(L"ubuntu-default");

            // Validate that default can be override via the 'Append' manifest
            auto overrideRestore = SetManifest(R"({"Default": "ubuntu"})", true);

            UnregisterDistribution(L"ubuntu-default");

            ValidateInstall(
                L"--no-launch --name ubuntu-default",
                L"Installing: UbuntuFriendlyName\r\n\
Distribution successfully installed. It can be launched via 'wsl.exe -d ubuntu-default'\r\n");

            ValidateDistributionStarts(L"ubuntu-default");
        }

        // Install a legacy distribution
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "",
                    "Sha256": ""
                }}
            }}
        ]
    }},
    "Distributions": [
        {{"Name": "legacy",
          "FriendlyName": "legacy",
          "StoreAppId": "Dummy",
          "PackageFamilyName": "Dummy",
          "Amd64": true,
          "Arm64": true,
          "Amd64PackageUrl": "http://127.0.0.1:12/dummyUrl" }}]
}})",
                tarPath);

            auto restore = SetManifest(manifest);

            // There's no easy way to automate the appx package installation, but verify that we take the legacy path
            ValidateInstallError(
                L"--install legacy --no-launch --web-download",
                L"Downloading: legacy\r\n\
A connection with the server could not be established \r\n\
Error code: Wsl/InstallDistro/WININET_E_CANNOT_CONNECT\r\n",
                L"wsl: Using legacy distribution registration. Consider using a tar based distribution instead.\r\n");

            ValidateInstallError(
                L"--install legacy --no-launch --web-download --legacy",
                L"Downloading: legacy\r\n\
A connection with the server could not be established \r\n\
Error code: Wsl/InstallDistro/WININET_E_CANNOT_CONNECT\r\n",
                L"wsl: Using legacy distribution registration. Consider using a tar based distribution instead.\r\n");
        }

        // Validate that modern distros takes precedences, but can be overriden.
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ]
    }},
    "Distributions": [
        {{"Name": "debian-12",
          "FriendlyName": "debian-12",
          "StoreAppId": "Dummy",
          "PackageFamilyName": "Dummy",
          "Amd64": true,
          "Arm64": true,
          "Amd64PackageUrl": "http://127.0.0.1:12/dummyUrl" }}]
}})",
                tarPath,
                tarHash);

            auto restore = SetManifest(manifest);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { UnregisterDistribution(L"debian-12"); });

            ValidateInstall(L"debian-12 --no-launch --name debian-12");
            ValidateDistributionStarts(L"debian-12");

            // Validate that --legacy takes the appx path.
            ValidateInstallError(
                L"--install debian-12 --no-launch --web-download --legacy",
                L"Downloading: debian-12\r\n\
A connection with the server could not be established \r\n\
Error code: Wsl/InstallDistro/WININET_E_CANNOT_CONNECT\r\n",
                L"wsl: Using legacy distribution registration. Consider using a tar based distribution instead.\r\n");
        }

        // Validate that distribution can be overriden
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }},
            {{
                "Name": "debian-base",
                "FriendlyName": "DebianFriendlyName",
                "Default": true,
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": ""
                }}
            }}
        ]
    }},
    "Distributions": [{{"Name": "Dummy", "FriendlyName": "Dummy", "StoreAppId": "Dummy", "Amd64": true, "Arm64": true }}]
}})",
                "DoesNotExist",
                tarPath,
                tarHash);

            auto overrideManifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyNameOverriden",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ]
    }}
}})",
                tarPath,
                tarHash);

            auto restore = SetManifest(manifest);
            auto override = SetManifest(overrideManifest, true);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
                UnregisterDistribution(L"debian-12");
                UnregisterDistribution(L"debian-base");
            });

            ValidateInstall(L"debian-12 --no-launch --name debian-12");

            // Validate that distros coming from the 'main' manifest can still be installed.
            ValidateInstall(L"debian-12 --no-launch --name debian-base");
        }

        // Validate that the distribution default name comes from the manifest, event if oobe.defaultName isn't set
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "test-default-manifest-name",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "{}"
                }}
            }}
        ]
    }}
}})",
                tarPath,
                tarHash);

            auto restore = SetManifest(manifest);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { UnregisterDistribution(L"test-default-manifest-name"); });

            ValidateInstall(L"test-default-manifest-name");
            ValidateDistributionStarts(L"test-default-manifest-name");
        }

        // Validate that install fails if hash doesn't match
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "0x12"
                }}
            }}
        ]
    }}
}})",
                tarPath);

            auto restore = SetManifest(manifest);

            ValidateInstallError(
                L"--install debian-12",
                std::format(
                    L"Installing: DebianFriendlyName\r\n\
The distribution hash doesn't match. Expected: 0x12, actual hash: {}\r\n\
Error code: Wsl/InstallDistro/VerifyChecksum/TRUST_E_BAD_DIGEST\r\n",
                    wsl::shared::string::MultiByteToWide(tarHash)),
                L"");
        }

        // Validate that we fail if the hash format is incorrect
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "{}",
                    "Sha256": "wrongformat"
                }}
            }}
        ]
    }}
}})",
                tarPath);

            auto restore = SetManifest(manifest);

            ValidateInstallError(
                L"--install debian-12",
                L"Installing: DebianFriendlyName\r\n\
Invalid hex string: wrongformat\r\n\
Error code: Wsl/InstallDistro/VerifyChecksum/E_INVALIDARG\r\n",
                L"");
        }

        // Validate various command line error paths
        {
            auto manifest =
                R"({
    "Distributions": [
        {"Name": "debian-12",
          "FriendlyName": "debian-12",
          "StoreAppId": "Dummy",
          "PackageFamilyName": "Dummy",
          "Amd64": true,
          "Arm64": true,
          "Amd64PackageUrl": "" }]
})";

            auto restore = SetManifest(manifest);

            ValidateInstallError(
                L"--install debian-12 --location foo",
                L"'--location' is not supported when installing legacy distributions.\r\n",
                L"");

            ValidateInstallError(
                L"--install debian-12 --name foo", L"'--name' is not supported when installing legacy distributions.\r\n", L"");

            ValidateInstallError(
                L"--install debian-12 --vhd-size 1GB",
                L"'--vhd-size' is not supported when installing legacy distributions.\r\n",
                L"");

            ValidateInstallError(
                L"--install invalid",
                L"Invalid distribution name: 'invalid'.\r\n\
To get a list of valid distributions, use 'wsl.exe --list --online'.\r\n\
Error code: Wsl/InstallDistro/WSL_E_DISTRO_NOT_FOUND\r\n",
                L"");
        }

        // Validate that a distribution isn't downloaded if its name is already in use.
        {
            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "debian": [
            {{
                "Name": "{}",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {{
                    "Url": "file://doesnotexist",
                    "Sha256": ""
                }}
            }},
            {{
                "Name": "dummy",
                "FriendlyName": "dummy",
                "Amd64Url": {{
                    "Url": "file://doesnotexist",
                    "Sha256": ""
                }}
            }}
        ]
    }}
}})",
                LXSS_DISTRO_NAME_TEST);

            auto restore = SetManifest(manifest);

            {
                auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--install {}", LXSS_DISTRO_NAME_TEST_L), -1);

                VERIFY_ARE_EQUAL(
                    out,
                    L"A distribution with the supplied name already exists. Use --name to chose a different name.\r\n"
                    L"Error code: Wsl/InstallDistro/ERROR_ALREADY_EXISTS\r\n");

                VERIFY_ARE_EQUAL(err, L"");
            }

            {
                auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--install dummy --name {}", LXSS_DISTRO_NAME_TEST_L), -1);

                VERIFY_ARE_EQUAL(
                    out,
                    L"A distribution with the supplied name already exists. Use --name to chose a different name.\r\n"
                    L"Error code: Wsl/InstallDistro/ERROR_ALREADY_EXISTS\r\n");

                VERIFY_ARE_EQUAL(err, L"");
            }
        }

        // Validate handling of case where no default install distro is configured.
        {
            auto manifest =
                R"({
    "ModernDistributions": {
        "debian": [
            {
                "Name": "debian-12",
                "FriendlyName": "DebianFriendlyName",
                "Amd64Url": {
                    "Url": "",
                    "Sha256": ""
                }
            }
        ]
    }
})";

            auto restore = SetManifest(manifest);
            ValidateInstallError(
                L"--install",
                L"No default distribution has been configured. Please provide a distribution to install.\r\n\
Error code: Wsl/InstallDistro/E_UNEXPECTED\r\n",
                L"");
        }

        // Validate that invalid json errors are correctly handled.
        {
            auto restore = SetManifest("Bad json");

            ValidateInstallError(
                L"--install debian",
                L"Invalid JSON document. Parse error: [json.exception.parse_error.101] parse error at line 1, column 1: syntax error while parsing value - invalid literal; last read: 'B'\r\n\
Error code: Wsl/InstallDistro/WSL_E_INVALID_JSON\r\n",
                L"");
        }

        // Validate that url parameters are correctly handled.
        {
            constexpr auto tarEndpoint = L"http://127.0.0.1:6667/";

            UniqueWebServer fileServer(tarEndpoint, std::filesystem::path(g_testDistroPath));

            wil::unique_handle tarHandle{CreateFile(g_testDistroPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            VERIFY_IS_TRUE(!!tarHandle);

            auto manifest = std::format(
                R"({{
    "ModernDistributions": {{
        "test": [
            {{
                "Name": "test-url-download",
                "FriendlyName": "FriendlyName",
                "Default": true,
                "Amd64Url": {{
                    "Url": "{}/distro.tar?foo=bar&key=value",
                    "Sha256": "{}"
                }}
            }}
        ]
    }}}})",
                tarEndpoint,
                tarHash);

            auto restore = SetManifest(manifest);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { UnregisterDistribution(L"test-url-download"); });

            auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"--install --no-launch test-url-download");
            VERIFY_ARE_EQUAL(
                output,
                L"Downloading: FriendlyName\r\nInstalling: FriendlyName\r\nDistribution successfully installed. It can be "
                L"launched via 'wsl.exe -d test-url-download'\r\n");

            VERIFY_ARE_EQUAL(error, L"");
        }
    }

    TEST_METHOD(ModernInstallEndToEnd)
    {
        constexpr auto tarName = L"end2end.tar";

        DistroFileChange distributionconf(L"/etc/wsl-distribution.conf", false);
        distributionconf.SetContent(
            L"[oobe]\ncommand = /bin/bash -c 'echo OOBE && useradd -u 1011 -m -s /bin/bash myuser'\n defaultUid = 1011\n");

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--export test_distro {}", tarName)), 0L);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            DeleteFile(tarName);

            LxsstuLaunchWsl(L"--unregister end2end");
        });

        wil::unique_handle tarHandle{CreateFile(tarName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!tarHandle);

        auto tarHash = wsl::windows::common::string::BytesToHex(wsl::windows::common::wslutil::HashFile(tarHandle.get(), CALG_SHA_256));

        constexpr auto manifestEndpoint = L"http://127.0.0.1:6666/";
        constexpr auto tarEndpoint = L"http://127.0.0.1:6667/";

        auto manifest = std::format(
            LR"({{
    \"ModernDistributions\": {{
        \"end2end\": [
            {{
                \"Name\": \"end2end\",
                \"FriendlyName\": \"FriendlyName\",
                \"Default\": true,
                \"Amd64Url\": {{
                    \"Url\": \"{}/distro.tar\",
                    \"Sha256\": \"{}\"
                }}
            }}
        ]
    }}}})",
            tarEndpoint,
            tarHash);

        UniqueWebServer apiServer(manifestEndpoint, manifest.c_str());
        UniqueWebServer fileServer(tarEndpoint, std::filesystem::path(tarName));

        RegistryKeyChange<std::wstring> manifestOverride{
            HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, wsl::windows::common::distribution::c_distroUrlRegistryValue, manifestEndpoint};

        {
            auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"--install --no-launch end2end");
            VERIFY_ARE_EQUAL(
                output,
                L"Downloading: FriendlyName\r\nInstalling: FriendlyName\r\nDistribution successfully installed. It can be "
                L"launched via 'wsl.exe -d end2end'\r\n");

            VERIFY_ARE_EQUAL(error, L"");
        }

        // Check that OOBE runs
        {
            auto [read, write] = CreateSubprocessPipe(true, false);
            write.reset();

            wsl::windows::common::SubProcess process(nullptr, LxssGenerateWslCommandLine(L"-d end2end").c_str());
            process.SetStdHandles(read.get(), nullptr, nullptr);

            auto oobeResult = process.RunAndCaptureOutput();
            VERIFY_ARE_EQUAL(oobeResult.Stdout, L"OOBE\n");
            VERIFY_ARE_EQUAL(oobeResult.Stderr, L"");
            VERIFY_ARE_EQUAL(oobeResult.ExitCode, 0);
        }

        // Run the command again to check that oobe doesn't run twice
        {
            auto [read, write] = CreateSubprocessPipe(true, false);
            write.reset();

            wsl::windows::common::SubProcess process(nullptr, LxssGenerateWslCommandLine(L"-d end2end").c_str());
            process.SetStdHandles(read.get(), nullptr, nullptr);

            auto oobeResult = process.RunAndCaptureOutput();
            VERIFY_ARE_EQUAL(oobeResult.Stdout, L"");
            VERIFY_ARE_EQUAL(oobeResult.Stderr, L"");
            VERIFY_ARE_EQUAL(oobeResult.ExitCode, 0);
        }

        // Validate UID
        auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"-d end2end id -u");
        VERIFY_ARE_EQUAL(output, L"1011\n");
        VERIFY_ARE_EQUAL(error, L"");
    }

    TEST_METHOD(DistroTarFormats)
    {
        auto version = LxsstuVmMode() ? L"2" : L"1";

        auto convert = [](LPCWSTR Command, LPCWSTR FileName) {
            const wil::unique_handle output{CreateFile(FileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr)};
            VERIFY_IS_TRUE(!!output);

            wsl::windows::common::helpers::SetHandleInheritable(output.get());

            LxsstuLaunchWsl(std::format(L"xz -d -c $(wslpath '{}') | {}", g_testDistroPath, Command), nullptr, output.get());

            return wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [FileName]() { std::filesystem::remove(FileName); });
        };

        auto importAndTest = [&version](LPCWSTR FileName) {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [FileName]() { LxsstuLaunchWsl(L"--unregister test-format"); });
            LxsstuLaunchWsl(std::format(L"--install --no-launch --from-file {} --name test-format --version {}", FileName, version));

            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"-d test-format echo OK");
            VERIFY_ARE_EQUAL(out, L"OK\n");
        };

        // Tar bz2
        {
            auto cleanup = convert(L"bzip2", L"test-distro.tar.bz2");
            importAndTest(L"test-distro.tar.bz2");
        }

        // Tar gz
        {
            auto cleanup = convert(L"gzip", L"test-distro.tar.gz");
            importAndTest(L"test-distro.tar.gz");
        }

        // N.B. tar xz is already covered since it's the format of the test distro.
        VERIFY_IS_TRUE(wsl::shared::string::EndsWith(g_testDistroPath, std::wstring_view{L".xz"}));
    }

    TEST_METHOD(InnerCommandLineParsing)
    {
        using namespace wsl::windows::common;
        using namespace wsl::shared;

        constexpr auto entryPoint = L"dummy";

        auto parse = [&](ArgumentParser& Parser, LPCWSTR ExpectedError = nullptr) {
            const ExecutionContext context(Context::Wsl);
            std::optional<std::wstring> error;

            try
            {
                Parser.Parse();
            }
            catch (...)
            {
                if (context.ReportedError().has_value())
                {
                    error = wslutil::ErrorToString(context.ReportedError().value()).Message;
                }
                else
                {
                    error = wslutil::ErrorCodeToString(wil::ResultFromCaughtException());
                    THROW_HR(wil::ResultFromCaughtException());
                }
            }

            if (error.has_value())
            {
                VERIFY_ARE_EQUAL(ExpectedError, error.value());
            }
            else
            {
                VERIFY_IS_NULL(ExpectedError);
            }
        };

        {
            ArgumentParser parser(L"--a b --c d pos-value", entryPoint, 0);
            std::wstring a;
            std::wstring c;
            std::wstring e;
            std::wstring pos;
            parser.AddArgument(a, L"--a");
            parser.AddArgument(c, L"--c");
            parser.AddArgument(e, L"--e");
            parser.AddPositionalArgument(pos, 0);

            parse(parser);

            VERIFY_ARE_EQUAL(a, L"b");
            VERIFY_ARE_EQUAL(c, L"d");
            VERIFY_ARE_EQUAL(pos, L"pos-value");
            VERIFY_ARE_EQUAL(e, L"");
        }

        {
            ArgumentParser parser(L"--a b -- --c", entryPoint, 0);
            std::wstring a;
            std::wstring c;
            std::wstring e;
            std::wstring pos;
            parser.AddArgument(a, L"--a");
            parser.AddArgument(e, L"--e");
            parser.AddPositionalArgument(pos, 0);

            parse(parser);

            VERIFY_ARE_EQUAL(a, L"b");
            VERIFY_ARE_EQUAL(pos, L"--c");
            VERIFY_ARE_EQUAL(e, L"");
        }

        {
            GUID expectedGuid = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
            auto commandLine = std::format(
                L"--flag b --arg value pos-arg2 pos-arg3 --flag3 --flag4 value4 --guid {}",
                wsl::shared::string::GuidToString<wchar_t>(expectedGuid));

            ArgumentParser parser(commandLine.c_str(), entryPoint, 0);
            bool flag{};
            std::wstring arg;
            std::wstring pos1;
            std::wstring pos2;
            std::wstring pos3;
            bool flag3{};
            std::wstring value4;
            bool dummy{};
            GUID parsedGuid;

            parser.AddArgument(flag, L"--flag");
            parser.AddArgument(arg, L"--arg");
            parser.AddPositionalArgument(pos1, 0);
            parser.AddPositionalArgument(pos2, 1);
            parser.AddPositionalArgument(pos3, 2);
            parser.AddArgument(flag3, L"--flag3");
            parser.AddArgument(value4, L"--flag4");
            parser.AddArgument(dummy, L"--dummy");
            parser.AddArgument(parsedGuid, L"--guid");

            parse(parser);

            VERIFY_IS_TRUE(flag);
            VERIFY_ARE_EQUAL(arg, L"value");
            VERIFY_ARE_EQUAL(pos1, L"b");
            VERIFY_ARE_EQUAL(pos2, L"pos-arg2");
            VERIFY_ARE_EQUAL(pos3, L"pos-arg3");
            VERIFY_IS_TRUE(flag3);
            VERIFY_ARE_EQUAL(L"value4", value4);
            VERIFY_IS_FALSE(dummy);
            VERIFY_ARE_EQUAL(expectedGuid, parsedGuid);
        }

        {
            ArgumentParser parser(L"--a", entryPoint, 0);
            std::wstring a;
            parser.AddArgument(a, L"--a");

            parse(
                parser,
                std::format(
                    L"Command line argument --a requires a value.\n"
                    "Please use '{} --help' to get a list of supported arguments.",
                    entryPoint)
                    .c_str());
        }

        {
            ArgumentParser parser(L"--does-not-exist --a b -- --c", entryPoint, 0);
            parser.AddArgument(NoOp{}, L"--a");
            parser.AddArgument(NoOp{}, L"--e");
            parser.AddPositionalArgument(NoOp{}, 0);

            parse(
                parser,
                std::format(
                    L"Invalid command line argument: --does-not-exist\n"
                    "Please use '{} --help' to get a list of supported arguments.",
                    entryPoint)
                    .c_str());
        }

        {
            ArgumentParser parser(L"--guid foo", entryPoint, 0);
            GUID guid;
            parser.AddArgument(guid, L"--guid");

            parse(parser, L"Invalid GUID format: 'foo'");
        }

        {
            ArgumentParser parser(L"-abc pos-value", entryPoint, 0);
            bool aLong{};
            bool a{};
            bool b{};
            bool c{};
            bool d{};
            std::wstring pos;

            parser.AddArgument(aLong, L"--a");
            parser.AddArgument(a, nullptr, 'a');
            parser.AddArgument(b, nullptr, 'b');
            parser.AddArgument(c, nullptr, 'c');
            parser.AddArgument(d, nullptr, 'd');
            parser.AddPositionalArgument(pos, 0);

            parse(parser);

            VERIFY_IS_TRUE(a);
            VERIFY_IS_TRUE(b);
            VERIFY_IS_TRUE(c);
            VERIFY_IS_FALSE(d);
            VERIFY_IS_FALSE(aLong);
            VERIFY_ARE_EQUAL(pos, L"pos-value");
        }

        {
            ArgumentParser parser(L"-abc", entryPoint, 0);

            parser.AddArgument(NoOp{}, nullptr, 'a');
            parser.AddArgument(NoOp{}, nullptr, 'c');

            parse(
                parser,
                std::format(
                    L"Invalid command line argument: -abc\n"
                    "Please use '{} --help' to get a list of supported arguments.",
                    entryPoint)
                    .c_str());
        }

        {
            ArgumentParser parser(L"- --", entryPoint, 0);

            parse(
                parser,
                std::format(
                    L"Invalid command line argument: -\n"
                    "Please use '{} --help' to get a list of supported arguments.",
                    entryPoint)
                    .c_str());
        }

        {
            ArgumentParser parser(L"--foo -", entryPoint, 0);
            bool a{};
            std::wstring pos;

            parser.AddArgument(a, L"--foo");
            parser.AddPositionalArgument(pos, 0);

            parse(parser);
            VERIFY_IS_TRUE(a);
            VERIFY_ARE_EQUAL(pos, L"-");
        }

        {
            constexpr auto testDir = "wslpath-test-dir";
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { std::filesystem::remove_all(testDir); });

            std::filesystem::create_directory(testDir);

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"wslpath -aw {}", testDir));
            VERIFY_ARE_EQUAL((std::filesystem::canonical(std::filesystem::current_path()) / testDir).wstring() + L"\n", out);

            std::tie(out, err) = LxsstuLaunchWslAndCaptureOutput(std::format(L"wslpath -wa {}", testDir));
            VERIFY_ARE_EQUAL((std::filesystem::canonical(std::filesystem::current_path()) / testDir).wstring() + L"\n", out);

            std::tie(out, err) = LxsstuLaunchWslAndCaptureOutput(std::format(L"wslpath {}", testDir));
            VERIFY_ARE_EQUAL(std::format(L"{}\n", testDir), out);

            std::tie(out, err) = LxsstuLaunchWslAndCaptureOutput(std::format(L"wslpath -a {}", testDir));
            VERIFY_IS_TRUE(out.find(L"/mnt/") == 0);
        }
    }

    TEST_METHOD(CaseSensitivity)
    {
        auto setCaseSensitivity = [](const std::wstring& Path, bool enable) {
            auto cmd = std::format(L"fsutil.exe file setCaseSensitiveInfo \"{}\" {}", Path.c_str(), enable ? L"enable" : L"disable");
            LxsstuLaunchCommandAndCaptureOutput(cmd.data());
        };

        auto getCaseSensitivity = [](const std::wstring& Path) {
            auto cmd = std::format(L"fsutil.exe file queryCaseSensitiveInfo \"{}\"", Path);
            auto [out, _] = LxsstuLaunchCommandAndCaptureOutput(cmd.data());
            if (out.find(L"is disabled") != std::string::npos)
            {
                return false;
            }
            else if (out.find(L"is enabled") != std::string::npos)
            {
                return true;
            }

            LogError("Failed to parse fsutil output: %ls", out.c_str());
            VERIFY_FAIL();

            return true;
        };

        constexpr auto testDir = L"case-test";
        constexpr auto flags = wsl::windows::common::filesystem::c_case_sensitive_folders_only | LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { std::filesystem::remove_all(testDir); });

        std::filesystem::create_directories(testDir);
        setCaseSensitivity(testDir, false);
        VERIFY_IS_FALSE(getCaseSensitivity(testDir));

        wsl::windows::common::filesystem::EnsureCaseSensitiveDirectory(testDir, flags);
        VERIFY_IS_TRUE(getCaseSensitivity(testDir));
        setCaseSensitivity(testDir, false);

        std::filesystem::create_directories(std::format(L"{}/l1/l2/l3", testDir));
        setCaseSensitivity(std::format(L"{}/l1/l2/l3", testDir), false);
        setCaseSensitivity(std::format(L"{}/l1/l2", testDir), false);

        std::filesystem::create_directories(std::format(L"{}/l1/l2/l3-other", testDir));
        setCaseSensitivity(std::format(L"{}/l1/l2/l3-other", testDir), false);

        VERIFY_IS_FALSE(getCaseSensitivity(std::format(L"{}/l1/l2", testDir)));
        VERIFY_IS_FALSE(getCaseSensitivity(std::format(L"{}/l1/l2/l3", testDir)));
        VERIFY_IS_FALSE(getCaseSensitivity(std::format(L"{}/l1/l2/l3-other", testDir)));

        wsl::windows::common::filesystem::EnsureCaseSensitiveDirectory(testDir, flags);

        VERIFY_IS_TRUE(getCaseSensitivity(std::format(L"{}/l1/l2/l3", testDir)));
        VERIFY_IS_TRUE(getCaseSensitivity(std::format(L"{}/l1/l2/l3-other", testDir)));
        VERIFY_IS_TRUE(getCaseSensitivity(std::format(L"{}/l1/l2", testDir)));
        VERIFY_IS_TRUE(getCaseSensitivity(std::format(L"{}/l1", testDir)));
        VERIFY_IS_TRUE(getCaseSensitivity(testDir));
    }

    TEST_METHOD(AutomountRespectedWithElevation)
    {
        DistroFileChange distributionconf(L"/etc/wsl.conf", false);
        distributionconf.SetContent(L"[automount]\nenabled=false\n");

        DistroFileChange distributionFstab(L"/etc/fstab", false);
        distributionFstab.SetContent(L"");
        TerminateDistribution();

        const auto nonElevatedToken = GetNonElevatedToken();
        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(L"echo dummy", nullptr, nullptr, nullptr, nonElevatedToken.get()));
        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"mountpoint /mnt/c", 32u);
        VERIFY_ARE_EQUAL(out, L"/mnt/c is not a mountpoint\n");
    }

    TEST_METHOD(FstabRespectedWithElevationAndAutomountDisabled)
    {
        DistroFileChange distributionconf(L"/etc/wsl.conf", false);
        distributionconf.SetContent(L"[automount]\nenabled=false\n");

        DistroFileChange distributionFstab(L"/etc/fstab", false);
        distributionFstab.SetContent(L"C:\\\\ /mnt/c drvfs metadata 0 0");

        TerminateDistribution();

        const auto nonElevatedToken = GetNonElevatedToken();
        VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(L"echo dummy", nullptr, nullptr, nullptr, nonElevatedToken.get()));
        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"mountpoint /mnt/c", 0u);
        VERIFY_ARE_EQUAL(out, L"/mnt/c is a mountpoint\n");
    }

    // This test case validates that the pipeline doesn't get stuck when both stdout & stdin are a pipe.
    // See: https://github.com/microsoft/WSL/issues/12523
    TEST_METHOD(DualPipeRelay)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { DeleteFile(L"compressed.gz"); });

        wsl::windows::common::SubProcess process{
            nullptr, L"cmd /c type \"C:\\Program Files\\WSL\\wsl.exe\" | wsl gzip > compressed.gz"};

        VERIFY_ARE_EQUAL(process.Run(), 0L);

        wil::unique_handle file{CreateFile(L"compressed.gz", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!file);

        wsl::windows::common::helpers::SetHandleInheritable(file.get());

        // Validate that the relay didn't get stuck, and that its output is correct.
        auto [expandedHash, _] = LxsstuLaunchWslAndCaptureOutput(L"gzip -d -| md5sum -", 0, file.get());
        auto [expectedHash, __] =
            LxsstuLaunchWslAndCaptureOutput(L"cat \"$(wslpath 'C:\\Program Files\\WSL\\wsl.exe')\" |  md5sum - ");

        VERIFY_ARE_EQUAL(expandedHash, expectedHash);
    }

    TEST_METHOD(EtcHosts)
    {
        {
            // Verify that setting network.generateHosts=false doesn't create /etc/hosts

            DistroFileChange wslConf{L"/etc/wsl.conf", false};
            wslConf.SetContent(L"[network]\ngenerateHosts=false");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"rm /etc/hosts"), 0L);

            TerminateDistribution();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"! test -f /etc/hosts"), 0L);
        }

        {
            // Verify that /etc/hosts generation is correct.
            TerminateDistribution();

            auto [content, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/hosts");
            auto [hostname, domain] = wsl::windows::common::filesystem::GetHostAndDomainNames();

            const auto lines = wsl::shared::string::Split(content, L'\n');
            VERIFY_IS_TRUE(lines.size() > 4);
            VERIFY_ARE_EQUAL(lines[0] + L"\n", WIDEN(LX_INIT_AUTO_GENERATED_FILE_HEADER));
            VERIFY_ARE_EQUAL(lines[1], L"# [network]");
            VERIFY_ARE_EQUAL(lines[2], L"# generateHosts = false");
            VERIFY_ARE_EQUAL(lines[3], L"127.0.0.1\tlocalhost");
            VERIFY_ARE_EQUAL(lines[4], std::format(L"127.0.1.1\t{}.{}\t{}", hostname, domain, hostname));
        }
    }

    TEST_METHOD(ExecEmptyArg)
    {
        // See: https://github.com/microsoft/WSL/issues/12649

        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--exec echo \"\"");
            VERIFY_ARE_EQUAL(out, L"\n");
            VERIFY_ARE_EQUAL(err, L"");
        }

        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--exec echo foo \"\" bar");
            VERIFY_ARE_EQUAL(out, L"foo  bar\n"); // Two spaces because echo adds one between each argument.
            VERIFY_ARE_EQUAL(err, L"");
        }
    }

    TEST_METHOD(DistroTimeout)
    {
        WslConfigChange config(LxssGenerateTestConfig() + L"[general]\ninstanceIdleTimeout=-1");
        auto distroId = GetDistributionId(LXSS_DISTRO_NAME_TEST_L);

        auto getDistroState = [&]() {
            wsl::windows::common::SvcComm service;

            for (const auto& e : service.EnumerateDistributions())
            {
                if (wsl::shared::string::IsEqual(e.DistroName, LXSS_DISTRO_NAME_TEST_L))
                {
                    return e.State;
                }
            }

            return LxssDistributionStateInvalid;
        };

        // Validate that distributions don't time out when timeout is -1
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo OK"), 0L);

            std::this_thread::sleep_for(std::chrono::seconds(20));
            VERIFY_ARE_EQUAL(getDistroState(), LxssDistributionStateRunning);
        }

        // Validate that distributions time out when timeout value is > 0
        {
            config.Update(LxssGenerateTestConfig() + L"[general]\ninstanceIdleTimeout=2000");

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo OK"), 0L);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);

            unsigned long iterations = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (getDistroState() == LxssDistributionStateInstalled)
                {
                    LogInfo("Distribution stopped after %lu iterations", iterations);
                    return;
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
                iterations++;
            }

            LogError("Distribution failed to time out after %lu iterations. State: %i", iterations, getDistroState());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(WslUpdate)
    {
        // Test the regular wsl --update logic
        {
            auto json =
                LR"(
        {
          "name": "2.4.12",
          "assets": [
            {
              "url": "http://arm-url",
              "id": 1,
              "name": "wsl.2.4.12.0.arm64.msi"
            },
            {
              "url": "http://x64-url",
              "id": 2,
              "name": "wsl.2.4.12.0.x64.msi"
            }]})";

            auto [version, asset] = wsl::windows::common::wslutil::GetLatestGithubRelease(false, json);

            VERIFY_ARE_EQUAL(version, L"2.4.12");
            VERIFY_ARE_EQUAL(asset.id, 2);
            VERIFY_ARE_EQUAL(asset.url, L"http://x64-url");
            VERIFY_ARE_EQUAL(asset.name, L"wsl.2.4.12.0.x64.msi");
        }

        // Test wsl --update --pre-release
        {
            auto json =
                LR"([
        {
          "name": "2.4.12"
        },
        {
          "name": "2.5.1",
          "assets": [
            {
              "url": "http://arm-url",
              "id": 1,
              "name": "wsl.2.5.1.0.arm64.msi"
            },
            {
              "url": "http://x64-url",
              "id": 2,
              "name": "wsl.2.5.1.0.x64.msi"
            }
            ]
        },
        {
          "name": "2.4.13"
        }])";

            auto [version, asset] = wsl::windows::common::wslutil::GetLatestGithubRelease(true, json);

            VERIFY_ARE_EQUAL(version, L"2.5.1");
            VERIFY_ARE_EQUAL(asset.id, 2);
            VERIFY_ARE_EQUAL(asset.url, L"http://x64-url");
            VERIFY_ARE_EQUAL(asset.name, L"wsl.2.5.1.0.x64.msi");
        }
    }

    TEST_METHOD(CustomModulesVhd)
    {
        WSL2_TEST_ONLY();

#ifdef WSL_DEV_INSTALL_PATH

        auto modulesPath = std::format(L"{}\\modules.vhd", WSL_DEV_INSTALL_PATH);
        auto kernelPath = std::format(L"{}\\kernel", WSL_DEV_INSTALL_PATH);

#else
        auto modulesPath = std::format(L"{}\\tools\\modules.vhd", wsl::windows::common::wslutil::GetMsiPackagePath().value());
        auto kernelPath = std::format(L"{}\\tools\\kernel", wsl::windows::common::wslutil::GetMsiPackagePath().value());

#endif

        // Create a copy of the modules vhd
        auto testModules = std::filesystem::current_path() / "test-modules.vhd";

        VERIFY_IS_TRUE(CopyFile(modulesPath.c_str(), testModules.c_str(), false));

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(testModules); });

        auto cmd = std::format(
            LR"($acl = Get-Acl '{}' ; $acl.RemoveAccessRuleAll((New-Object System.Security.AccessControl.FileSystemAccessRule(\"Everyone\", \"Read\", \"None\", \"None\", \"Allow\"))); Set-Acl -Path '{}' -AclObject $acl)",
            testModules,
            testModules);

        LxsstuLaunchPowershellAndCaptureOutput(cmd);

        // Update .wslconfig to point to the copied kernel
        WslConfigChange config{LxssGenerateTestConfig({.kernel = kernelPath, .kernelModules = testModules.wstring()})};

        // Validate that WSL starts correctly
        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"echo OK");
        VERIFY_ARE_EQUAL(out, L"OK\n");
        VERIFY_ARE_EQUAL(err, L"");
    }

    TEST_METHOD(BrokenDistroImport)
    { // Validate that importing an empty tar fails.
        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--import broken-test-distro . NUL", -1);

            VERIFY_ARE_EQUAL(
                out,
                L"The imported file is not a valid Linux distribution.\r\nError code: "
                L"Wsl/Service/RegisterDistro/WSL_E_NOT_A_LINUX_DISTRO\r\n");

            // TODO: Uncomment once SetVersionDebug is removed from the tests .wslconfig.
            // VERIFY_ARE_EQUAL(err, L"");
        }

        // Validate that importing an empty tar via wsl --install fails.
        {
            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--install --from-file NUL --name broken-test-distro", -1);

            VERIFY_ARE_EQUAL(
                out,
                L"Installing: NUL\r\nThe imported file is not a valid Linux distribution.\r\nError code: "
                L"Wsl/Service/RegisterDistro/WSL_E_NOT_A_LINUX_DISTRO\r\n");
            // TODO: Uncomment once SetVersionDebug is removed from the tests .wslconfig.
            // VERIFY_ARE_EQUAL(err, L"");
        }

        // Validate that importing an empty VHDX fails.
        if (LxsstuVmMode())
        {
            constexpr auto testVhd = L"EmptyVhd.vhdx";

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(testVhd); });

            LxsstuLaunchPowershellAndCaptureOutput(std::format(L"New-Vhd {}  -SizeBytes 20MB", testVhd));

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"--mount {} --vhd --bare", testVhd)), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"mkfs.ext4 /dev/sde"), 0L);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount"), 0L);

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"--import-in-place broken-test-distro {}", testVhd), -1);

            VERIFY_ARE_EQUAL(
                out,
                L"The imported file is not a valid Linux distribution.\r\nError code: "
                L"Wsl/Service/RegisterDistro/WSL_E_NOT_A_LINUX_DISTRO\r\n");
            // TODO: Uncomment once SetVersionDebug is removed from the tests .wslconfig.
            // VERIFY_ARE_EQUAL(err, L"");
        }

        // Validate that tars containing /etc, but not /bin/sh are accepted.
        if (LxsstuVmMode())
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(L"--unregister empty-distro"); });

            DistroFileChange conf(L"/etc/wsl.conf", false);
            conf.SetContent(L"");

            auto [out, err] = LxsstuLaunchWslAndCaptureOutput(
                L"tar cf - /etc/wsl.conf | wsl.exe --install --from-file - --name empty-distro --no-launch "
                L"--version 2");
        }
    }

    TEST_METHOD(ImportExportStdout)
    {
        constexpr auto test_distro = L"import-test-distro";
        auto cleanup =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { LxsstuLaunchWsl(std::format(L"--unregister {}", test_distro)); });

        auto commandLine = std::format(L"cmd.exe /c wsl --export {} - | wsl --import {} . -", LXSS_DISTRO_NAME_TEST_L, test_distro);

        VERIFY_ARE_EQUAL(LxsstuRunCommand(commandLine.data()), 0L);

        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(std::format(L"-d {} echo ok", test_distro));
        VERIFY_ARE_EQUAL(out, L"ok\n");
        VERIFY_ARE_EQUAL(err, L"");
    }

    TEST_METHOD(EtcHostsParsing)
    {
        constexpr auto inputFileName = L"test-etc-hosts.txt";

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(inputFileName); });

        auto validate = [](const std::string& Input, const std::string& ExpectedOutput) {
            wil::unique_handle inputFile{CreateFile(inputFileName, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr)};

            VERIFY_IS_TRUE(WriteFile(inputFile.get(), Input.c_str(), static_cast<DWORD>(Input.size()), nullptr, nullptr));

            auto output = wsl::windows::common::filesystem::GetWindowsHosts(inputFileName);

            VERIFY_ARE_EQUAL(ExpectedOutput, output);
        };

        validate("127.0.0.1 microsoft.com", "127.0.0.1\tmicrosoft.com\n");
        validate("\xEF\xBB\xBF 127.0.0.1 microsoft.com", "127.0.0.1\tmicrosoft.com\n"); // Validate that BOM headers are ignored.
        validate("#Comment 127.0.0.1 microsoft.com windows.microsoft.com\n#AnotherComment", "");
        validate(
            "#Comment 127.0.0.1 microsoft.com windows.microsoft.com\n#AnotherComment\n127.0.0.1 wsl.dev", "127.0.0.1\twsl.dev\n");
    }

    // Validate that a distribution can be unregistered even if its BasePath doesn't exist.
    // See https://github.com/microsoft/WSL/issues/13004
    TEST_METHOD(BrokenDistroUnregister)
    {
        const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
        const auto distroKey = wsl::windows::common::registry::CreateKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");

        auto revert = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            wsl::windows::common::registry::DeleteKey(userKey.get(), L"{baa405ef-1822-4bbe-84e2-30e4c6330d42}");
        });

        wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"BasePath", L"C:\\DoesNotExit");
        wsl::windows::common::registry::WriteString(distroKey.get(), nullptr, L"DistributionName", L"DummyBrokenDistro");
        wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"DefaultUid", 0);
        wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Version", LXSS_DISTRO_VERSION_2);
        wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"State", LxssDistributionStateInstalled);
        wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, L"Flags", LXSS_DISTRO_FLAGS_VM_MODE);

        auto [out, err] = LxsstuLaunchWslAndCaptureOutput(L"--unregister DummyBrokenDistro");

        VERIFY_ARE_EQUAL(out, L"The operation completed successfully. \r\n");
        VERIFY_ARE_EQUAL(err, L"");
    }

    // Validate that calling the binfmt interpreter with tty fd's but not controlling terminal doesn't display a warning.
    // See https://github.com/microsoft/WSL/issues/13173.
    TEST_METHOD(SetSidNoWarning)
    {
        auto [out, err] =
            LxsstuLaunchWslAndCaptureOutput(L"socat - 'EXEC:setsid --wait cmd.exe /c echo OK',pty,setsid,ctty,stderr");

        VERIFY_ARE_EQUAL(out, L"OK\r\r\n");
        VERIFY_ARE_EQUAL(err, L"");
    }

}; // namespace UnitTests
} // namespace UnitTests
