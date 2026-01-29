#include "precomp.h"
#include "ImageCommand.h"
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

int RunListImageCommand()
{
    auto session = OpenCLISession();
    wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
    ULONG count = 0;
    THROW_IF_FAILED(session->ListImages(&images, &count));

    const wchar_t* plural = count == 1 ? L"" : L"s";
    wslutil::PrintMessage(std::format(L"[wslc] Found {} image", count, plural), stdout);

    // Loop over images using begin and end pointers.
    for (auto ptr = images.get(); ptr < images.get() + count; ++ptr)
    {
        const WSLA_IMAGE_INFORMATION& image = *ptr;
        const char* imageName = image.Image;
        auto size = image.Size;

        wprintf(L"%hs (%.2f MB)\n", imageName, static_cast<double>(size) / (1024 * 1024));
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
        return RunListImageCommand();
    }

    if (subverb == L"pull")
    {
        return RunPullImageCommand(commandLine);
    }

    return PrintHelp();
}