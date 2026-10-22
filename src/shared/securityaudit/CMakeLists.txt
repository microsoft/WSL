# Copyright (C) Microsoft Corporation. All rights reserved.

add_library(wsl_securityaudit_protocol INTERFACE)
target_include_directories(wsl_securityaudit_protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(wsl_securityaudit_protocol INTERFACE nlohmann_json::nlohmann_json)
