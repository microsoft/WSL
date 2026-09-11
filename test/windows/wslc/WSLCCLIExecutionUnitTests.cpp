/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIExecutionUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI command execution.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "SessionModel.h"

#include "AsyncExecution.h"
#include "Command.h"
#include "CommandLineParser.h"
#include "RootCommand.h"
#include "ArgumentValidation.h"
#include "ContainerCommand.h"
#include "ContainerTasks.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIExecutionUnitTests {
namespace {
    struct TestUpCommand final : Command
    {
        TestUpCommand(const std::wstring& parent) : Command(L"up", parent)
        {
        }

        std::vector<Argument> GetArguments() const override
        {
            return {Argument::Create(ArgType::Detach)};
        }

        std::wstring ShortDescription() const override
        {
            return L"Scoped global leaf";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }
    };

    struct TestComposeCommand final : Command
    {
        TestComposeCommand(const std::wstring& parent) : TestComposeCommand(L"compose", parent)
        {
        }

        TestComposeCommand(std::wstring_view name, const std::wstring& parent) : Command(name, parent)
        {
        }

        std::vector<Argument> GetGlobalArguments() const override
        {
            return {Argument::Create(ArgType::Progress)};
        }

        std::wstring ShortDescription() const override
        {
            return L"Scoped global command";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        std::vector<std::unique_ptr<Command>> CreateCommands() const override
        {
            std::vector<std::unique_ptr<Command>> commands;
            commands.emplace_back(std::make_unique<TestUpCommand>(FullName()));
            return commands;
        }

        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }
    };

    struct TestRootCommand final : Command
    {
        TestRootCommand() : Command(L"root", L"")
        {
        }

        std::vector<Argument> GetGlobalArguments() const override
        {
            return {Argument::Create(ArgType::Session)};
        }

        std::wstring ShortDescription() const override
        {
            return L"Scoped global root";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        std::vector<std::unique_ptr<Command>> CreateCommands() const override
        {
            std::vector<std::unique_ptr<Command>> commands;
            commands.emplace_back(std::make_unique<TestComposeCommand>(FullName()));
            return commands;
        }

        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }
    };

    struct TestPositionalCommand final : Command
    {
        TestPositionalCommand(const std::wstring& parent) : Command(L"show", parent)
        {
        }

        std::vector<Argument> GetArguments() const override
        {
            return {Argument::Create(ArgType::ImageId, {.Required = true})};
        }

        std::wstring ShortDescription() const override
        {
            return L"Positional leaf";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }
    };

    struct TraversalTrackingCommand final : Command
    {
        TraversalTrackingCommand(const std::wstring& parent, size_t& traversalCount) :
            Command(L"unrelated", parent), m_traversalCount(traversalCount)
        {
        }

        std::wstring ShortDescription() const override
        {
            return L"Tracks lazy subtree traversal";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

        std::vector<Argument> GetGlobalArguments() const override
        {
            return {Argument::Create(ArgType::Progress)};
        }

    protected:
        std::vector<std::unique_ptr<Command>> CreateCommands() const override
        {
            ++m_traversalCount;
            return {};
        }

        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }

    private:
        size_t& m_traversalCount;
    };

    struct PositionalTestRootCommand final : Command
    {
        PositionalTestRootCommand(size_t& traversalCount) : Command(L"root", L""), m_traversalCount(traversalCount)
        {
        }

        std::wstring ShortDescription() const override
        {
            return L"Positional test root";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        std::vector<std::unique_ptr<Command>> CreateCommands() const override
        {
            std::vector<std::unique_ptr<Command>> commands;
            commands.emplace_back(std::make_unique<TestPositionalCommand>(FullName()));
            commands.emplace_back(std::make_unique<TraversalTrackingCommand>(FullName(), m_traversalCount));
            return commands;
        }

        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }

    private:
        size_t& m_traversalCount;
    };

    CommandInvocation ParseTestCommandLine(std::vector<std::wstring> arguments, CLIExecutionContext& context)
    {
        CommandInvocation invocation{std::make_unique<TestRootCommand>(), std::move(arguments)};
        ParseCommandLine(invocation, context);
        return invocation;
    }
} // namespace

