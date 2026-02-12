/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Utils.cpp

Abstract:

    This file contains the Utils implementation

--*/
#include "Utils.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"
#include "ExecutionContext.h"
#include <thread>
#include <format>
#include "ImageService.h"
#include "SessionService.h"

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::relay::MultiHandleWait;

void PullImpl(wslc::models::Session& session, const std::string& image)
{
    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console;

    // TODO: Handle terminal resizes.
    class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") Callback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
    {
    public:
        auto MoveToLine(SHORT Line, bool Revert = true)
        {
            if (Line > 0)
            {
                wprintf(L"\033[%iA", Line);
            }

            return wil::scope_exit([Line = Line]() {
                if (Line > 1)
                {
                    wprintf(L"\033[%iB", Line - 1);
                }
            });
        }

        HRESULT OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
        try
        {
            if (Id == nullptr || *Id == '\0') // Print all 'global' statuses on their own line
            {
                wprintf(L"%hs\n", Status);
                m_currentLine++;
                return S_OK;
            }

            auto info = Info();

            auto it = m_statuses.find(Id);
            if (it == m_statuses.end())
            {
                // If this is the first time we see this ID, create a new line for it.
                m_statuses.emplace(Id, m_currentLine);
                wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
                m_currentLine++;
            }
            else
            {
                auto revert = MoveToLine(m_currentLine - it->second);
                wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
            }

            return S_OK;
        }
        CATCH_RETURN();

    private:
        static CONSOLE_SCREEN_BUFFER_INFO Info()
        {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));

            return info;
        }

        std::wstring GenerateStatusLine(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total, const CONSOLE_SCREEN_BUFFER_INFO& Info)
        {
            std::wstring line;
            if (Total != 0)
            {
                line = std::format(L"{} '{}': {}%", Status, Id, Current * 100 / Total);
            }
            else if (Current != 0)
            {
                line = std::format(L"{} '{}': {}s", Status, Id, Current);
            }
            else
            {
                line = std::format(L"{} '{}'", Status, Id);
            }

            // Erase any previously written char on that line.
            while (line.size() < Info.dwSize.X)
            {
                line += L' ';
            }

            return line;
        }

        std::map<std::string, SHORT> m_statuses;
        SHORT m_currentLine = 0;
        ChangeTerminalMode m_terminalMode{GetStdHandle(STD_OUTPUT_HANDLE), false};
    };

    wslc::services::ImageService imageService;
    Callback callback;
    imageService.Pull(session, image, &callback);
}
