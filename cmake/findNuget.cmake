function(find_nuget_package name var_name path)

    string(JSON "${var_name}_VERSION" GET ${NUGET_PACKAGES_JSON} "${name}")

    set(${var_name}_SOURCE_DIR "${CMAKE_BINARY_DIR}/packages/${name}.${${var_name}_VERSION}${path}" PARENT_SCOPE)
    set(${var_name}_VERSION "${${var_name}_VERSION}" PARENT_SCOPE)
endfunction()

function (restore_nuget_packages)

    # Fetch nuget.exe
    FILE(DOWNLOAD
        https://dist.nuget.org/win-x86-commandline/v5.10.0/nuget.exe
        ${CMAKE_BINARY_DIR}/_deps/nuget.exe
        EXPECTED_HASH SHA256=852b71cc8c8c2d40d09ea49d321ff56fd2397b9d6ea9f96e532530307bbbafd3)

    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/packages.config
        ${CMAKE_CURRENT_LIST_DIR}/nuget.config)

    # Restore nuget packages
    execute_process(COMMAND
        ${CMAKE_BINARY_DIR}/_deps/nuget.exe restore packages.config -SolutionDirectory ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function (parse_nuget_packages_versions)

    # Parse the list of available packages
    set(CMD "$packages=@{}; (Select-Xml -path ${CMAKE_SOURCE_DIR}/packages.config /packages).Node.ChildNodes | Where-Object { $_.name -ne '#whitespace'} | % {$packages.add($_.id, $_.Attributes['version'].Value) }; $packages | ConvertTo-Json | Write-Host")

    execute_process(
        COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive -Command "${CMD}"
        OUTPUT_VARIABLE output
        COMMAND_ERROR_IS_FATAL ANY)

    set(NUGET_PACKAGES_JSON ${output} PARENT_SCOPE)
endfunction()
