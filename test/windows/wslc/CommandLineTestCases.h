/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineTestCases.h

Abstract:

    Test case data for command-line parsing tests.

--*/

// These cases should be for testing valid command lines against the defined commands.
// This executes the command line parsing logic and verifies that the command line is valid
// for the defined commands. It does not actually execute the command.

// X-Macro definition: COMMAND_LINE_TEST_CASE(commandLine, expectedCommand, shouldSucceed)

// Root command tests
COMMAND_LINE_TEST_CASE(L"", L"root", true)
COMMAND_LINE_TEST_CASE(L"--help", L"root", true)

// Diag command tests
COMMAND_LINE_TEST_CASE(L"diag list", L"list", true)
COMMAND_LINE_TEST_CASE(L"diag list -v", L"list", true)
COMMAND_LINE_TEST_CASE(L"diag list --verbose", L"list", true)
COMMAND_LINE_TEST_CASE(L"diag list --verbose --help", L"list", true)
COMMAND_LINE_TEST_CASE(L"diag list --notanarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"diag list extraarg", L"list", false)

// Container command tests
COMMAND_LINE_TEST_CASE(L"container list", L"list", true)
COMMAND_LINE_TEST_CASE(L"container ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"container ps", L"list", true)
COMMAND_LINE_TEST_CASE(L"list", L"list", true)
COMMAND_LINE_TEST_CASE(L"ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"ps", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --session foo", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list -qa", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format json", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format table", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format badformat", L"list", false)

// Error cases
COMMAND_LINE_TEST_CASE(L"invalid command", L"", false)
COMMAND_LINE_TEST_CASE(L"CONTAINER list", L"list", false)               // We are intentionally case-sensitive
COMMAND_LINE_TEST_CASE(L"container LS", L"list", false)                 // commands and aliases are case-sensitive
COMMAND_LINE_TEST_CASE(L"container list --FORMAT json", L"list", false) // Args also case-sensitive
COMMAND_LINE_TEST_CASE(L"container list -A", L"list", false)            // So are arg aliases
