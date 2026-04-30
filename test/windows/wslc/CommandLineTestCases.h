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
COMMAND_LINE_TEST_CASE(L"-?", L"root", true)
COMMAND_LINE_TEST_CASE(L"--version", L"root", true)
COMMAND_LINE_TEST_CASE(L"-v", L"root", true)

// Session command tests
COMMAND_LINE_TEST_CASE(L"session list", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --verbose --help", L"list", true)
COMMAND_LINE_TEST_CASE(L"session list --notanarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session list extraarg", L"list", false)
COMMAND_LINE_TEST_CASE(L"session shell session1", L"shell", true)
COMMAND_LINE_TEST_CASE(L"session shell", L"shell", true)
COMMAND_LINE_TEST_CASE(L"session terminate session1", L"terminate", true)
COMMAND_LINE_TEST_CASE(L"session terminate", L"terminate", true)
COMMAND_LINE_TEST_CASE(L"session enter C:\\storage", L"enter", true)
COMMAND_LINE_TEST_CASE(L"session enter C:\\storage --name my-session", L"enter", true)
COMMAND_LINE_TEST_CASE(L"session enter --name my-session C:\\storage", L"enter", true)
COMMAND_LINE_TEST_CASE(L"session enter", L"enter", false)                        // Missing required storage-path
COMMAND_LINE_TEST_CASE(L"session enter C:\\storage --notanarg", L"enter", false) // Invalid argument
COMMAND_LINE_TEST_CASE(L"session enter --name my-session", L"enter", false)      // Missing required positional before flag

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
COMMAND_LINE_TEST_CASE(L"run --rm -it --entrypoint bash archlinux:latest -c \"echo 123\"", L"run", true)
COMMAND_LINE_TEST_CASE(L"run --rm --entrypoint /bin/bash debian:latest -c ls", L"run", true)
COMMAND_LINE_TEST_CASE(L"run jrottenberg/ffmpeg:4.4-alpine -i http://url/to/media.mp4 -stats", L"run", true)
COMMAND_LINE_TEST_CASE(
    L"run -v ./:/data jrottenberg/ffmpeg:4.4-scratch -stats -i http://www.hevc-10bit.mkv -c:v libx265 -pix_fmt yuv420p10 -t "
    L"5 -f mp4 test.mp4",
    L"run",
    true)
COMMAND_LINE_TEST_CASE(
    L"run -v ./:/data -it jrottenberg/ffmpeg:4.4-scratch -stats -i https://file-examples/file_example_MP4_480_1_5MG.mp4 -c:v "
    L"libx265 -pix_fmt yuv420p10 -t 5 -f mp4 /dataout.mp4",
    L"run",
    true)
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
COMMAND_LINE_TEST_CASE(L"create --workdir /app ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"create -w /app ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"container create --workdir /app ubuntu sh", L"create", true)
COMMAND_LINE_TEST_CASE(L"create --workdir", L"create", false)             // Missing value for --workdir
COMMAND_LINE_TEST_CASE(L"create --workdir \"\" ubuntu", L"create", false) // Empty working directory
COMMAND_LINE_TEST_CASE(L"run --workdir /app ubuntu echo hello", L"run", true)
COMMAND_LINE_TEST_CASE(L"run -w /app ubuntu echo hello", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run --workdir /app ubuntu sh", L"run", true)
COMMAND_LINE_TEST_CASE(L"run --workdir", L"run", false)                        // Missing value for --workdir
COMMAND_LINE_TEST_CASE(L"run --workdir \"\" ubuntu echo hello", L"run", false) // Empty working directory
// DNS tests for container create
COMMAND_LINE_TEST_CASE(L"create --dns 1.1.1.1 ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"create --dns 1.1.1.1 --dns 8.8.8.8 ubuntu", L"create", true) // Multiple --dns values
COMMAND_LINE_TEST_CASE(L"container create --dns-search example.com ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"container create --dns-search example.com --dns-search test.local ubuntu", L"create", true) // Multiple --dns-search values
COMMAND_LINE_TEST_CASE(L"create --dns-option ndots:5 ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"create --dns-option ndots:5 --dns-option timeout:3 ubuntu", L"create", true) // Multiple --dns-option values
COMMAND_LINE_TEST_CASE(L"create --dns 1.1.1.1 --dns-search example.com --dns-option ndots:5 ubuntu", L"create", true) // Combined DNS options
COMMAND_LINE_TEST_CASE(L"create --dns", L"create", false)        // Missing value for --dns
COMMAND_LINE_TEST_CASE(L"create --dns-search", L"create", false) // Missing value for --dns-search
COMMAND_LINE_TEST_CASE(L"create --dns-option", L"create", false) // Missing value for --dns-option
// DNS tests for container run
COMMAND_LINE_TEST_CASE(L"run --dns 1.1.1.1 ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"run --dns 1.1.1.1 --dns 8.8.8.8 ubuntu", L"run", true) // Multiple --dns values
COMMAND_LINE_TEST_CASE(L"container run --dns-search example.com ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run --dns-search example.com --dns-search test.local ubuntu", L"run", true) // Multiple --dns-search values
COMMAND_LINE_TEST_CASE(L"run --dns-option ndots:5 ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"run --dns-option ndots:5 --dns-option timeout:3 ubuntu", L"run", true) // Multiple --dns-option values
COMMAND_LINE_TEST_CASE(L"run --dns 1.1.1.1 --dns-search example.com --dns-option ndots:5 ubuntu", L"run", true) // Combined DNS options
COMMAND_LINE_TEST_CASE(L"run --dns", L"run", false)        // Missing value for --dns
COMMAND_LINE_TEST_CASE(L"run --dns-search", L"run", false) // Missing value for --dns-search
COMMAND_LINE_TEST_CASE(L"run --dns-option", L"run", false) // Missing value for --dns-option
// GPU tests for container run
COMMAND_LINE_TEST_CASE(L"run --gpus all ubuntu", L"run", true)
COMMAND_LINE_TEST_CASE(L"container run --gpus all ubuntu sh", L"run", true)
COMMAND_LINE_TEST_CASE(L"run --gpus invalid ubuntu", L"run", false) // Only 'all' is supported
COMMAND_LINE_TEST_CASE(L"run --gpus", L"run", false)                // Missing value for --gpus
// GPU tests for container create
COMMAND_LINE_TEST_CASE(L"create --gpus all ubuntu", L"create", true)
COMMAND_LINE_TEST_CASE(L"container create --gpus all ubuntu sh", L"create", true)
COMMAND_LINE_TEST_CASE(L"create --gpus none ubuntu", L"create", false) // Only 'all' is supported
COMMAND_LINE_TEST_CASE(L"create --gpus", L"create", false)             // Missing value for --gpus
COMMAND_LINE_TEST_CASE(L"exec cont1 echo Hello", L"exec", true)
COMMAND_LINE_TEST_CASE(L"exec cont1", L"exec", false)                                         // Missing required command argument
COMMAND_LINE_TEST_CASE(L"container exec -it cont1 sh -c \"echo a && echo b\"", L"exec", true) // docker exec example
COMMAND_LINE_TEST_CASE(L"exec --workdir /app cont1 echo Hello", L"exec", true)
COMMAND_LINE_TEST_CASE(L"exec -w /app cont1 echo Hello", L"exec", true)
COMMAND_LINE_TEST_CASE(L"container exec --workdir /app cont1 sh", L"exec", true)
COMMAND_LINE_TEST_CASE(L"exec --workdir", L"exec", false)                       // Missing value for --workdir
COMMAND_LINE_TEST_CASE(L"exec --workdir \"\" cont1 echo Hello", L"exec", false) // Empty working directory
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
COMMAND_LINE_TEST_CASE(L"image build C:\\context --verbose", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test --build-arg KEY=VALUE --verbose", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --no-cache", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context --no-cache --verbose", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build C:\\context -t test --no-cache", L"build", true)
COMMAND_LINE_TEST_CASE(L"image build", L"build", false)
COMMAND_LINE_TEST_CASE(L"build C:\\context", L"build", true)
COMMAND_LINE_TEST_CASE(L"build C:\\context -t test", L"build", true)
COMMAND_LINE_TEST_CASE(L"image list", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --no-trunc", L"list", true)
COMMAND_LINE_TEST_CASE(L"images", L"images", true) // Aliased off the root changes the name
COMMAND_LINE_TEST_CASE(L"image ls", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --format json", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list --format badformat", L"list", false)
COMMAND_LINE_TEST_CASE(L"image list --verbose", L"list", true)
COMMAND_LINE_TEST_CASE(L"image list -q", L"list", true)
COMMAND_LINE_TEST_CASE(L"image pull ubuntu", L"pull", true)
COMMAND_LINE_TEST_CASE(L"pull ubuntu", L"pull", true)

// Version command tests
COMMAND_LINE_TEST_CASE(L"version", L"version", true)
COMMAND_LINE_TEST_CASE(L"version --help", L"version", true)
COMMAND_LINE_TEST_CASE(L"version extraarg", L"version", false)
// Settings command
COMMAND_LINE_TEST_CASE(L"settings", L"settings", true)
COMMAND_LINE_TEST_CASE(L"settings reset", L"reset", true)

// Error cases
COMMAND_LINE_TEST_CASE(L"invalid command", L"", false)
COMMAND_LINE_TEST_CASE(L"CONTAINER list", L"list", false)               // We are intentionally case-sensitive
COMMAND_LINE_TEST_CASE(L"container LS", L"list", false)                 // commands and aliases are case-sensitive
COMMAND_LINE_TEST_CASE(L"container list --FORMAT json", L"list", false) // Args also case-sensitive
COMMAND_LINE_TEST_CASE(L"container list -A", L"list", false)            // So are arg aliases
