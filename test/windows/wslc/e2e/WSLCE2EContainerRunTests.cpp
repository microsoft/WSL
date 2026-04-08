/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerRunTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerRunTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerRunTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HelpCommand)
    {
        auto result = RunWslc(L"container run --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Container_With_Command)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto command = L"echo echo_from_container";
        auto result = RunWslc(std::format(L"container run --name {} {} {}", WslcContainerName, DebianImage.NameAndTag(), command));
        result.Verify({.Stdout = L"echo_from_container\n", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint)
    {
        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/whoami {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_And_Arguments)
    {
        auto result = RunWslc(
            std::format(L"container run --rm --entrypoint /bin/echo {} hello from entrypoint with args", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"hello from entrypoint with args\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Invalid_Path)
    {
        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/does-not-exist {}", DebianImage.NameAndTag()));
        result.Verify(
            {.Stdout = L"", .Stderr = L"failed to create task for container: failed to create shim task: OCI runtime create failed: runc create failed: unable to start container process: error during container init: exec: \"/bin/does-not-exist\": stat /bin/does-not-exist: no such file or directory: unknown\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Detach_Lifecycle)
    {
        auto result = RunWslc(std::format(
            L"container run --name {} -d --entrypoint /bin/sleep {} infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u 0 {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidGidRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u 0:0 {} sh -c \"id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownUser_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm -u user_does_not_exist {} id -u", DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"unable to find user user_does_not_exist: no matching entries in passwd file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownGroup_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm -u root:badgid {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"unable to find group badgid: no matching entries in group file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameGroupRoot)
    {
        auto result =
            RunWslc(std::format(L"container run --rm -u root:root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NonRootUser_Succeeds)
    {
        auto result = RunWslc(std::format(L"container run --rm -u nobody {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"nobody\n65534\n65534\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs {} sh -c \"echo -n 'tmpfs_test' > /wslc-tmpfs/data && cat "
            L"/wslc-tmpfs/data\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"tmpfs_test", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs:size=64k {} sh -c \"mount | grep -q ' on /wslc-tmpfs type tmpfs ' && echo "
            L"mounted\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_Multiple_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs1:size=64k --tmpfs /wslc-tmpfs2:size=128k {} sh -c \"mount | grep -q ' on "
            L"/wslc-tmpfs1 type tmpfs ' && mount | grep -q ' on /wslc-tmpfs2 type tmpfs ' && echo mounted\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_RelativePath_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm --tmpfs wslc-tmpfs {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: 'wslc-tmpfs' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_EmptyDestination_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm --tmpfs :size=64k {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: '' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS_SingleServer)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --dns 8.8.8.8 {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 8.8.8.8") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS_MultipleServers)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --dns 8.8.8.8 --dns 8.8.4.4 {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 8.8.8.8") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 8.8.4.4") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS_Search)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --dns-search example.com {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"search example.com") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS_Option)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --dns-option ndots:5 {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"options ndots:5") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS_AllOptions)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --dns 1.1.1.1 --dns-search test.local --dns-option ndots:3 {} cat /etc/resolv.conf",
            DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 1.1.1.1") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"search test.local") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"options ndots:3") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NoDNS_ConflictWithDNS_Fails)
    {

        auto result = RunWslc(std::format(
            L"container run --rm --no-dns --dns 8.8.8.8 {} echo test", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Cannot use --no-dns with --dns, --dns-domain, --dns-option, or --dns-search\r\n", .ExitCode = 1});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Runs a container. By default, the container is started in the background; use --detach to run in the "
               L"foreground.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container run [<options>] <image> [<command>] [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  image             Image name\r\n"
                 << L"  command           The command to run\r\n"
                 << L"  arguments         Arguments to pass to container's init process\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -d,--detach       Run container in detached mode\r\n"
                << L"  --dns             IP address of the DNS nameserver in resolv.conf\r\n"
                << L"  --dns-domain      Set the default DNS Domain\r\n"
                << L"  --dns-option      Set DNS options\r\n"
                << L"  --dns-search      Set DNS search domains\r\n"
                << L"  --entrypoint      Specifies the container init process executable\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --name            Name of the container\r\n"
                << L"  --no-dns          No configuration of DNS in the container\r\n"
                << L"  -p,--publish      Publish a port from a container to host\r\n"
                << L"  --rm              Remove the container after it stops\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  --tmpfs           Mount tmpfs to the container at the given path\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  -v,--volume       Bind mount a volume to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests