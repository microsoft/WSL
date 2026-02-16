/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentDefinitions.h

Abstract:

    Declaration of the available Arguments with their base properties.

--*/
#pragma once

// Here is where base argument types are defined, with their name, alias, kind, and default description.
// The description can be overridden by commands if a particular command needs a different description but otherwise the
// same argument type definition. The ArgType enum and the mapping of ArgType to data type are generated from this X-Macro, so all
// arguments must be defined here to be used in the system. The arguments defined here are the basis for all commands,
// but not all arguments need to be used by all commands, and additional properties of the arguments can be set in the command's
// GetArguments function when creating the Argument with Argument::Create.

// The Kind determines the data type:
// - Kind::Flag       -> bool
// - Kind::Value      -> std::wstring
// - Kind::Positional -> std::wstring
// - Kind::Forward    -> std::vector<std::wstring>

// No other files other than ArgumentValidation need to be changed when adding a new argument, and that is only
// if you wish to add validation for the new argument or have it use existing validation.

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Kind, Desc)
// clang-format off
#define WSLC_ARGUMENTS(_) \
_(ContainerId,    "container-id",        NO_ALIAS,          Kind::Positional,  L"Specify the target container by its ID") \
_(ForwardArgs,    "forwardargs",         NO_ALIAS,          Kind::Forward,     L"Args to pass along") \
_(Help,           "help",                WSLC_CLI_HELP_ARG, Kind::Flag,        Localization::WSLCCLI_HelpArgDescription()) \
_(Info,           "info",                NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_InfoArgDescription()) \
_(Publish,        "publish",             L"p",              Kind::Value,       L"Publish port") \
_(Verbose,        "verbose",             L"v",              Kind::Flag,        L"Output verbose details")
// clang-format on