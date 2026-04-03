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
COMMAND_LINE_TEST_CASE(L"--version", L"root", true)
COMMAND_LINE_TEST_CASE(L"-v", L"root", true)

// Session command tests
COMMAND_LINE_TEST_CASE(L"session list", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list -v", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose --help", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --notanarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session list extraarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session shell session1", L"shell", true)
COMMAND_LINE_TEST_CASE(L"session shell", L"shell", true)
COMMAND_LINE_TEST_CASE(L"session terminate session1", L"terminate", true)
COMMAND_LINE_TEST_CASE(L"session terminate", L"terminate", true)

// Container command tests
COMMAND_LINE_TEST_CASE(L"container list", L"list", true)
COMMAND_LINE_TEST_CASE(L"container ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"container ps", L"list", true)
COMMAND_LINE_TEST_CASE(L"list", L"list", true)
COMMAND_LINE_TEST_CASE(L"ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"ps", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --no-trunc", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --session foo", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list -qa", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format json", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format table", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --format badformat", L"list", false)
COMMAND_LINE_TEST_CASE(L"run ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run ubuntu bash -c 'echo Hello World'", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run -it --name foo ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run --rm -it --name foo ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"stop", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal 9", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal SIGALRM", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 --signal sigkill", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container stop cont1 -s KILL", L"stop", true)
COMMAND_LINE_TEST_CASE(L"start cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"container start cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"container start --attach cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"container start -a cont", L"start", true)
COMMAND_LINE_TEST_CASE(L"create ubuntu:latest", L"create", true)
COMMAND_LINE_TEST_CASE(L"container create --name foo ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"exec cont1 echo Hello", L"exec", true)
COMMAND_LINE_TEST_CASE(L"exec cont1", L"exec", false)                                         // Missing required command argument
COMMAND_LINE_TEST_CASE(L"container exec -it cont1 sh -c \"echo a && echo b\"", L"exec", true) // docker exec example
COMMAND_LINE_TEST_CASE(L"kill cont1 --signal sigkill", L"kill", true)
COMMAND_LINE_TEST_CASE(L"container kill cont1 -s KILL", L"kill", true)
COMMAND_LINE_TEST_CASE(L"inspect cont1", L"inspect", true)
COMMAND_LINE_TEST_CASE(L"container inspect cont1", L"inspect", true)
COMMAND_LINE_TEST_CASE(L"remove cont1", L"remove", true)
COMMAND_LINE_TEST_CASE(L"container remove cont1 cont2", L"remove", true)
COMMAND_LINE_TEST_CASE(L"rm cont1", L"remove", true)
COMMAND_LINE_TEST_CASE(L"container rm cont1 cont2", L"remove", true)
COMMAND_LINE_TEST_CASE(L"container attach cont", L"attach", true)
COMMAND_LINE_TEST_CASE(L"container attach", L"attach", false)

// Logs command
COMMAND_LINE_TEST_CASE(L"logs cont1", L"logs", true)
COMMAND_LINE_TEST_CASE(L"container logs cont1", L"logs", true)
COMMAND_LINE_TEST_CASE(L"container logs --follow cont1", L"logs", true)
COMMAND_LINE_TEST_CASE(L"container logs cont1 -f", L"logs", true)
COMMAND_LINE_TEST_CASE(L"container logs", L"logs", false)

// Image command
COMMAND_LINE_TEST_CASE(L"image build C:\\context", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --tag test:latest", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --file Dockerfile.custom", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -f -", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test:latest -f Dockerfile.other", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t tag1 -t tag2", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --tag tag1 --tag tag2 --tag tag3", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --build-arg KEY=VALUE", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --build-arg A=1 --build-arg B=2", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test:latest --build-arg KEY=VALUE -f Dockerfile.custom", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -v", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --verbose", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test --build-arg KEY=VALUE --verbose", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build", L"build", false)
COMMAND_LINE_TEST_CASE(L"build C:\\context", L"build", true)
COMMAND_LINE_TEST_CASE(L"build C:\\context -t test", L"build", true)
COMMAND_LINE_TEST_CASE(L"image list", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --no-trunc", L"list", true)
COMMAND_LINE_TEST_CASE(L"images", L"images", true) // Aliased off the root changes the name
COMMAND_LINE_TEST_CASE(L"image ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --format json", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --format badformat", L"list", false)
COMMAND_LINE_TEST_CASE(L"image list -v", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list -q", L"list", true)
COMMAND_LINE_TEST_CASE(L"image pull ubuntu", L"pull", true)
COMMAND_LINE_TEST_CASE(L"pull ubuntu", L"pull", true)

// Settings command
COMMAND_LINE_TEST_CASE(L"settings", L"settings", true)
COMMAND_LINE_TEST_CASE(L"settings reset", L"reset", true)

// Error cases
COMMAND_LINE_TEST_CASE(L"invalid command", L"", false)
COMMAND_LINE_TEST_CASE(L"CONTAINER list", L"list", false)               // We are intentionally case-sensitive
COMMAND_LINE_TEST_CASE(L"container LS", L"list", false)                 // commands and aliases are case-sensitive
COMMAND_LINE_TEST_CASE(L"container list --FORMAT json", L"list", false) // Args also case-sensitive
COMMAND_LINE_TEST_CASE(L"container list -A", L"list", false)            // So are arg aliases
