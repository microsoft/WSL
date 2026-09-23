/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCIoPrimitivesUnitTests.cpp

Abstract:

    This file contains unit tests for the WSLC IO handle and relay primitives.

--*/

#include "precomp.h"
#include "Common.h"
#include "HandleIO.h"
#include "helpers.hpp"
#include "wslutil.h"
#include "HttpHeaderEndDetector.h"

namespace WSLCIoPrimitivesUnitTests {

using wsl::windows::common::io::DockerIORelayHandle;
using wsl::windows::common::io::OverlappedIOHandle;
using wsl::windows::common::io::ReadHandle;
using wsl::windows::common::io::WriteHandle;

class WSLCIoPrimitivesUnitTests
{
    WSLC_TEST_CLASS(WSLCIoPrimitivesUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(LineBasedReader)
    {
        auto runTest = [](bool Crlf, const std::string& Data, const std::vector<std::string>& ExpectedLines) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> lines;
            auto onData = [&](const gsl::span<char>& data) { lines.emplace_back(data.data(), data.size()); };

            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::LineBasedReadHandle>(std::move(readPipe), std::move(onData), Crlf));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::io::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < lines.size(); i++)
            {
                if (i >= ExpectedLines.size())
                {
                    LogError(
                        "Input: '%hs'. Line %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedLines[i] != lines[i])
                {
                    LogError(
                        "Input: '%hs'. Line %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str(),
                        EscapeString(lines[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedLines.size() != lines.size())
            {
                LogError(
                    "Input: '%hs', Number of lines do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedLines.size(),
                    lines.size());
                VERIFY_FAIL();
            }
        };

        runTest(false, "foo\nbar", {"foo", "bar"});
        runTest(false, "foo", {"foo"});
        runTest(false, "\n", {});
        runTest(false, "\n\n", {});
        runTest(false, "\n\r\n", {"\r"});
        runTest(false, "\n\nfoo\nbar", {"foo", "bar"});
        runTest(false, "foo\r\nbar", {"foo\r", "bar"});
        runTest(true, "foo\nbar", {"foo\nbar"});
        runTest(true, "foo\r\nbar", {"foo", "bar"});
        runTest(true, "foo\rbar\nbaz", {"foo\rbar\nbaz"});
        runTest(true, "\r", {"\r"});
    }

    TEST_METHOD(HTTPChunkReader)
    {
        auto runTest = [](const std::string& Data, const std::vector<std::string>& ExpectedChunk) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> chunks;
            auto onData = [&](const gsl::span<char>& data) { chunks.emplace_back(data.data(), data.size()); };

            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::HTTPChunkBasedReadHandle>(std::move(readPipe), std::move(onData)));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::io::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < ExpectedChunk.size(); i++)
            {
                if (i >= chunks.size())
                {
                    LogError(
                        "Input: '%hs': Chunk %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedChunk[i] != chunks[i])
                {
                    LogError(

                        "Input: '%hs': Chunk %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str(),
                        EscapeString(chunks[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedChunk.size() != chunks.size())
            {
                LogError(
                    "Input: '%hs', Number of chunks do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedChunk.size(),
                    chunks.size());
                VERIFY_FAIL();
            }
        };

        runTest("3\r\nfoo\r\n3\r\nbar", {"foo", "bar"});
        runTest("3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n", {"foo", "bar"});
        runTest("1\r\na\r\n\r\n", {"a"});

        runTest("c\r\nlf\nin\r\nchunk\r\n3\r\nEOF", {"lf\nin\r\nchunk", "EOF"});
        runTest("15\r\n\r\nchunkstartingwithlf\r\n3\r\nEOF", {"\r\nchunkstartingwithlf", "EOF"});

        // Validate that invalid chunk sizes fail
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid\r\nInvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4nolf", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("12\nyeseighteenletters", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4invalid\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid\n", {}); }), E_INVALIDARG);
    }

    TEST_METHOD(HTTPChunkReaderSplitReads)
    {
        auto runTest = [](const std::vector<std::string>& Data, const std::vector<std::string>& ExpectedChunk) {
            std::vector<std::string> chunks;
            auto onData = [&](const gsl::span<char>& data) { chunks.emplace_back(data.data(), data.size()); };

            auto reader = std::make_unique<wsl::windows::common::io::HTTPChunkBasedReadHandle>(
                wsl::windows::common::io::HandleWrapper{nullptr}, std::move(onData));

            std::string allData;
            for (const auto& datum : Data)
            {
                size_t currentSize = allData.size();
                allData.append(datum);
                reader->OnRead(gsl::span<char>{&allData[currentSize], datum.size()});
            }

            // Final 0 byte read
            reader->OnRead(gsl::span<char>{nullptr, static_cast<size_t>(0)});

            for (size_t i = 0; i < ExpectedChunk.size(); i++)
            {
                if (i >= chunks.size())
                {
                    LogError(
                        "Input: '%hs': Chunk %zu is missing. Expected: '%hs'",
                        EscapeString(allData).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedChunk[i] != chunks[i])
                {
                    LogError(

                        "Input: '%hs': Chunk %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(allData).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str(),
                        EscapeString(chunks[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedChunk.size() != chunks.size())
            {
                LogError(
                    "Input: '%hs', Number of chunks do not match. Expected: %zu, Actual: %zu",
                    EscapeString(allData).c_str(),
                    ExpectedChunk.size(),
                    chunks.size());
                VERIFY_FAIL();
            }

            LogInfo("HTTPChunkReaderSplitReads success. Input: %hs", EscapeString(allData).c_str());
        };

        runTest({"3\r\nfo", "o\r\n3\r\nbar"}, {"foo", "bar"});
        runTest({"1\r\n", "a\r\n\r\n"}, {"a"});

        runTest({"c\r\nlf\n", "in\r\nchunk\r\n3\r\nEOF"}, {"lf\nin\r\nchunk", "EOF"});
        runTest({"15\r\n\r\nchunkstartingwithlf\r\n", "3\r\nEOF"}, {"\r\nchunkstartingwithlf", "EOF"});

        runTest({"3", "\r\nfoo\r\n3\r\nbar"}, {"foo", "bar"});
        runTest({"3\r\nfoo\r\n3\r\nbar\r\n0", "\r\n\r\n"}, {"foo", "bar"});
    }

    WSLC_TEST_METHOD(WriteHandleContent)
    {
        // Validate that writing to a pipe works as expected.
        {
            const std::string expectedData = "Pipe-test";
            std::vector<char> writeBuffer{expectedData.begin(), expectedData.end()};

            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::string readData;
            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::ReadHandle>(std::move(readPipe), [&](const gsl::span<char>& buffer) {
                if (!buffer.empty())
                {
                    readData.append(buffer.data(), buffer.size());
                }
            }));

            io.AddHandle(std::make_unique<WriteHandle>(std::move(writePipe), writeBuffer));

            io.Run({});

            VERIFY_ARE_EQUAL(expectedData, readData);
        }

        // Validate that writing to files work as expected.
        // Use a large buffer to make sure that overlapped writes correctly handle offsets.
        {
            constexpr size_t fileSize = 50 * 1024 * 1024;

            std::vector<char> writeBuffer(fileSize);
            for (size_t i = 0; i < fileSize; i++)
            {
                writeBuffer[i] = static_cast<char>(i % 251);
            }

            auto outputFile = wil::open_or_create_file(L"write-handle-test", GENERIC_WRITE | GENERIC_READ, 0, nullptr);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                outputFile.reset();
                std::filesystem::remove("write-handle-test");
            });

            wsl::windows::common::io::MultiHandleWait io;
            io.AddHandle(std::make_unique<WriteHandle>(outputFile.get(), writeBuffer));
            io.Run({});

            VERIFY_ARE_NOT_EQUAL(SetFilePointer(outputFile.get(), 0, nullptr, FILE_BEGIN), INVALID_SET_FILE_POINTER);

            LARGE_INTEGER size{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetFileSizeEx(outputFile.get(), &size));
            VERIFY_ARE_EQUAL(static_cast<long long>(fileSize), size.QuadPart);

            std::vector<char> readBuffer(fileSize);
            DWORD bytesRead = 0;
            VERIFY_IS_TRUE(ReadFile(outputFile.get(), readBuffer.data(), static_cast<DWORD>(fileSize), &bytesRead, nullptr));
            VERIFY_ARE_EQUAL(static_cast<DWORD>(fileSize), bytesRead);
            VERIFY_IS_TRUE(readBuffer == writeBuffer);
        }

        // Validate that WriteHandle behaves correctly when its buffer is fully written, and CompleteOnDrained is false.
        {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            PartialHandleRead reader(readPipe.get());

            wsl::windows::common::io::MultiHandleWait io;
            auto writerHandle =
                std::make_unique<WriteHandle>(wsl::windows::common::io::HandleWrapper{std::move(writePipe)}, std::vector<char>{}, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // A reusable writer with nothing queued is Idle, so Run() has no handle to wait on and returns.
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            // First write: a single Push() transitions the writer out of Idle and is delivered.
            std::string first = "first-chunk";
            writer->Push(gsl::make_span(first.data(), first.size()));
            VERIFY_ARE_EQUAL(writer->PendingBytes(), first.size());
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.ExpectConsume(first);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));

            // Reuse: the writer returned to Idle (not Completed) so it is still registered, and several
            // queued Push() calls accumulate and are written in order during the next Run().
            std::string a = "aaa";
            std::string b = "bbbb";
            std::string c = "cc";
            writer->Push(gsl::make_span(a.data(), a.size()));
            writer->Push(gsl::make_span(b.data(), b.size()));
            writer->Push(gsl::make_span(c.data(), c.size()));
            VERIFY_ARE_EQUAL(writer->PendingBytes(), a.size() + b.size() + c.size());
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.ExpectConsume(a + b + c);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));

            // Close the writer.
            writer->SetCompleteOnDrained(true);
            std::string exit = "exit";
            writer->Push(gsl::make_span(exit.data(), exit.size()));

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.Expect(exit);
            reader.ExpectClosed();
        }
    }

    TEST_METHOD(WriteNamedPipeContent)
    {
        using wsl::windows::common::io::HandleWrapper;
        using wsl::windows::common::io::MultiHandleWait;
        using wsl::windows::common::io::WriteNamedPipe;

        auto createServerPipe = [](const std::wstring& name) {
            wil::unique_hfile pipe(CreateNamedPipeW(
                name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
            THROW_LAST_ERROR_IF(!pipe);

            return pipe;
        };

        auto connect = [](const std::wstring& name) {
            for (;;)
            {
                wil::unique_hfile client(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
                if (client)
                {
                    return client;
                }

                const auto error = GetLastError();
                THROW_WIN32_IF(error, error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND);

                THROW_IF_WIN32_BOOL_FALSE(WaitNamedPipeW(name.c_str(), 30 * 1000));
            }
        };

        auto push = [](WriteNamedPipe& writer, std::string& data) { writer.Push(gsl::make_span(data.data(), data.size())); };

        // Scenario 1: a payload queued before any client exists is delivered once a client connects,
        // and PendingBytes() drops to zero after the write drains.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // Connect a client up-front; the writer completes the handshake during Run().
            auto client = connect(name);
            PartialHandleRead reader(client.get());

            std::string expected = "hello-named-pipe";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 2: multiple Push() calls accumulate and are delivered, in order, as a single stream.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            auto client = connect(name);
            PartialHandleRead reader(client.get());

            std::string a = "aaaa";
            std::string b = "bbbbbb";
            std::string c = "cc";
            push(*writer, a);
            push(*writer, b);
            push(*writer, c);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), a.size() + b.size() + c.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(a + b + c);
        }

        // Scenario 3: when the connected client disconnects, the next write fails and the writer
        // reconnects, resuming delivery to a new client without losing the buffered payload.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // Phase 1: the first client connects, reads the first payload, then disconnects (the reader
            // and client are scoped so the reader thread joins before the client handle closes).
            std::string first = "first-payload";
            {
                auto client1 = connect(name);
                PartialHandleRead reader1(client1.get());

                push(*writer, first);
                VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
                reader1.Expect(first);
            }

            // Phase 2: the next write fails against the now-closed client, triggering a reconnect. A
            // second client connects while Run() performs the reconnect and receives the buffered payload.
            std::string second = "second-payload";
            push(*writer, second);

            wil::unique_hfile client2;
            std::thread connector([&]() { client2 = connect(name); });
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            connector.join();

            PartialHandleRead reader2(client2.get());
            reader2.Expect(second);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 4: a writer over an already-connected handle (Connected=true) skips the connection
        // handshake and behaves like a persistent WriteHandle, writing queued data straight to the handle.
        {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            PartialHandleRead reader(readPipe.get());

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{std::move(writePipe)}, false, true);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            std::string expected = "no-reconnect-path";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 5: Validate that the named pipe is connected if constructed with Connected = false.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, false, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            std::string expected = "handshake-without-reconnect";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            // Connect the client after the payload is queued; the writer completes the handshake during Run().
            wil::unique_hfile client;
            std::thread connector([&]() { client = connect(name); });
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            connector.join();

            PartialHandleRead reader(client.get());
            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }
    }

    TEST_METHOD(DockerIORelay)
    {
        using namespace wsl::windows::common::io;

        auto runTest = [](const std::vector<char>& Input, const std::string& ExpectedStdout, const std::string& ExpectedStderr) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);

            auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);
            auto [stderrRead, stderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);

            MultiHandleWait io;

            std::string readStdout;
            std::string readStderr;

            io.AddHandle(std::make_unique<DockerIORelayHandle>(
                std::move(readPipe), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));
            io.AddHandle(std::make_unique<WriteHandle>(std::move(writePipe), Input));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stdoutRead), [&](const auto& buffer) { readStdout.append(buffer.data(), buffer.size()); }));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stderrRead), [&](const auto& buffer) { readStderr.append(buffer.data(), buffer.size()); }));

            io.Run({});

            VERIFY_ARE_EQUAL(ExpectedStdout, readStdout);
            VERIFY_ARE_EQUAL(ExpectedStderr, readStderr);
        };

        auto insert = [](std::vector<char>& buffer, auto fd, const std::string& content) {
            DockerIORelayHandle::MultiplexedHeader header;
            header.Fd = fd;
            header.Length = htonl(static_cast<uint32_t>(content.size()));

            buffer.insert(buffer.end(), (char*)&header, ((char*)&header) + sizeof(header));
            buffer.insert(buffer.end(), content.begin(), content.end());
        };

        {
            std::vector<char> input;
            insert(input, 1, "foo");
            insert(input, 1, "bar");
            insert(input, 2, "stderr");
            insert(input, 2, "stderrAgain");
            insert(input, 1, "stdOutAgain");

            runTest(input, "foobarstdOutAgain", "stderrstderrAgain");
        }

        {
            std::vector<char> input;
            insert(input, 0, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }

        {
            std::vector<char> input;
            insert(input, 12, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }

        // Validate that behavior is correct if a read spans across multiple streams.
        {
            std::vector<char> input;

            std::string largeStdout(LX_RELAY_BUFFER_SIZE + 150, 'a');
            std::string largeStderr(LX_RELAY_BUFFER_SIZE + 12, 'b');
            insert(input, 1, largeStdout);
            insert(input, 2, largeStderr);
            insert(input, 1, "regularStdout");

            runTest(input, largeStdout + "regularStdout", largeStderr);
        }

        // Validate that behavior is correct with various input sizes.
        {
            const std::string marker1 = "--start--";
            const std::string marker2 = "--end--";

            auto runTest = [&](size_t payloadSize) {
                std::vector<char> input;
                insert(input, 1, marker1);
                insert(input, 1, std::string(payloadSize, 'A'));
                insert(input, 1, marker2);
                const std::string expected = marker1 + std::string(payloadSize, 'A') + marker2;

                auto [inputRead, inputWrite] =
                    wsl::windows::common::wslutil::OpenAnonymousPipe(static_cast<DWORD>(input.size() + 1), true, false);
                auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(4096, true, true);
                auto [stderrRead, stderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(4096, true, true);

                DWORD written = 0;
                THROW_IF_WIN32_BOOL_FALSE(WriteFile(inputWrite.get(), input.data(), static_cast<DWORD>(input.size()), &written, nullptr));
                VERIFY_ARE_EQUAL(written, static_cast<DWORD>(input.size()));

                std::string output;
                MultiHandleWait io;

                io.AddHandle(std::make_unique<DockerIORelayHandle>(
                    std::move(inputRead), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));

                io.AddHandle(std::make_unique<ReadHandle>(std::move(stdoutRead), [&](const auto& buffer) {
                    output.append(buffer.data(), buffer.size());
                    if (output.find(marker2) != std::string::npos)
                    {
                        io.Cancel();
                    }
                }));

                io.Run(std::chrono::seconds(60));

                VERIFY_ARE_EQUAL(expected, output);
            };

            for (const size_t payloadSize : {1, 100, 4096, 8192, 32768, 64036, 65535, 65536, 65537, 65556, 65571, 65572, 65576, 130000})
            {
                runTest(payloadSize);
            }
        }
    }

    TEST_METHOD(RelayHandleLargeBuffer)
    {
        using namespace wsl::windows::common::io;

        auto [srcRead, srcWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);
        auto [dstRead, dstWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);

        // A payload larger than the relay read buffer forces several read -> write cycles through the
        // RelayHandle's reused WriteHandle.
        const std::string payload(LX_RELAY_BUFFER_SIZE * 4 + 123, 'x');

        MultiHandleWait io;

        io.AddHandle(std::make_unique<WriteHandle>(std::move(srcWrite), std::vector<char>(payload.begin(), payload.end())));
        io.AddHandle(std::make_unique<RelayHandle<>>(std::move(srcRead), std::move(dstWrite)));

        // Collect the relayed output.
        std::string output;
        io.AddHandle(std::make_unique<ReadHandle>(
            std::move(dstRead), [&](const gsl::span<char>& buffer) { output.append(buffer.data(), buffer.size()); }));

        io.Run({});

        VERIFY_ARE_EQUAL(payload.size(), output.size());
        VERIFY_IS_TRUE(payload == output);
    }

    TEST_METHOD(HttpHeaderEndDetector)
    {
        // Returns the index of the byte of header end, or -1 if the header never ends.
        const auto headerEndIndex = [](std::string_view input) {
            wsl::windows::common::HttpHeaderEndDetector detector;
            for (size_t i = 0; i < input.size(); i++)
            {
                if (detector.Consume(input[i]))
                {
                    return static_cast<int>(i);
                }
            }

            return -1;
        };

        VERIFY_ARE_EQUAL(3, headerEndIndex("\r\n\r\n"));
        VERIFY_ARE_EQUAL(4, headerEndIndex("a\r\n\r\n"));
        VERIFY_ARE_EQUAL(7, headerEndIndex("a\r\nb\r\n\r\n"));
        VERIFY_ARE_EQUAL(4, headerEndIndex("\r\r\n\r\n"));
        VERIFY_ARE_EQUAL(3, headerEndIndex("\r\n\r\nbody"));

        VERIFY_ARE_EQUAL(-1, headerEndIndex(""));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("Header: value\r\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("HTTP/1.1 200 OK\r\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\r\n\r"));

        // Detection is strict.
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\n\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\r\n\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\n\r\n"));
    }
};
} // namespace WSLCIoPrimitivesUnitTests
