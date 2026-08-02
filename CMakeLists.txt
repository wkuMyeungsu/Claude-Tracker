# 3.21 이상 필요: qt_generate_deploy_app_script 와
# CMP0087(install(CODE) 안에서 제너레이터 표현식 평가)에 의존한다.
cmake_minimum_required(VERSION 3.21)

project(ClaudeTray VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Concurrent)

# 계층별로 나눠 둔다. core 는 Qt Widgets 에 의존하지 않는 순수 계산이라
# 테스트 타깃이 그대로 링크할 수 있고, ui 는 화면만, data 는 외부 입력만 다룬다.
set(CORE_SOURCES        # 순수 계산 — GUI·네트워크 비의존
    src/core/UsageTypes.h
    src/core/ModelPricing.h
    src/core/ModelPricing.cpp
    src/core/UsageAggregator.h
    src/core/UsageAggregator.cpp
    src/core/UsageMerger.h
    src/core/UsageMerger.cpp
    src/core/QuotaCalibrator.h
    src/core/QuotaCalibrator.cpp
)
set(DATA_SOURCES        # 외부 입력 — 파일·네트워크·자격증명
    src/data/CredentialsReader.h
    src/data/CredentialsReader.cpp
    src/data/UsageApiClient.h
    src/data/UsageApiClient.cpp
    src/data/SessionLogReader.h
    src/data/SessionLogReader.cpp
    src/data/SessionLogWatcher.h
    src/data/SessionLogWatcher.cpp
)
set(UI_SOURCES          # 화면
    src/ui/QuotaGauge.h
    src/ui/QuotaGauge.cpp
    src/ui/UsageWindow.h
    src/ui/UsageWindow.cpp
    src/ui/ToggleSwitch.h
    src/ui/ToggleSwitch.cpp
)
set(APP_SOURCES         # 조립·흐름 제어
    src/app/TrayController.h
    src/app/TrayController.cpp
)

set(PROJECT_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${DATA_SOURCES}
    ${UI_SOURCES}
    ${APP_SOURCES}
    resources/resources.qrc
)

