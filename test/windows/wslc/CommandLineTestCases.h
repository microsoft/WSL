/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineTestCases.h

Abstract:

    Test case data for command-line parsing tests.
    Uses X-macro pattern for easy maintenance and reuse.

Usage:
    #define COMMAND_LINE_TEST_CASE(commandLine, expectedCommand, shouldSucceed) ...
    #include "CommandLineTestCases.h"
    #undef COMMAND_LINE_TEST_CASE

--*/

// X-Macro definition: COMMAND_LINE_TEST_CASE(commandLine, expectedCommand, shouldSucceed)

// Root command tests
COMMAND_LINE_TEST_CASE(L"", L"root", true)
COMMAND_LINE_TEST_CASE(L"--help", L"root", true)

// Container command tests
COMMAND_LINE_TEST_CASE(L"container create --name test ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"container list", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list --all", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list mycontainer", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list -a mycontainer", L"list", true)
COMMAND_LINE_TEST_CASE(L"container list mycontainer container2 container3 container4", L"list", true)
COMMAND_LINE_TEST_CASE(L"container run ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run container --forward -arguments for the init process", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run container --forward -arguments=\"foo for the\" init proc\"ess", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run -d ubuntu", L"run", false)  // -d requires other args
COMMAND_LINE_TEST_CASE(L"container run -itrm image", L"run", true) // Adjoined alias flags is valid
COMMAND_LINE_TEST_CASE(L"container run -irmt image", L"run", true) // Adjoined multi-character flags in any order is valid
COMMAND_LINE_TEST_CASE(L"container run -rmit image", L"run", true) // Adjoined multi-character flags final permutation
COMMAND_LINE_TEST_CASE(L"container run -p=80:80 -p 8000:8000 --publish 47:47 --publish=99:99 image", L"run", true) // Alias values, multi-use of same value
COMMAND_LINE_TEST_CASE(L"container run -rmitp=80:80 image", L"run", false) // Adjoined must be flags only
COMMAND_LINE_TEST_CASE(L"container start mycontainer", L"start", true)
COMMAND_LINE_TEST_CASE(L"container stop mycontainer", L"stop", true)
COMMAND_LINE_TEST_CASE(L"container exec image -f -o --ward --args", L"exec", true)

// Shorthand tests (no "container" prefix)
COMMAND_LINE_TEST_CASE(L"create --name test ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"list", L"list", true)

// Image command tests
COMMAND_LINE_TEST_CASE(L"image pull ubuntu:latest", L"pull", true)
COMMAND_LINE_TEST_CASE(L"image list", L"list", true)
COMMAND_LINE_TEST_CASE(L"image push myimage", L"push", true)

// Session command tests
COMMAND_LINE_TEST_CASE(L"session list", L"list", true)

// Volume command tests
COMMAND_LINE_TEST_CASE(L"volume create myvolume", L"create", true)
COMMAND_LINE_TEST_CASE(L"volume list", L"list", true)
COMMAND_LINE_TEST_CASE(L"volume delete myvolume", L"delete", true)

// Registry command tests
COMMAND_LINE_TEST_CASE(L"registry login myregistry", L"login", true)
COMMAND_LINE_TEST_CASE(L"registry logout myregistry", L"logout", true)

// Error cases
COMMAND_LINE_TEST_CASE(L"invalid command", L"", false)
COMMAND_LINE_TEST_CASE(L"container invalid", L"", false)
COMMAND_LINE_TEST_CASE(L"container create", L"create", false) // Missing required args
