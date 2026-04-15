cmake_minimum_required(VERSION 3.5)

project(ClaudeTray VERSION 1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Concurrent)

set(PROJECT_SOURCES
    main.cpp
    usagedata.h
    credentialsreader.h
    credentialsreader.cpp
    usageapiclient.h
    usageapiclient.cpp
    usagescanner.h
    usagescanner.cpp
    statusled.h
    statusled.cpp
    quotapanel.h
    quotapanel.cpp
    trayapp.h
    trayapp.cpp
    usagepopup.h
    usagepopup.cpp
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(ClaudeTray
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    if(ANDROID)
        add_library(ClaudeTray SHARED ${PROJECT_SOURCES})
    else()
        add_executable(ClaudeTray ${PROJECT_SOURCES})
    endif()
endif()

target_link_libraries(ClaudeTray PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets      # QSystemTrayIcon, QMenu, QProgressBar
    Qt${QT_VERSION_MAJOR}::Network      # QNetworkAccessManager
    Qt${QT_VERSION_MAJOR}::Concurrent   # QtConcurrent::run (백그라운드 JSONL 스캔)
)

set_target_properties(ClaudeTray PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS ClaudeTray
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(ClaudeTray)
endif()
