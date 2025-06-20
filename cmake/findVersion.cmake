function(get_version_impl command var_name)
    execute_process(
        COMMAND powershell.exe
                -NoProfile
                -NonInteractive
                -ExecutionPolicy Bypass
                -Command "$env:Path += ';${GITVERSION_SOURCE_DIR}' ; . .\\tools\\devops\\version_functions.ps1 ; ${command}"
        OUTPUT_VARIABLE OUTPUT_VERSION
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
    string(STRIP "${OUTPUT_VERSION}" OUTPUT_VERSION)
    SET(${var_name} ${OUTPUT_VERSION} PARENT_SCOPE)
endfunction()

function(find_commit_hash var_name)
    get_version_impl("Get-Current-Commit-Hash" "command_output")
    SET(${var_name} ${command_output} PARENT_SCOPE)
endfunction()

function(find_version msix_var_name nuget_var_name)
    get_version_impl("Get-VersionInfo -Nightly $false | ConvertTo-Json" "command_output")
    string(JSON  "${msix_var_name}" GET "${command_output}" MsixVersion)
    string(JSON  "${nuget_var_name}" GET "${command_output}" NugetVersion)

    return(PROPAGATE ${msix_var_name} ${nuget_var_name})
endfunction()