// Helper structure to hold test data
struct CommandLineTestCase
{
    std::wstring commandLine;
    std::wstring expectedCommand;
    bool shouldSucceed;
};

class WSLCCLIExecutionUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIExecutionUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(ValidateArguments_RequiredOptionUsesLongName)
    {
        RootCommand command;
        ArgMap args;
        const std::vector<Argument> definitions{Argument::Create(ArgType::Password, {.Required = true})};

        try
        {
            command.ValidateArguments(args, definitions, false);
            VERIFY_FAIL(L"Expected ArgumentException");
        }
        catch (const ArgumentException& exception)
        {
            VERIFY_ARE_EQUAL(wsl::shared::Localization::WSLCCLI_RequiredArgumentOptionError(L"--password"), exception.Message());
        }
    }

    TEST_METHOD(GlobalEnvironmentOptions_NoColorIsAppliedAndFrozen)
    {
        {
            CLIExecutionContext context;

            context.ApplyGlobalEnvironmentOptions();
            VERIFY_IS_FALSE(context.Terminal.IsNoColor());

            VERIFY_THROWS_SPECIFIC(context.GlobalArgs.Add<ArgType::NoColor>(true), wil::ResultException, [](const wil::ResultException& e) {
                return e.GetErrorCode() == E_ILLEGAL_METHOD_CALL;
            });
        }

        {
            CLIExecutionContext present;
            present.GlobalArgs.Add<ArgType::NoColor>(true);
            present.ApplyGlobalEnvironmentOptions();
            VERIFY_IS_TRUE(present.Terminal.IsNoColor());

            VERIFY_NO_THROW(Argument::Create(ArgType::NoColor).Validate(present.GlobalArgs));
            VERIFY_THROWS_SPECIFIC(present.GlobalArgs.Remove(ArgType::NoColor), wil::ResultException, [](const wil::ResultException& e) {
                return e.GetErrorCode() == E_ILLEGAL_METHOD_CALL;
            });
        }
    }

    TEST_METHOD(ScopedGlobalArguments_AccumulateAcrossCommandScopes)
    {
        CLIExecutionContext context;
        const auto invocation =
            ParseTestCommandLine({L"--session", L"foo", L"compose", L"--progress", L"plain", L"up", L"--detach"}, context);

        VERIFY_ARE_EQUAL(std::wstring_view{L"up"}, invocation.Selected().Name());
        VERIFY_ARE_EQUAL(std::wstring{L"foo"}, context.GlobalArgs.GetValue<ArgType::Session>());
        VERIFY_IS_TRUE(context.GlobalArgs.Contains(ArgType::Progress));
        VERIFY_IS_TRUE(context.Args.GetValue<ArgType::Detach>());
        VERIFY_IS_FALSE(context.Args.Contains(ArgType::Session));
        VERIFY_IS_FALSE(context.Args.Contains(ArgType::Progress));

        const auto compose = invocation.Selected().Parent();
        VERIFY_IS_TRUE(compose.has_value());
        VERIFY_ARE_EQUAL(std::wstring_view{L"compose"}, compose->get().Name());

        const auto root = compose->get().Parent();
        VERIFY_IS_TRUE(root.has_value());
        VERIFY_ARE_EQUAL(&invocation.Root(), &root->get());
        VERIFY_ARE_EQUAL(7u, invocation.OriginalArguments().size());
        VERIFY_ARE_EQUAL(invocation.OriginalArguments().size(), invocation.Position());
    }

    TEST_METHOD(ScopedGlobalArguments_PathPreservesOwningCommands)
    {
        const TestRootCommand root;
        const auto& compose = *root.GetCommands().front();
        const auto& up = *compose.GetCommands().front();
        const auto scopes = GetGlobalArgumentPath(up);

        VERIFY_ARE_EQUAL(2u, scopes.size());
        VERIFY_ARE_EQUAL(std::wstring{L"wslc"}, scopes[0].CommandInvocation);
        VERIFY_ARE_EQUAL(ArgType::Session, scopes[0].Arguments[0].Type());
        VERIFY_ARE_EQUAL(std::wstring{L"wslc compose"}, scopes[1].CommandInvocation);
        VERIFY_ARE_EQUAL(ArgType::Progress, scopes[1].Arguments[0].Type());
    }

    TEST_METHOD(ScopedGlobalArguments_PathIncludesStandaloneTarget)
    {
        const TestComposeCommand command{L"standalone"};
        const auto scopes = GetGlobalArgumentPath(command);

        VERIFY_ARE_EQUAL(1u, scopes.size());
        VERIFY_ARE_EQUAL(std::wstring{L"wslc compose"}, scopes[0].CommandInvocation);
        VERIFY_ARE_EQUAL(ArgType::Progress, scopes[0].Arguments[0].Type());
    }

    TEST_METHOD(ScopedGlobalArguments_PositionalDoesNotTraverseUnrelatedSubtrees)
    {
        size_t traversalCount = 0;
        CLIExecutionContext context;
        CommandInvocation invocation{
            std::make_unique<PositionalTestRootCommand>(traversalCount), std::vector<std::wstring>{L"show", L"image"}};

        ParseCommandLine(invocation, context);

        VERIFY_ARE_EQUAL(std::wstring_view{L"show"}, invocation.Selected().Name());
        VERIFY_ARE_EQUAL(std::wstring{L"image"}, context.Args.GetValue<ArgType::ImageId>());
        VERIFY_ARE_EQUAL(0u, traversalCount);
    }

    TEST_METHOD(ScopedGlobalArguments_DetachedSubtreeFormatsChildInvocation)
    {
        const TestComposeCommand command{L"root"};
        const auto& child = *command.GetCommands().front();

        VERIFY_ARE_EQUAL(std::wstring{L"wslc compose up"}, child.FormatInvocation());
    }

    TEST_METHOD(ScopedGlobalArguments_DescendantGlobalAtRootIsUnknown)
    {
        CLIExecutionContext context;

        try
        {
            ParseTestCommandLine({L"--progress", L"plain", L"compose", L"up"}, context);
            VERIFY_FAIL(L"Expected ArgumentException");
        }
        catch (const ArgumentException& exception)
        {
            VERIFY_ARE_EQUAL(wsl::shared::Localization::WSLCCLI_InvalidNameError(L"--progress"), exception.Message());
        }
    }

    TEST_METHOD(ScopedGlobalArguments_AncestorGlobalAtChildReportsCorrectPlacement)
    {
        CLIExecutionContext context;

        try
        {
            ParseTestCommandLine({L"compose", L"--session", L"foo", L"up"}, context);
            VERIFY_FAIL(L"Expected ArgumentException");
        }
        catch (const ArgumentException& exception)
        {
            VERIFY_ARE_EQUAL(
                wsl::shared::Localization::WSLCCLI_MisplacedInheritedGlobalOptionError(L"--session", L"wslc", L"compose"),
                exception.Message());
        }
    }

    TEST_METHOD(ScopedGlobalArguments_GlobalAfterLeafOptionReportsCorrectPlacement)
    {
        CLIExecutionContext context;

        try
        {
            ParseTestCommandLine({L"compose", L"up", L"--detach", L"--progress", L"plain"}, context);
            VERIFY_FAIL(L"Expected ArgumentException");
        }
        catch (const ArgumentException& exception)
        {
            VERIFY_ARE_EQUAL(
                wsl::shared::Localization::WSLCCLI_MisplacedInheritedGlobalOptionError(L"--progress", L"wslc compose", L"up"),
                exception.Message());
        }
    }

    TEST_METHOD(ScopedGlobalArguments_SiblingGlobalIsUnknown)
    {
        size_t traversalCount = 0;
        CLIExecutionContext context;
        CommandInvocation invocation{
            std::make_unique<PositionalTestRootCommand>(traversalCount),
            std::vector<std::wstring>{L"show", L"--progress", L"plain"}};

        try
        {
            ParseCommandLine(invocation, context);
            VERIFY_FAIL(L"Expected ArgumentException");
        }
        catch (const ArgumentException& exception)
        {
            VERIFY_ARE_EQUAL(wsl::shared::Localization::WSLCCLI_InvalidNameError(L"--progress"), exception.Message());
        }

        VERIFY_ARE_EQUAL(0u, traversalCount);
    }

    // Test: Verify EnumVariantMap on DataMap for Context Data
    TEST_METHOD(EnumVariantMap_DataMapValidation)
    {
        // DataMap is an EnumVariantMap, but for command execution context data instead of arguments.
        // It does not have rigid typing like the Args map, so this will verify every Data enum value
        // can be added and retrieved successfully. The arguments unit tests have more complex tests
        // for the EnumVariantMap behavior. This one ensures Data enum values are correct.
        wsl::windows::wslc::execution::DataMap dataMap;

        // Verify all data enum values defined.
        auto allDataTypes = std::vector<Data>{};
        for (int i = 0; i < static_cast<int>(Data::Max); ++i)
        {
            Data dataType = static_cast<Data>(i);

            // Add the data to the DataMap with a test value based on its type.
            // Each data type needs to be added here as each enum may have its own value.
            VERIFY_IS_FALSE(dataMap.Contains(dataType));
            bool handled = false;
            if (dataType == Data::Session)
            {
                // Create a null session for testing - Session requires a COM pointer
                wil::com_ptr<IWSLCSession> nullSession; // Creates null COM pointer
                wsl::windows::wslc::models::Session session{nullSession};
                dataMap.Add<Data::Session>(std::move(session));
                handled = true;
            }
            else if (dataType == Data::Containers)
            {
                std::vector<wsl::windows::wslc::models::ContainerInformation> containers;
                dataMap.Add<Data::Containers>(std::move(containers));
                handled = true;
            }
            else if (dataType == Data::ContainerOptions)
            {
                wsl::windows::wslc::models::ContainerOptions options;
                dataMap.Add<Data::ContainerOptions>(std::move(options));
                handled = true;
            }
            else if (dataType == Data::Images)
            {
                std::vector<wsl::windows::wslc::models::ImageInformation> images;
                dataMap.Add<Data::Images>(std::move(images));
                handled = true;
            }
            else if (dataType == Data::Volumes)
            {
                std::vector<wsl::windows::common::wslc_schema::VolumeListEntry> volumes;
                dataMap.Add<Data::Volumes>(std::move(volumes));
                handled = true;
            }
            else if (dataType == Data::Networks)
            {
                std::vector<wsl::windows::common::wslc_schema::NetworkListEntry> networks;
                dataMap.Add<Data::Networks>(std::move(networks));
                handled = true;
            }
            else if (dataType == Data::NetworkEndpointOptions)
            {
                wsl::windows::wslc::models::NetworkEndpointOptions endpointOptions;
                dataMap.Add<Data::NetworkEndpointOptions>(std::move(endpointOptions));
                handled = true;
            }

            if (!handled)
            {
                VERIFY_FAIL(L"Unhandled Data type in test");
            }

            allDataTypes.push_back(dataType);
            VERIFY_IS_TRUE(dataMap.Contains(dataType));
        }

        // Verify basic retrieval.
        auto& session = dataMap.Get<Data::Session>();
        VERIFY_IS_NULL(session.Get()); // A null ptr was added.

        auto& containers = dataMap.Get<Data::Containers>();
        VERIFY_ARE_EQUAL(0u, containers.size());

        // Other more complex EnumVariantMap tests are in the Args unit tests.
        // This one will just verify all the data types in the Data Map work as expected.
    }

    // Test: SetContainerOptionsFromArgs sets WorkingDirectory when --workdir is provided
    TEST_METHOD(SetContainerOptionsFromArgs_WithWorkDir_SetsWorkingDirectory)
    {
        CLIExecutionContext context;
        context.Args.Add<ArgType::WorkDir>(std::wstring{L"/app"});

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: SetContainerOptionsFromArgs leaves WorkingDirectory empty when --workdir is not provided
    TEST_METHOD(SetContainerOptionsFromArgs_WithoutWorkDir_WorkingDirectoryIsEmpty)
    {
        CLIExecutionContext context;

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.WorkingDirectory.empty());
    }

    // Test: Full parse of 'exec --workdir "" cont1 cmd' rejects empty working directory
    TEST_METHOD(ExecCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        // Invoke ContainerExecCommand parsing directly with the subcommand arguments it accepts.
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'exec --workdir /path cont1 cmd' sets WorkingDirectory
    TEST_METHOD(ExecCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        // Invoke ContainerExecCommand parsing directly with the subcommand arguments it accepts.
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'exec -w /path cont1 cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(ExecCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: Full parse of 'run --workdir "" image cmd' rejects empty working directory
    TEST_METHOD(RunCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'run --workdir /path image cmd' sets WorkingDirectory
    TEST_METHOD(RunCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'run -w /path image cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(RunCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: Full parse of 'create --workdir "" image cmd' rejects empty working directory
    TEST_METHOD(CreateCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'create --workdir /path image cmd' sets WorkingDirectory
    TEST_METHOD(CreateCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'create -w /path image cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(CreateCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    TEST_METHOD(RunCommand_ParseGpusAll_SetsGpuOption)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus all ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.Gpu);
    }

    TEST_METHOD(RunCommand_ParseGpusInvalid_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus invalid ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    TEST_METHOD(CreateCommand_ParseGpusAll_SetsGpuOption)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus all ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.Gpu);
    }

    TEST_METHOD(CreateCommand_ParseGpusInvalid_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus none ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    TEST_METHOD(SetContainerOptionsFromArgs_WithoutNetwork_NetworksIsEmpty)
    {
        CLIExecutionContext context;

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.Networks.empty());
    }

    TEST_METHOD(RunCommand_ParseNetworkSingleValue_SetsNetwork)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network net1 ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(1u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
    }

    TEST_METHOD(RunCommand_ParseNetworkMultipleValues_PreservesOrder)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network net1 --network net2 ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(2u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(std::string("net2"), options.Networks[1].Name);
    }

    TEST_METHOD(RunCommand_ParseDockerNetworkAliases_SetsPerNetworkAliases)
    {
        auto invocation =
            CreateInvocationFromCommandLine(L"wslc --network name=net1,alias=a,alias=b --network name=net2,alias=c ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(2u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(2u, options.Networks[0].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("a"), options.Networks[0].Aliases[0]);
        VERIFY_ARE_EQUAL(std::string("b"), options.Networks[0].Aliases[1]);
        VERIFY_ARE_EQUAL(std::string("net2"), options.Networks[1].Name);
        VERIFY_ARE_EQUAL(1u, options.Networks[1].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("c"), options.Networks[1].Aliases[0]);
    }

    TEST_METHOD(RunCommand_ParseDockerNetworkAliasesWithNameAfterAlias_SetsPerNetworkAliases)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network alias=a,name=net1,alias=b ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(1u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(2u, options.Networks[0].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("a"), options.Networks[0].Aliases[0]);
        VERIFY_ARE_EQUAL(std::string("b"), options.Networks[0].Aliases[1]);
    }

    TEST_METHOD(RunCommand_ParseNetworkEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network \"\" ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    TEST_METHOD(CreateCommand_ParseNetworkSingleValue_SetsNetwork)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network net1 ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(1u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
    }

    TEST_METHOD(CreateCommand_ParseNetworkMultipleValues_PreservesOrder)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network net1 --network net2 ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(2u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(std::string("net2"), options.Networks[1].Name);
    }

    TEST_METHOD(CreateCommand_ParseDockerNetworkAliases_SetsPerNetworkAliases)
    {
        auto invocation =
            CreateInvocationFromCommandLine(L"wslc --network name=net1,alias=a,alias=b --network name=net2,alias=c ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(2u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(2u, options.Networks[0].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("a"), options.Networks[0].Aliases[0]);
        VERIFY_ARE_EQUAL(std::string("b"), options.Networks[0].Aliases[1]);
        VERIFY_ARE_EQUAL(std::string("net2"), options.Networks[1].Name);
        VERIFY_ARE_EQUAL(1u, options.Networks[1].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("c"), options.Networks[1].Aliases[0]);
    }

    TEST_METHOD(CreateCommand_ParseDockerNetworkAliasesWithNameAfterAlias_SetsPerNetworkAliases)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network alias=a,name=net1,alias=b ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(1u, options.Networks.size());
        VERIFY_ARE_EQUAL(std::string("net1"), options.Networks[0].Name);
        VERIFY_ARE_EQUAL(2u, options.Networks[0].Aliases.size());
        VERIFY_ARE_EQUAL(std::string("a"), options.Networks[0].Aliases[0]);
        VERIFY_ARE_EQUAL(std::string("b"), options.Networks[0].Aliases[1]);
    }

    TEST_METHOD(CreateCommand_ParseNetworkDuplicateNameOption_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network name=net1,name=net2 ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkDuplicateNameError(L"network");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkUnsupportedOption_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(
            L"wslc --network name=net1,driver-opt=com.docker.network.endpoint.sysctls="
            L"net.ipv4.conf.IFNAME.log_martians=1 ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage =
                wsl::shared::Localization::WSLCCLI_NetworkUnsupportedOptionError(L"network", L"driver-opt");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkUnknownOption_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network name=net1,aliases=a ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkUnsupportedOptionError(L"network", L"aliases");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkBackendAliasesOption_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network name=net1,Aliases=a ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkUnsupportedOptionError(L"network", L"Aliases");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkAliasWithoutName_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network alias=a ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkEmptyError(L"network");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkNameWhitespaceValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network \"name=   \" ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkEmptyError(L"network");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(ParseNetworkArgument_NameUnicodeWhitespaceValue_ThrowsArgumentException)
    {
        VERIFY_THROWS_SPECIFIC(
            validation::ParseNetworkArgument(L"name=\u3000", L"network"), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
                const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkEmptyError(L"network");
                return exception.Message() == expectedMessage;
            });
    }

    TEST_METHOD(CreateCommand_ParseNetworkAliasEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network name=net1,alias= ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
            const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkAliasEmptyError(L"network");
            return exception.Message() == expectedMessage;
        });
    }

    TEST_METHOD(CreateCommand_ParseNetworkEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network \"\" ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    TEST_METHOD(CreateCommand_SetContainerOptionsInvalidNetwork_ThrowsArgumentExceptionWithArgumentName)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --network name=net1,name=net2 ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            wsl::windows::wslc::task::SetContainerOptionsFromArgs(context), wsl::windows::wslc::ArgumentException, [](const auto& exception) {
                const auto expectedMessage = wsl::shared::Localization::WSLCCLI_NetworkDuplicateNameError(L"network");
                return exception.Message() == expectedMessage;
            });
    }

    // Test: Command Line test parsing all cases defined in CommandLineTestCases.h
    // This test verifies the command line parsing logic used by the CLI and executes the same
    // code as the CLI up to the point of command execution, including parsing and argument validtion.
    // It does not actually verify the execution of the command, just that the correct command is
    // found and the provided command line parsed correctly according to the command's defined arguments,
    // and the argument validation rules are correctly applied. The test cases are defined in
    // CommandLineTestCases.h and cover various valid and invalid command lines.
    //
    // Mirrors CoreMain's parser. Environment application is intentionally skipped so test behavior
    // is not affected by the host environment.
    TEST_METHOD(CommandLineParsing_AllCases)
    {
        std::vector<CommandLineTestCase> testCases = {
#define COMMAND_LINE_TEST_CASE(cmdLine, expectedCmd, shouldPass) {cmdLine, expectedCmd, shouldPass},
#include "CommandLineTestCases.h"
#undef COMMAND_LINE_TEST_CASE
        };

        // Run all test cases
        for (const auto& testCase : testCases)
        {
            LogComment(L"Testing: " + testCase.commandLine);

            // Pre-pend executable name, which will get stripped off by CommandLineToArgvW
            auto fullCommandLine = L"wslc " + testCase.commandLine;

            // Process the command line as Windows does.
            int argc = 0;
            auto argv = CommandLineToArgvW(fullCommandLine.c_str(), &argc);
            std::vector<std::wstring> args;
            for (int i = 1; i < argc; ++i)
            {
                args.emplace_back(argv[i]);
            }

            // And now process the command line like WSLC does.
            bool succeeded = true;
            try
            {
                CommandInvocation invocation{std::make_unique<RootCommand>(), std::move(args)};
                CLIExecutionContext context;
                ParseCommandLine(invocation, context);

                // Ensure we found the expected command
                VERIFY_ARE_EQUAL(testCase.expectedCommand, invocation.Selected().Name());
            }
            catch (const CommandException& ce)
            {
                LogComment(L"Command line parsing threw an exception: " + ce.Message());
                succeeded = false;
            }
            catch (...)
            {
                LogComment(L"Command line parsing threw an unexpected exception.");
                succeeded = false;
            }

            VERIFY_ARE_EQUAL(testCase.shouldSucceed, succeeded);
        }
    }
};

class ForEachAsyncUnitTests
{
    WSLC_TEST_CLASS(ForEachAsyncUnitTests)

    TEST_METHOD(ForEachAsync_SuccessCallbackInvokedForAllItems)
    {
        const std::vector<int> items = {1, 2, 3, 4, 5};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [](int item) { return item * 2; },
            [&](int result) { results.push_back(result); },
            [](int /*item*/, wil::ResultException /*error*/) { VERIFY_FAIL(L"Unexpected error"); });

        VERIFY_ARE_EQUAL(items.size(), results.size());
        for (int item : items)
        {
            VERIFY_IS_TRUE(std::find(results.begin(), results.end(), item * 2) != results.end());
        }
    }

    TEST_METHOD(ForEachAsync_ErrorCallbackInvokedOnFailure)
    {
        const std::vector<int> items = {1, 2, 3};
        std::vector<int> failedItems;
        std::vector<int> succeededItems;

        ForEachAsync<int>(
            items,
            [](int item) -> int {
                if (item == 2)
                {
                    THROW_HR(E_FAIL);
                }
                return item;
            },
            [&](int result) { succeededItems.push_back(result); },
            [&](int item, wil::ResultException /*error*/) { failedItems.push_back(item); });

        VERIFY_ARE_EQUAL(1u, failedItems.size());
        VERIFY_ARE_EQUAL(2, failedItems[0]);
        VERIFY_ARE_EQUAL(2u, succeededItems.size());
    }

    TEST_METHOD(ForEachAsync_EmptyInputProducesNoCallbacks)
    {
        const std::vector<int> items;
        bool successCalled = false;
        bool errorCalled = false;

        ForEachAsync<int>(
            items,
            [](int item) { return item; },
            [&](int /*result*/) { successCalled = true; },
            [&](int /*item*/, wil::ResultException /*error*/) { errorCalled = true; });

        VERIFY_IS_FALSE(successCalled);
        VERIFY_IS_FALSE(errorCalled);
    }

    TEST_METHOD(ForEachAsync_BatchSizeOfOneProcessesAllItems)
    {
        const std::vector<int> items = {10, 20, 30, 40, 50};
        std::vector<int> results;

        ForEachAsync<int>(
            items,
            [](int item) { return item; },
            [&](int result) { results.push_back(result); },
            [](int /*item*/, wil::ResultException /*error*/) { VERIFY_FAIL(L"Unexpected error"); },
            /*batchSize=*/1);

        VERIFY_ARE_EQUAL(items.size(), results.size());
        for (int item : items)
        {
            VERIFY_IS_TRUE(std::find(results.begin(), results.end(), item) != results.end());
        }
    }

    TEST_METHOD(ForEachAsync_ErrorInOnErrorPropagatesThrow)
    {
        const std::vector<int> items = {1};

        VERIFY_THROWS_SPECIFIC(
            ForEachAsync<int>(
                items,
                [](int /*item*/) -> int { THROW_HR(E_ACCESSDENIED); },
                [](int /*result*/) {},
                [](int /*item*/, wil::ResultException error) { throw error; }),
            wil::ResultException,
            [](const wil::ResultException& ex) { return ex.GetErrorCode() == E_ACCESSDENIED; });
    }
};

} // namespace WSLCCLIExecutionUnitTests
