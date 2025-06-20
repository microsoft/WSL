add_appx_target(msixgluepackage
                "wsl.exe"
                "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.in"
                "${BIN}/gluepackage.msix"
                "wsl")