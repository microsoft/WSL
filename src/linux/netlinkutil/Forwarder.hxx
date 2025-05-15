// Copyright (C) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include "lxwil.h"
#include "Forwarder.h"
#include "Packet.h"
#include "Syscall.h"

#include <stdio.h>

#define _countof(a) (sizeof(a) / sizeof(*(a)))

template <typename ProcessingFunction, typename ExceptionHandler>
Forwarder<ProcessingFunction, ExceptionHandler>::Forwarder(int SourceFd, int DestinationFd, ProcessingFunction Handler, ExceptionHandler exceptionHandler)
{
    // Create a pipe to signal the thread to stop.
    int pipes[2];
    Syscall(pipe2, pipes, 0);
    TerminateFd = pipes[1];

    Worker = std::thread([=]() {
        try
        {
            wil::unique_fd terminate = pipes[0];
            auto wait_for_fd = [&terminate](int fd, int event) -> bool {
                struct pollfd poll_fds[2];
                poll_fds[0] = {.fd = fd, .events = event, .revents = 0};
                poll_fds[1] = {.fd = terminate.get(), .events = POLLIN, .revents = 0};
                for (;;)
                {
                    const int return_value = poll(poll_fds, _countof(poll_fds), -1);
                    if (return_value < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }

                        throw std::runtime_error(std::string("poll returned ") + std::string(strerror(errno)));
                    }
                    else if (return_value == 0)
                    {
                        continue;
                    }
                    else if (poll_fds[1].revents)
                    {
                        return false;
                    }
                    else if (poll_fds[0].revents & event)
                    {
                        return true;
                    }
                }
            };

            Packet packet;
            for (;;)
            {
                packet.reset();

                // Grow the packet to provide space.
                packet.adjust_tail(Packet::InitialPacketSize);
                if (!wait_for_fd(SourceFd, POLLIN))
                {
                    break;
                }

                int bytes_read = Syscall(read, SourceFd, packet.data(), packet.data_end() - packet.data());

                // Shrink packet to size of data read.
                packet.adjust_tail(bytes_read - (packet.data_end() - packet.data()));

                // If the handler returns true, write the packet to the destination fd.
                if (Handler(packet))
                {
                    if (!wait_for_fd(DestinationFd, POLLOUT))
                    {
                        break;
                    }

                    Syscall(write, DestinationFd, packet.data(), packet.data_end() - packet.data());
                }
            }
        }
        catch (std::exception& e)
        {
            if (!exceptionHandler(e))
            {
                throw;
            }
        }
    });
}

template <typename ProcessingFunction, typename ExceptionHandler>
Forwarder<ProcessingFunction, ExceptionHandler>::~Forwarder()
{
    // Close the write end of the pipe to signal the thread to stop.
    close(TerminateFd);
    if (Worker.joinable())
    {
        Worker.join();
    }
}
