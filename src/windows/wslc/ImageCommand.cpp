#include "precomp.h"
#include "ImageCommand.h"
#include "Utils.h"
#include "WSLAProcessLauncher.h"
#include <CommandLine.h>
#include <format>
#include "ImageService.h"
#include "TableOutput.h"

namespace wslc::commands
{
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
    wprintf(L"Supported commands for 'wslc image':\n");
    wprintf(L"  list   - List all available images\n");
    wprintf(L"  pull   - Pull a new image\n");
    return 0;
}

int RunPullImageCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3);

    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);

    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    PullImpl(*OpenCLISession(), image);

    return 0;
}

int RunListImageCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 3, true);
    std::string format = "table";
    bool quiet = false;
    parser.AddArgument(Utf8String(format), L"--format", L'f');
    parser.AddArgument(quiet, L"--quiet", L'q');
    parser.Parse();

    wslc::services::ImageService imageServie;
    auto images = imageServie.List();
    if (format == "json")
    {
        for (const services::ImageInformation& image : images)
        {
            wprintf(L"%hs", wsl::shared::ToJson(image).c_str());
        }
    }
    else if (quiet)
    {
        for (const auto& image : images)
        {
            wprintf(L"%hs\n", image.Name.c_str());
        }
    }
    else
    {
        TablePrinter tablePrinter({L"NAME", L"SIZE (MB)"});
        for (const auto& [imageName, size] : images)
        {
            tablePrinter.AddRow({
                std::wstring(imageName.begin(), imageName.end()), 
                std::format(L"{:.2f} MB", static_cast<double>(size) / (1024 * 1024))
            });
        }

        tablePrinter.Print();
    }

    return 0;
}

// Handler for `wslc image` command.
int RunImageCommand(std::wstring_view commandLine)
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

    if (subverb == L"list")
    {
        return RunListImageCommand(commandLine);
    }

    if (subverb == L"pull")
    {
        return RunPullImageCommand(commandLine);
    }

    return PrintHelp();
}
}
