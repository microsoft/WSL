#include <iostream>
#include <windows.h>

int wmain(int argc, wchar_t** argv)
{
    bool list = false;
    bool help = false;

    // Simple argument parsing: look for --list or --help
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--list") == 0)
        {
            list = true;
        }
        else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0)
        {
            help = true;
        }
    }

    if (help)
    {
        std::wcout << L"wsladiag - WSLA diagnostics tool\n"
                      L"Usage:\n"
                      L"  wsladiag --list    List WSLA sessions\n"
                      L"  wsladiag --help    Show this help\n";
        return 0;
    }

    if (list)
    {
        std::wcout << L"[wsladiag] --list: placeholder.\n"
                      L"Next step: call WSLA service ListSessions and display sessions.\n";
        return 0;
    }

    // No args → show usage
    std::wcout << L"wsladiag - WSLA diagnostics tool\n"
                  L"Usage:\n"
                  L"  wsladiag --list    List WSLA sessions\n"
                  L"  wsladiag --help    Show this help\n";
    return 0;
}
