# Testing

## Setup

Tests are created and executed using the [Test Authoring and Execution Framework (TAEF)](https://docs.microsoft.com/windows-hardware/drivers/taef/). Once you have successfully built and deployed the WSL application, all you need are the TAEF binaries to begin authoring and running tests. It is best practice to use taef binaries included in the Microsoft.Taef nuget package used to compile the tests. For example: `packages\Microsoft.Taef.10.77.230207002\build\Binaries`

## Executing Tests

Executing tests with TAEF is done by invoking the `TE.exe` binary:

1. Open a command prompt with administrative privileges.
2. Navigate to the subdirectory containing the built test binaries (`bin/<X64|Arm64>/<Debug|Release>/`)
3. Execute the binaries via invoking TE and passing the test dll/s as arguments: `TE.exe test1.dll test2.dll test3.dll`

## Useful **TE.exe** Command Line Parameters for Debugging/Executing Tests

Command Line parameters are passed to `TE.exe` after supplying the target `.dll`:

### **/list**

Lists the individual tests loaded from the test `.dll` passed in:

`TE.exe test1.dll test2.dll /list`

### **/name:\<testname\>**

Specifies a specific test or group of tests, supporting wildcards `*` and `?` to execute (without this, every test will be run on invoke):

`TE.exe test1.dll /name:*HelloWorldTest*`

### **/inproc**

Very useful for debugging via WinDbg, executes tests within the TE.exe process and not the TE.ProcessHost.exe child process:

`TE.exe test1.dll /inproc`

### **/breakOnCreate /breakOnError /breakOnInvoke**

Especially useful for WinDbg debugging when coupled with `/inproc`. They break into the debugger if/on: before instantiating a test class, if a error or test failure is logged, and prior to test method invoking, respectively.

`TE.exe test1.dll /inproc /breakOnCreate /breakOnError /breakOnInvoke`

### **/p:\<paramName\>=\<paramName\>**

Used for passing runtime parameters to test methods, as well as to setup and cleanup methods. Be mindful of the use of quotation marks.

`TE.exe test1.dll /p:"foo=hello" /p:"bar=2"`

These variables can be retrieved in test source code using the following example:

```cpp
    using namespace WEX::Common;
    using namespace WEX::TestExecution;

    String runtimeParamString;
    DWORD fooBar;

    VERIFY_SUCCEEDED(RuntimeParameters::TryGetValue(L"foo", runtimeParamString));
    VERIFY_SUCCEEDED(RuntimeParameters::TryGetValue(L"bar", fooBar));
```

### **/runas:<\RunAsType\>**

Specifies the environment to run the tests in:

`TE.exe *.dll /runas:<System|Elevated|Restricted|LowIL|AppContainer|etc>`

### **/sessionTime:<\value\>**

Specify a timeout for the **TE.exe** execution, which aborts on timeout.

`TE.exe test1.dll /sessionTimeout:0:0:0.5 // [Day.]Hour[:Minute[:Second[.FractionalSeconds]]`

## Creating Tests

A good example for [how to create tests with TAEF](https://docs.microsoft.com/windows-hardware/drivers/taef/authoring-tests-in-c--) can be found in the `/test/SimpleTests.cpp`, `/test/MountTests.cpp`, and `/test/CMakeLists.txt`.

Make sure to locate the TAEF header file the files at `%\Program Files (x86)\Windows Kits\10\Testing\Development\inc\WexTestClass.h`.

Below is a brief overview:

### Writing the Test

For example, consider the file below, named `ExampleTest.cpp`:

```cpp
    #include "WexTestClass.h" // this included be used for creating TAEF tests classes

    #include "Common.h" // referring to /test/Common.h, where general utility functions for interacting with WSL in regards to testing reside

    #define INLINE_TEST_METHOD_MARKUP // optional, but defined within the directory cmake build instructions. this is the practice that the preexisting tests use

    namespace ExampleTest
    {
        class ExampleTest
        {
            TEST_CLASS(ExampleTest) // define this as a test class

            // add tests via test methods of the test class
            TEST_METHOD(HelloWorldTest) // ExampleTest::ExampleTest::HelloWorldTest
            {
                std::wstring outputExpected = L"Linux on Windows Rocks!\n";
                auto [output, __] = LxsstuLaunchWslAndCaptureOutput(L"echo Linux on Windows Rocks!"); // from /test/Common.h
                VERIFY_ARE_EQUAL(output, outputExpected); // TAEF test method that passes if both are equal, and fails otherwise.
            }
        };
    } //namespace ExampleTest
```

For more in-depth examples of writing TAEF tests, check out `/tests/MountTests.cpp` and [Advanced Authoring Tests in C++](https://docs.microsoft.com/windows-hardware/drivers/taef/authoring-tests-in-c--#advanced-authoring-tests-in-c).

## Building Tests

### CMake

For examples on how to get your test/s building within the repo, please view `/test/CMakeLists.txt` for the structure of creating add to the `wsltest.dll`. For additional information on how to use CMake, try [CMake Documentation and Community](https://cmake.org/documentation/).

### Building

Follow the same instructions listed at the root of this repository and build the application as you would regularly.

### Executing

See the parts above for how to run your new test, but if nothing went awry, your shiny new test dll should be placed in the binary directory. Try running it with:

`TE.exe exampletest.dll`

## Existing Tests

To run all existing tests: `TE.exe wsltests.dll`

### SimpleTests

Very basic tests focusing on the connection to WSL. Tests examine commands like `wsl echo`, `wsl --user`, and `wsl --cd`.

Run these with: `TE.exe wsltests.dll /name:*SimpleTests*`

### MountTests

Tests focusing on the `wsl --mount` functionality. These tests include things like: `--bare` mounting, mounting disk partitions, mounting FAT partitions, etc.

Run these with: `TE.exe wsltests.dll /name:*MountTests*`
### NetworkTests

Tests focusing on the networking aspects of WSL. These are also used to test certain functionality like WSL configurations related to networking, mirrored networking, flow steering, etc.

Run these with `TE.exe wsltests.dll /name:*NetworkTests*`
### Plan9Tests

Tests that focus on validating the functionality of the Plan 9 filesystem component of WSL, testing filesystem-related operations like the creation, deletion, and I/O of files and directories.

Run these with: `TE.exe wsltests.dll /name:*Plan9Tests*`

### UnitTests

Tests that assess general Linux behavior from within the distribution and the features/changes WSL has made on the Linux side. This includes process creation, signals, sockets, etc.
The individual tests are located under `linux/unit_test/*.c` with the exception of `systemd` tests, which are defined in the `windows/UnitTests.cpp`.

Run all unit tests with: `TE.exe wsltests.dll /name:*UnitTests*`

To run only `systemd` tests, use: `TE.exe wsltests.dll /name:UnitTests::UnitTests::Systemd*`
