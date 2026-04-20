// Copyright (C) Microsoft Corporation. All rights reserved.

// Test helper: bind(0) -> close -> immediate rebind on the same port.
// Verifies that the port tracker does not break this pattern.
//
// Exit codes:
//   0 — rebind succeeded
//   1 — a syscall failed (error printed to stderr)

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void)
{
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);

    int sock1 = socket(AF_INET, SOCK_STREAM, 0);
    if (sock1 < 0)
    {
        perror("socket");
        return 1;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;

    if (bind(sock1, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }

    if (getsockname(sock1, (struct sockaddr*)&addr, &len) < 0)
    {
        perror("getsockname");
        return 1;
    }
    int port = ntohs(addr.sin_port);

    close(sock1);

    int sock2 = socket(AF_INET, SOCK_STREAM, 0);
    if (sock2 < 0)
    {
        perror("socket 2");
        return 1;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock2, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        printf("Failed to bind second socket to port %d: %m\n", port);
        return 1;
    }

    close(sock2);
    return 0;
}
