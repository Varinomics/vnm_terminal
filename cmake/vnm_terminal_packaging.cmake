include(GNUInstallDirs)

set(vnm_terminal_runtime_component vnm_terminal_runtime)

if(APPLE)
    set(vnm_terminal_executable_destination ".")
else()
    set(vnm_terminal_executable_destination "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS vnm_terminal
    BUNDLE
        DESTINATION "."
        COMPONENT "${vnm_terminal_runtime_component}"
    RUNTIME
        DESTINATION "${vnm_terminal_executable_destination}"
        COMPONENT "${vnm_terminal_runtime_component}"
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT "${vnm_terminal_runtime_component}"
)

if(UNIX AND NOT APPLE)
    set(vnm_terminal_linux_runtime_dir
        "${CMAKE_INSTALL_LIBDIR}/vnm_terminal")

    set_target_properties(vnm_terminal PROPERTIES
        INSTALL_RPATH
            "\$ORIGIN/../${vnm_terminal_linux_runtime_dir}"
    )

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/com.varinomics.vnm-terminal.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        COMPONENT "${vnm_terminal_runtime_component}"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/com.varinomics.vnm-terminal.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
        COMPONENT "${vnm_terminal_runtime_component}"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/com.varinomics.vnm-terminal.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        COMPONENT "${vnm_terminal_runtime_component}"
    )
endif()

if(NOT VNM_TERMINAL_BUILD_PACKAGES)
    return()
endif()

if(VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER AND
    NOT VNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS)
    message(FATAL_ERROR
        "VNM_TERMINAL_BUILD_PACKAGES=ON requires packaged MSDF dependencies. "
        "Set VNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS=ON and provide "
        "vnm_msdf_text through CMAKE_PREFIX_PATH.")
endif()

if(WIN32)
    set(QT_DEPLOY_BIN_DIR "${CMAKE_INSTALL_BINDIR}")
    set(QT_DEPLOY_LIB_DIR "${CMAKE_INSTALL_BINDIR}")
    set(QT_DEPLOY_PLUGINS_DIR "${CMAKE_INSTALL_BINDIR}/plugins")
    set(QT_DEPLOY_QML_DIR "${CMAKE_INSTALL_BINDIR}/qml")
elseif(UNIX AND NOT APPLE)
    set(QT_DEPLOY_BIN_DIR "${CMAKE_INSTALL_BINDIR}")
    set(QT_DEPLOY_LIB_DIR "${vnm_terminal_linux_runtime_dir}")
    set(QT_DEPLOY_PLUGINS_DIR "${vnm_terminal_linux_runtime_dir}/plugins")
    set(QT_DEPLOY_QML_DIR "${vnm_terminal_linux_runtime_dir}/qml")
else()
    message(FATAL_ERROR
        "VNM_TERMINAL_BUILD_PACKAGES is supported on Windows and Linux")
endif()

set(vnm_terminal_deploy_plugins qoffscreen)
set(vnm_terminal_deploy_excluded_plugin_types qmltooling)
set(vnm_terminal_deploy_options)
if(UNIX AND NOT APPLE)
    list(APPEND vnm_terminal_deploy_plugins qwayland)
    list(APPEND vnm_terminal_deploy_excluded_plugin_types
        egldeviceintegrations
        generic
        iconengines
        imageformats
        networkinformation
        platforminputcontexts
        platformthemes
        tls
    )
    list(APPEND vnm_terminal_deploy_options
        POST_EXCLUDE_REGEXES
            "^/lib/.*"
            "^/lib64/.*"
            "^/usr/lib/.*"
            "^/usr/lib64/.*"
    )
endif()

qt_generate_deploy_qml_app_script(
    TARGET vnm_terminal
    OUTPUT_SCRIPT vnm_terminal_deploy_script
    EXCLUDE_PLUGIN_TYPES ${vnm_terminal_deploy_excluded_plugin_types}
    INCLUDE_PLUGINS ${vnm_terminal_deploy_plugins}
    NO_TRANSLATIONS
    ${vnm_terminal_deploy_options}
)

