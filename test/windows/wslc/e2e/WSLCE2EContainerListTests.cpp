/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ContainerModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

using namespace wsl::windows::wslc::models;
using namespace wsl::windows::common::string;

class WSLCE2EContainerListTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerListTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_HelpCommand)
    {
        auto result = RunWslc(L"container list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_AllOption)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Find container in list output
        result = RunWslc(L"container list --no-trunc --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto outputLines = result.GetStdoutLines();
        std::optional<std::wstring> foundContainerLine{};
        for (const auto& line : outputLines)
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                foundContainerLine = line;
                break;
            }
        }

        // Verify we found the container in the list output
        VERIFY_IS_TRUE(foundContainerLine.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, foundContainerLine->find(L"created"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_NoOptions_RunningContainers)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = TruncateId(result.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Find container in list output with no options
        result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto outputLines = result.GetStdoutLines();
        std::optional<std::wstring> foundContainerLine{};
        for (const auto& line : outputLines)
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                foundContainerLine = line;
                break;
            }
        }

        // Verify we found the container in the list output
        VERIFY_IS_TRUE(foundContainerLine.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, foundContainerLine->find(L"running"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_NoOptions_ExcludesCreatedContainers)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create (but do not start) a container.
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(result.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Default list should only show running containers.
        result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        bool isListed = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                isListed = true;
                break;
            }
        }

        VERIFY_IS_FALSE(isListed);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_QuietOption_OutputsIdsOnly)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        result = RunWslc(L"container list --all --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the created container ID appears in the quiet output.
        VERIFY_IS_TRUE(result.StdoutContainsLine(containerId));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_InvalidFormatOption)
    {
        const auto result = RunWslc(L"container list --format invalid");
        result.Verify({.Stderr = L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table.\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_JsonFormat)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // List containers with json format
        result = RunWslc(L"container list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        // Parse json and verify we got the expected container information back
        auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_IS_GREATER_THAN_OR_EQUAL(containers.size(), 1U);

        auto findContainer = [](const std::vector<ContainerInformation>& list, const std::wstring& id) {
            return std::ranges::any_of(list, [&](const auto& c) { return wsl::shared::string::MultiByteToWide(c.Id) == id; });
        };

        VERIFY_IS_TRUE(findContainer(containers, containerId));

        // Create another container
        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId2 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId2.empty());

        // List containers with json format again
        result = RunWslc(L"container list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        // Parse json and verify we got both containers back
        containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_IS_GREATER_THAN_OR_EQUAL(containers.size(), 2U);

        VERIFY_IS_TRUE(findContainer(containers, containerId));
        VERIFY_IS_TRUE(findContainer(containers, containerId2));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_InvalidKey)
    {
        // Filter keys are validated by the Docker daemon, which rejects unknown keys.
        const auto result = RunWslc(L"container list --filter color=blue");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter 'color'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_MalformedValue)
    {
        // Filter values must be of the form key=value; bare keys are rejected by the CLI.
        const auto result = RunWslc(L"container list --filter status");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = Localization::WSLCCLI_InvalidFilterError(L"status") + L"\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_InvalidStatusValue)
    {
        // Status values are validated by the Docker daemon, which rejects unknown values.
        const auto result = RunWslc(L"container list --filter status=bogus");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter 'status=bogus'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_Name)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId2 = result.GetStdoutOneLine();

        result = RunWslc(std::format(L"container list --all --format json --filter name={}", WslcContainerName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1U, containers.size());
        VERIFY_ARE_EQUAL(WideToMultiByte(WslcContainerName2), std::string(containers[0].Name));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_Status)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        // Created (never started) container.
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Running container.
        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"container list --all --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(r.Stdout.value().c_str());
            std::set<std::string> names;
            for (const auto& c : containers)
            {
                names.insert(c.Name);
            }
            return names;
        };

        // status=created
        {
            const auto names = listNames(L"--filter status=created");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // status=running
        {
            const auto names = listNames(L"--filter status=running");
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // Multiple --filter status= values are OR'd.
        {
            const auto names = listNames(L"--filter status=created --filter status=running");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName2)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_Label)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        // First container has both labels; second has only one of them.
        auto result = RunWslc(std::format(
            L"container create --name {} --label filter.test=yes --label filter.role=primary {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container create --name {} --label filter.test=yes {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"container list --all --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(r.Stdout.value().c_str());
            std::set<std::string> names;
            for (const auto& c : containers)
            {
                names.insert(c.Name);
            }
            return names;
        };

        // label=<key> matches both since both have filter.test set (any value).
        {
            const auto names = listNames(L"--filter label=filter.test");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // label=<key>=<value> matches only the first.
        {
            const auto names = listNames(L"--filter label=filter.role=primary");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // Multiple --filter label= entries are AND'd.
        {
            const auto names = listNames(L"--filter label=filter.test --filter label=filter.role=primary");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName2)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_Id)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Filter by id (full id) should return exactly one container.
        result = RunWslc(std::format(L"container list --all --format json --filter id={}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1U, containers.size());
        VERIFY_ARE_EQUAL(WideToMultiByte(containerId), std::string(containers[0].Id));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_Exited)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        // First container exits with code 0.
        auto result = RunWslc(std::format(L"container run --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.ExitCode = 0});

        // Second container exits with non-zero code.
        result = RunWslc(std::format(L"container run --name {} {} sh -c \"exit 7\"", WslcContainerName2, DebianImage.NameAndTag()));
        // run with non-zero container exit code is allowed; we don't assert exit here.

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"container list --all --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(r.Stdout.value().c_str());
            std::set<std::string> names;
            for (const auto& c : containers)
            {
                names.insert(c.Name);
            }
            return names;
        };

        // exited=0 should match the first container only.
        {
            const auto names = listNames(L"--filter exited=0");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // exited=7 should match the second container only.
        {
            const auto names = listNames(L"--filter exited=7");
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName2)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_Filter_BeforeSince)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        // Create the first container, then the second so that ordering is deterministic.
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"container list --all --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(r.Stdout.value().c_str());
            std::set<std::string> names;
            for (const auto& c : containers)
            {
                names.insert(c.Name);
            }
            return names;
        };

        // before=<container2 name> -> only container1 (created earlier) is visible.
        {
            const auto names = listNames(std::format(L"--filter before={}", WslcContainerName2));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName2)));
        }

        // since=<container1 name> -> only container2 (created later) is visible.
        {
            const auto names = listNames(std::format(L"--filter since={}", WslcContainerName));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(WslcContainerName)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(WslcContainerName2)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_LastAndLatest)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        // Create container1 then container2 (deterministic order).
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // --latest is shorthand for --last 1; should return only container2 (most recent).
        // Implies --all so created-but-not-running containers are visible.
        {
            result = RunWslc(L"container list --latest --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1U, containers.size());
            VERIFY_ARE_EQUAL(WideToMultiByte(WslcContainerName2), std::string(containers[0].Name));
        }

        // --last 2 should cap output at 2 containers.
        {
            result = RunWslc(L"container list --last 2 --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
            VERIFY_IS_TRUE(containers.size() <= 2u);
        }

        // --last and --latest are mutually exclusive.
        {
            result = RunWslc(L"container list --last 1 --latest");
            VERIFY_ARE_EQUAL(1, result.ExitCode);
        }

        // --last requires an integer.
        {
            result = RunWslc(L"container list --last bogus");
            VERIFY_ARE_EQUAL(1, result.ExitCode);
        }
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ContainerListLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container list [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: ls ps\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -a,--all     Show all regardless of state.\r\n"
                << L"  -f,--filter  " << Localization::WSLCCLI_FilterArgDescription() << L"\r\n"
                << L"  --format     " << Localization::WSLCCLI_FormatArgDescription() << L"\r\n"
                << L"  -n,--last    " << Localization::WSLCCLI_LastArgDescription() << L"\r\n"
                << L"  -l,--latest  " << Localization::WSLCCLI_LatestArgDescription() << L"\r\n"
                << L"  --no-trunc   Do not truncate output\r\n"
                << L"  -q,--quiet   Outputs the container IDs only\r\n"
                << L"  --session    Specify the session to use\r\n"
                << L"  -?,--help    Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests