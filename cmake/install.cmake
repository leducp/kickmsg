# Install rules, pkg-config, and CMake find_package support.
# Included from the top-level CMakeLists.txt.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# --- Headers ---
install(DIRECTORY include/kickmsg
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# --- Library target + export ---
set_target_properties(kickmsg PROPERTIES EXPORT_NAME kickmsg)
install(TARGETS kickmsg EXPORT kickmsgTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# --- pkg-config ---
set(KICKMSG_PC_LIBS "-lkickmsg")
string(REPLACE ";" " -l" KICKMSG_PC_SYSLIBS "${OS_LIBRARIES}")
if(KICKMSG_PC_SYSLIBS)
    set(KICKMSG_PC_SYSLIBS "-l${KICKMSG_PC_SYSLIBS}")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickmsg.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/kickmsg.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kickmsg.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# --- CMake find_package support ---
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickmsgConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/kickmsgConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kickmsg
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/kickmsgConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion
)

install(EXPORT kickmsgTargets
    FILE kickmsgTargets.cmake
    NAMESPACE kickmsg::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kickmsg
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/kickmsgConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/kickmsgConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kickmsg
)