string(CONCAT vnm_terminal_install_deploy_code
    "set(QT_DEPLOY_BIN_DIR [==[${QT_DEPLOY_BIN_DIR}]==])\n"
    "set(QT_DEPLOY_LIB_DIR [==[${QT_DEPLOY_LIB_DIR}]==])\n"
    "set(QT_DEPLOY_PLUGINS_DIR [==[${QT_DEPLOY_PLUGINS_DIR}]==])\n"
    "set(QT_DEPLOY_QML_DIR [==[${QT_DEPLOY_QML_DIR}]==])\n"
    "include([==[${vnm_terminal_deploy_script}]==])\n"
)
if(UNIX AND NOT APPLE)
    string(APPEND vnm_terminal_install_deploy_code
        "file(GLOB_RECURSE vnm_terminal_deployed_shared_libraries\n"
        "    LIST_DIRECTORIES false\n"
        "    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${QT_DEPLOY_LIB_DIR}/*.so*\")\n"
        "foreach(vnm_terminal_deployed_shared_library\n"
        "    IN LISTS vnm_terminal_deployed_shared_libraries)\n"
        "    file(CHMOD \"\${vnm_terminal_deployed_shared_library}\"\n"
        "        PERMISSIONS\n"
        "            OWNER_READ OWNER_WRITE OWNER_EXECUTE\n"
        "            GROUP_READ GROUP_EXECUTE\n"
        "            WORLD_READ WORLD_EXECUTE)\n"
        "endforeach()\n"
    )
endif()
install(CODE "${vnm_terminal_install_deploy_code}"
    COMPONENT "${vnm_terminal_runtime_component}"
)

set(CPACK_PACKAGE_NAME "vnm-terminal")
set(CPACK_PACKAGE_VENDOR "Varinomics")
set(CPACK_PACKAGE_CONTACT "Ioannis Makris <imak@imak.gr>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Cross-platform GPU-accelerated terminal emulator")
set(CPACK_PACKAGE_DESCRIPTION
    "vnm_terminal is a cross-platform GPU-accelerated terminal emulator built with Qt Quick.")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/Varinomics/vnm_terminal")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(vnm_terminal_cpack_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(WIN32)
    set(vnm_terminal_cpack_license
        "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${vnm_terminal_cpack_license}"
        COPYONLY)
endif()
set(CPACK_RESOURCE_FILE_LICENSE "${vnm_terminal_cpack_license}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)
set(CPACK_COMPONENTS_ALL "${vnm_terminal_runtime_component}")
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};${vnm_terminal_runtime_component};/")

if(WIN32)
    set(CPACK_GENERATOR WIX)
    set(CPACK_PACKAGE_FILE_NAME
        "vnm_terminal_v${PROJECT_VERSION}_windows_x64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "vnm_terminal")
    set(CPACK_PACKAGE_EXECUTABLES "vnm_terminal" "vnm_terminal")
    set(CPACK_WIX_VERSION 3)
    set(CPACK_WIX_UPGRADE_GUID
        "F2D514D2-2D09-4DDB-A857-B27F65DD8BC0")
    set(CPACK_WIX_INSTALL_SCOPE perUser)
    set(CPACK_WIX_PRODUCT_ICON
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vnm_terminal.ico")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "vnm_terminal")
    set(CPACK_WIX_PROPERTY_ARPCOMMENTS
        "Cross-platform GPU-accelerated terminal emulator")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT
        "https://github.com/Varinomics/vnm_terminal")
    set(CPACK_WIX_PROPERTY_ARPURLUPDATEINFO
        "https://github.com/Varinomics/vnm_terminal/releases")
else()
    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER
        "Ioannis Makris <imak@imak.gr>")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
    set(CPACK_RPM_PACKAGE_RELEASE 2)
    set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
    set(CPACK_RPM_SPEC_MORE_DEFINE
        "%global __requires_exclude ^lib(Qt6|icu).*")
endif()

include(CPack)