# exe 에 아이콘 리소스를 심는다. 없으면 탐색기/시작 메뉴에서 Windows 기본
# 아이콘으로 표시된다. 시작 메뉴 바로가기도 이 임베드 아이콘을 따라간다.
if(WIN32)
    list(APPEND PROJECT_SOURCES resources/appicon.rc)
    # .rc 가 참조하는 .ico 는 빌드 시스템이 자동으로 추적하지 못한다.
    # 명시하지 않으면 아이콘만 교체했을 때 재컴파일되지 않아
    # exe 에 옛 아이콘이 그대로 남는다.
    set_source_files_properties(resources/appicon.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.ico")
endif()

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

# 업데이트 체크와 User-Agent 가 쓰는 현재 버전. 소스에 하드코딩하면
# 버전을 올릴 때 자기 자신을 최신으로 오인해 알림이 뜨지 않는다.
target_compile_definitions(ClaudeTray PRIVATE APP_VERSION="${PROJECT_VERSION}")

# 계층 간 include 는 "UsageTypes.h" 처럼 파일명만 쓴다. 디렉터리를 옮겨도
# 상대 경로("../core/...")가 줄줄이 깨지지 않게 하기 위함이다.
target_include_directories(ClaudeTray PRIVATE
    src/core
    src/data
    src/ui
    src/app
)

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

# exe 는 반드시 windeployqt 가 Qt DLL 을 푸는 디렉터리(bin)와 같은 곳에 둔다.
# 나누면 exe 가 Qt6Core.dll 을 찾지 못해 실행되지 않는다.
install(TARGETS ClaudeTray
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(ClaudeTray)

    # windeployqt 로 Qt 런타임 DLL 과 플러그인을 함께 설치한다.
    # 이게 없으면 exe 한 개만 패키징돼 Qt 미설치 PC 에서 실행 불가이고,
    # styles/qmodernwindowsstyle.dll 누락으로 UI 가 각지게 렌더링된다.
    #
    # windeployqt/macdeployqt 가 있는 플랫폼에서만 부른다. Linux 에는 대응 도구가
    # 없어 어차피 할 일이 없고, 배포판 Qt(예: Ubuntu 24.04 의 6.4)는
    # OUTPUT_SCRIPT/NO_TRANSLATIONS 인자를 아직 몰라 configure 단계에서
    # "Unexpected arguments" 로 실패한다. CI 의 Linux 잡은 빌드·테스트만 하면 된다.
    if(WIN32 OR APPLE)
        qt_generate_deploy_app_script(
            TARGET ClaudeTray
            OUTPUT_SCRIPT QT_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR
            NO_TRANSLATIONS
        )
        install(SCRIPT ${QT_DEPLOY_SCRIPT})
    endif()
endif()

# windeployqt 는 Qt6*.dll 과 플러그인만 복사하고 MinGW/서드파티 런타임
# (libgcc, libstdc++, libpcre2, harfbuzz ...) 은 빠뜨린다. MSYS2 Qt 의 알려진 한계라
# 이걸 빼면 Qt 미설치 PC 에서 0xC0000135(DLL_NOT_FOUND) 로 즉시 종료된다.
# exe 와 플러그인의 의존성을 재귀적으로 훑어 bin/ 에 함께 설치한다.
if(WIN32 AND MINGW)
    # 컴파일러 런타임 디렉터리와 Qt DLL 디렉터리는 배포판에 따라 다르다.
    #   MSYS2   : 둘 다 <prefix>/bin 으로 동일
    #   공식 Qt : 컴파일러는 C:/Qt/Tools/mingw*/bin, Qt DLL 은 C:/Qt/<ver>/mingw_64/bin
    # 같다고 가정하면 Qt6Core.dll 을 못 찾아 dumpbin 단계에서 실패한다.
    get_filename_component(MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # 플러그인/DLL 원본 위치도 배포판마다 다르므로 Qt 가 알려주는 값을 쓴다.
    #   공식 Qt : <prefix>/plugins
    #   MSYS2   : <prefix>/share/qt6/plugins
    if(DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_PLUGINS)
        get_filename_component(QT_PLUGIN_SRC_DIR
            "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}" ABSOLUTE)
        get_filename_component(QT_BIN_SRC_DIR
            "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" ABSOLUTE)
    else()
        get_filename_component(QT_PLUGIN_SRC_DIR
            "${MINGW_RUNTIME_DIR}/../share/qt6/plugins" ABSOLUTE)
        set(QT_BIN_SRC_DIR "${MINGW_RUNTIME_DIR}")
    endif()
    foreach(_d QT_PLUGIN_SRC_DIR QT_BIN_SRC_DIR)
        if(NOT EXISTS "${${_d}}")
            message(FATAL_ERROR "${_d} not found: ${${_d}}")
        endif()
    endforeach()
    message(STATUS "Qt plugins: ${QT_PLUGIN_SRC_DIR}")
    message(STATUS "Qt bin:     ${QT_BIN_SRC_DIR}")
    message(STATUS "MinGW bin:  ${MINGW_RUNTIME_DIR}")
    install(CODE "
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)
        endif()
        set(_bin \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")

        # 분석은 반드시 '원본'(빌드트리 exe + Qt 설치본)을 대상으로 한다.
        # 설치된 사본을 넣으면 같은 Qt6Core.dll 이 두 경로에서 발견돼
        # 'Multiple conflicting paths' 로 실패한다.
        file(GLOB _staged_qt \"\${_bin}/Qt6*.dll\")
        set(_src_qt \"\")
        foreach(_q IN LISTS _staged_qt)
            get_filename_component(_n \"\${_q}\" NAME)
            list(APPEND _src_qt \"${QT_BIN_SRC_DIR}/\${_n}\")
        endforeach()

        # 배포된 플러그인 루트도 배포판마다 다르므로 두 후보를 모두 본다.
        set(_staged_plugin_root \"\")
        foreach(_cand \"plugins\" \"share/qt6/plugins\")
            if(EXISTS \"\${CMAKE_INSTALL_PREFIX}/\${_cand}\")
                set(_staged_plugin_root \"\${CMAKE_INSTALL_PREFIX}/\${_cand}\")
                break()
            endif()
        endforeach()
        set(_src_plugins \"\")
        if(_staged_plugin_root)
            file(GLOB_RECURSE _staged_plugins \"\${_staged_plugin_root}/*.dll\")
            foreach(_p IN LISTS _staged_plugins)
                string(REPLACE \"\${_staged_plugin_root}\"
                               \"${QT_PLUGIN_SRC_DIR}\" _sp \"\${_p}\")
                list(APPEND _src_plugins \"\${_sp}\")
            endforeach()
        endif()

        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"$<TARGET_FILE:ClaudeTray>\"
            LIBRARIES \${_src_qt}
            MODULES \${_src_plugins}
            RESOLVED_DEPENDENCIES_VAR _resolved
            UNRESOLVED_DEPENDENCIES_VAR _unresolved
            DIRECTORIES \"${QT_BIN_SRC_DIR}\" \"${MINGW_RUNTIME_DIR}\"
            PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
            POST_EXCLUDE_REGEXES \"[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32\"
        )
        foreach(_dep IN LISTS _resolved)
            file(INSTALL DESTINATION \"\${_bin}\" TYPE SHARED_LIBRARY
                 FOLLOW_SYMLINK_CHAIN FILES \"\${_dep}\")
        endforeach()
        list(LENGTH _resolved _n)
        message(STATUS \"Bundled \${_n} runtime dependencies into \${_bin}\")
        if(_unresolved)
            message(WARNING \"Unresolved runtime deps: \${_unresolved}\")
        endif()
    ")
endif()

# ── 단위 테스트 ───────────────────────────────────────────────────────────────
# Qt Test 가 설치돼 있을 때만 켠다. 없으면 조용히 건너뛰므로 배포 빌드에는
# 영향이 없다.  실행:  cd build && ctest --output-on-failure
option(CLAUDETRAY_BUILD_TESTS "Build unit tests" ON)
if(CLAUDETRAY_BUILD_TESTS AND QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Qt Test not found - skipping unit tests")
    endif()
endif()

# CPack Packaging Configuration
set(CPACK_PACKAGE_NAME "claude-tracker")
set(CPACK_PACKAGE_VENDOR "wkuMyeungsu")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Claude Code Usage Monitor Tray Application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ClaudeTray")
set(CPACK_PACKAGE_BINARY_DIR "${CMAKE_BINARY_DIR}")

if(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")

    # 고정 UpgradeCode. 값이 바뀌면 기존 설치본을 덮어쓰지 못하고
    # 버전이 중복 설치되므로 절대 변경하지 말 것.
    set(CPACK_WIX_UPGRADE_GUID "9B5C7BFF-E32D-4B51-A4E1-23F67D7D1234")
    set(CPACK_WIX_ROOT_FEATURE_TITLE "Claude Tracker")
    # 제어판 '프로그램 및 기능' 목록에 표시될 아이콘
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.ico")
    # 시작 메뉴 바로가기 (exe 이름, 표시 이름)
    set(CPACK_PACKAGE_EXECUTABLES "ClaudeTray" "Claude Tracker")
    set(CPACK_CREATE_DESKTOP_LINKS "ClaudeTray")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
