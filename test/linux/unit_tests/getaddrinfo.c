/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    getaddrinfo.c

Abstract:

    This file contains tests for getaddrinfo().

--*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "lxtcommon.h"
#include "unittests.h"

#define LXT_NAME "GetAddrInfo"

int LookupHost(const char* Host)
{

    char AddressString[100];
    char* Argv[3];
    char* Envp[1];
    struct addrinfo Hints, *Info;
    void* Ptr = NULL;
    int Result = LXT_RESULT_FAILURE;

    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = PF_UNSPEC;
    Hints.ai_socktype = SOCK_STREAM;
    Hints.ai_flags |= AI_CANONNAME;

    LxtCheckErrno(getaddrinfo(Host, NULL, &Hints, &Info));
    LxtLogInfo("Host: %s", Host);
    while (Info)
    {
        inet_ntop(Info->ai_family, Info->ai_addr->sa_data, AddressString, 100);
        switch (Info->ai_family)
        {
        case AF_INET:
            Ptr = &((struct sockaddr_in*)Info->ai_addr)->sin_addr;
            break;

        case AF_INET6:
            Ptr = &((struct sockaddr_in6*)Info->ai_addr)->sin6_addr;
            break;

        default:
            LxtLogError("ai_family unexpected %d", Info->ai_family);
            goto ErrorExit;
        }

        inet_ntop(Info->ai_family, Ptr, AddressString, 100);
        LxtLogInfo("IPv%d address: %s (%s)", Info->ai_family == PF_INET6 ? 6 : 4, AddressString, Info->ai_canonname);

        Info = Info->ai_next;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return 0;
}

int GetAddrInfoTestEntry(int Argc, char* Argv[])
{

    LXT_ARGS Args;
    int Result = LXT_RESULT_FAILURE;

    LxtCheckErrno(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    if (Argc < 2)
    {
        LxtLogError("Requires HostName as argument");
        goto ErrorExit;
    }

    Result = LookupHost(Argv[1]);

ErrorExit:
    return Result;
}
