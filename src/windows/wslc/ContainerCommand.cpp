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
    wslc::services::RunOptions runOptions;
    runOptions.TTY = tty;
    runOptions.Interactive = interactive;
    for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
    {
        runOptions.Arguments.push_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
    }

    return containerService.Run(*session, image, runOptions);
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

    return PrintHelp();
}
