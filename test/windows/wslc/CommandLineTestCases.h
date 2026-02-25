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

// Session command tests
COMMAND_LINE_TEST_CASE(L"session list", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list -v", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose --help", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --notanarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session list extraarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session shell session1", L"shell", true)

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
COMMAND_LINE_TEST_CASE(L"run ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run ubuntu bash -c 'echo Hello World'", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run -it --name foo ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"stop", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal 9", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal SIGALRM", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal sigkill", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 -s KILL", L"stop", true)
COMMAND_LINE_TEST_CASE(L"start cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"container start cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"create ubuntu:latest", L"create", true)
COMMAND_LINE_TEST_CASE(L"container create --name foo ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"exec cont1 echo Hello", L"exec", true)
COMMAND_LINE_TEST_CASE(L"exec cont1", L"exec", false)                                         // Missing required command argument
COMMAND_LINE_TEST_CASE(L"container exec -it cont1 sh -c \"echo a && echo b\"", L"exec", true) // docker exec example
COMMAND_LINE_TEST_CASE(L"kill cont1 --signal sigkill", L"kill", true)
COMMAND_LINE_TEST_CASE(L"container kill cont1 -s KILL", L"kill", true)
COMMAND_LINE_TEST_CASE(L"inspect cont1", L"inspect", true)
COMMAND_LINE_TEST_CASE(L"container inspect cont1", L"inspect", true)
COMMAND_LINE_TEST_CASE(L"delete cont1", L"delete", true)
COMMAND_LINE_TEST_CASE(L"container delete cont1 cont2", L"delete", true)

// Error cases
COMMAND_LINE_TEST_CASE(L"invalid command", L"", false)
COMMAND_LINE_TEST_CASE(L"CONTAINER list", L"list", false)               // We are intentionally case-sensitive
COMMAND_LINE_TEST_CASE(L"container LS", L"list", false)                 // commands and aliases are case-sensitive
COMMAND_LINE_TEST_CASE(L"container list --FORMAT json", L"list", false) // Args also case-sensitive
COMMAND_LINE_TEST_CASE(L"container list -A", L"list", false)            // So are arg aliases
