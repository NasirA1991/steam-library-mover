cmake_minimum_required(VERSION 3.21)

project(SteamLibraryMover VERSION 0.1.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(SLM_VERSION="${PROJECT_VERSION}")
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)

include(CTest)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Svg Network DBus)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

add_library(slm_core STATIC
    src/core/types.cpp
    src/core/vdf.cpp
    src/core/appmanifest.cpp
    src/core/fileops.cpp
    src/core/mountinfo.cpp
    src/core/scanner.cpp
    src/core/scancache.cpp
    src/core/scancache.h
    src/core/library.cpp
    src/core/filetree.cpp
    src/core/health.cpp
    src/core/health.h
    src/core/process_check.cpp
    src/core/planner.cpp
    src/core/copyengine.cpp
    src/core/journal.cpp
    src/core/executor.cpp
    src/core/doctor.cpp
)

target_include_directories(slm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(slm_core PUBLIC Qt6::Core)

target_compile_options(slm_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(steam-library-mover-cli src/cli/main.cpp)
target_link_libraries(steam-library-mover-cli PRIVATE slm_core)
target_compile_options(steam-library-mover-cli PRIVATE -Wall -Wextra -Wpedantic)

add_library(slm_gui STATIC
    src/gui/mainwindow.cpp
    src/gui/gamemodel.cpp
    src/gui/uiformat.cpp
    src/gui/theme.cpp
    src/gui/theme.h
    src/gui/librarydelegate.cpp
    src/gui/librarydelegate.h
    src/gui/dragsupport.cpp
    src/gui/dragsupport.h
    src/gui/sleepinhibitor.cpp
    src/gui/sleepinhibitor.h
    src/gui/desktopnotification.cpp
    src/gui/desktopnotification.h
    src/gui/moveprogressdialog.cpp
    src/gui/moveprogressdialog.h
    src/gui/artworkprovider.cpp
    src/gui/artworkprovider.h
    src/gui/workers.h
)
target_link_libraries(slm_gui PUBLIC slm_core Qt6::Widgets Qt6::Svg Qt6::Network Qt6::DBus)
target_compile_options(slm_gui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(steam-library-mover src/gui/main.cpp)
target_link_libraries(steam-library-mover PRIVATE slm_gui)
target_compile_options(steam-library-mover PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS steam-library-mover steam-library-mover-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

install(FILES resources/steam-library-mover.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES resources/icons/steam-library-mover.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)
install(FILES resources/io.github.nasira1991.SteamLibraryMover.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
)
configure_file(resources/steam-library-mover.1.in ${CMAKE_CURRENT_BINARY_DIR}/steam-library-mover.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/steam-library-mover.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/steam-library-mover
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/steam-library-mover
)
