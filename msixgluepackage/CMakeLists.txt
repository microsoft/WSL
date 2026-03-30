add_appx_target(msixgluepackage
                "wsl.exe"
                "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.in"
                "${BIN_OUTPUT}/gluepackage.msix"
                "wsl")