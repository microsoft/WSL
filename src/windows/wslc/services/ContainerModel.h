/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.h

Abstract:

    This file contains the ContainerModel definitions

--*/

#pragma once

#include <wslservice.h>
#include <wslaservice.h>
#include <docker_schema.h>
#include <unordered_map>
#include <string>

namespace wsl::windows::wslc::models {

// Map of signal names to WSLASignal enum values
inline static const std::unordered_map<std::wstring, WSLASignal> SignalMap = {
    {L"SIGHUP", WSLASignalSIGHUP},   {L"SIGINT", WSLASignalSIGINT},     {L"SIGQUIT", WSLASignalSIGQUIT},
    {L"SIGILL", WSLASignalSIGILL},   {L"SIGTRAP", WSLASignalSIGTRAP},   {L"SIGABRT", WSLASignalSIGABRT},
    {L"SIGIOT", WSLASignalSIGIOT},   {L"SIGBUS", WSLASignalSIGBUS},     {L"SIGFPE", WSLASignalSIGFPE},
    {L"SIGKILL", WSLASignalSIGKILL}, {L"SIGUSR1", WSLASignalSIGUSR1},   {L"SIGSEGV", WSLASignalSIGSEGV},
    {L"SIGUSR2", WSLASignalSIGUSR2}, {L"SIGPIPE", WSLASignalSIGPIPE},   {L"SIGALRM", WSLASignalSIGALRM},
    {L"SIGTERM", WSLASignalSIGTERM}, {L"SIGTKFLT", WSLASignalSIGTKFLT}, {L"SIGCHLD", WSLASignalSIGCHLD},
    {L"SIGCONT", WSLASignalSIGCONT}, {L"SIGSTOP", WSLASignalSIGSTOP},   {L"SIGTSTP", WSLASignalSIGTSTP},
    {L"SIGTTIN", WSLASignalSIGTTIN}, {L"SIGTTOU", WSLASignalSIGTTOU},   {L"SIGURG", WSLASignalSIGURG},
    {L"SIGXCPU", WSLASignalSIGXCPU}, {L"SIGXFSZ", WSLASignalSIGXFSZ},   {L"SIGVTALRM", WSLASignalSIGVTALRM},
    {L"SIGPROF", WSLASignalSIGPROF}, {L"SIGWINCH", WSLASignalSIGWINCH}, {L"SIGIO", WSLASignalSIGIO},
    {L"SIGPOLL", WSLASignalSIGPOLL}, {L"SIGPWR", WSLASignalSIGPWR},     {L"SIGSYS", WSLASignalSIGSYS},
};

struct ContainerCreateOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
    std::string Name;
};

struct ContainerRunOptions : public ContainerCreateOptions
{
    bool Detach = false;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr ULONG DefaultTimeout = -1;

    int Signal = WSLASignalSIGTERM;
    ULONG Timeout = DefaultTimeout;
};

struct KillContainerOptions
{
    int Signal = WSLASignalSIGKILL;
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLA_CONTAINER_STATE State;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State);
};

struct ExecContainerOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
};
} // namespace wsl::windows::wslc::models
