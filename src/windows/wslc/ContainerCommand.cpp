#include "precomp.h"
#include "ContainerCommand.h"
#include "services/ContainerService.h"
#include "Utils.h"
#include "WSLAProcessLauncher.h"
#include <CommandLine.h>
#include <format>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

static int PrintHelp()
{
    wprintf(L"Supported commands for 'wslc container':\n");
    wprintf(L"  run [--interactive|-i] [--tty|-t] <image> [command...]\n");
    return 0;
}

static int RunRunContainerCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3, true);

    bool interactive{};
    bool tty{};
    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);
    parser.AddArgument(interactive, L"--interactive", 'i');
    parser.AddArgument(tty, L"--tty", 't');
    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    auto session = OpenCLISession();

    wslc::services::ContainerService containerService;
    wslc::services::CreateOptions options;
    options.TTY = tty;
    options.Interactive = interactive;
    for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
    {
        options.Arguments.push_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
    }

    return containerService.Run(*session, image, options);
}

static int RunCreateContainerCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3, true);

    bool interactive{};
    bool tty{};
    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);
    parser.AddArgument(interactive, L"--interactive", 'i');
    parser.AddArgument(tty, L"--tty", 't');
    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    auto session = OpenCLISession();

    wslc::services::ContainerService containerService;
    wslc::services::CreateOptions options;
    options.TTY = tty;
    options.Interactive = interactive;
    for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
    {
        options.Arguments.push_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
    }

    auto result = containerService.Create(*session, image, options);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(result.Id));
    return 0;
}

static int RunStartContainerCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3, true);

    bool interactive{};
    std::string id;
    parser.AddPositionalArgument(Utf8String{id}, 0);
    parser.AddArgument(interactive, L"--interactive", 'i');
    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, id.empty());

    auto session = OpenCLISession();

    wslc::services::ContainerService containerService;
    containerService.Start(*session, id);
    return 0;
}

static int RunStopContainerCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3, true);

    std::string id;
    wslc::services::StopContainerOptions options;
    parser.AddPositionalArgument(Utf8String{id}, 0);
    parser.AddArgument(options.Signal, L"--signal", 's');
    parser.AddArgument(options.Timeout, L"--time", 't');
    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, id.empty());

    auto session = OpenCLISession();

    wslc::services::ContainerService containerService;
    containerService.Stop(*session, id, options);
    return 0;
}

int RunContainerCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2, true);

    bool help = false;
    std::wstring subverb;
    parser.AddPositionalArgument(subverb, 0);
    parser.AddArgument(help, L"--help", L'h');
    parser.Parse();

    if (help)
    {
        return PrintHelp();
    }

    if (subverb == L"run")
    {
        return RunRunContainerCommand(commandLine);
    }

    if (subverb == L"create")
    {
        return RunCreateContainerCommand(commandLine);
    }

    if (subverb == L"start")
    {
        return RunStartContainerCommand(commandLine);
    }

    if (subverb == L"stop")
    {
        return RunStopContainerCommand(commandLine);
    }

    return PrintHelp();
}
