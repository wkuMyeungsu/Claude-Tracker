# GUI 없이 검증 가능한 로직만 모아 테스트한다. 트레이 아이콘·팝업이 필요한
# 코드(trayapp/usagepopup/quotapanel)는 링크하지 않는다.
#
# qt_add_executable() 이 아니라 일반 add_executable() 을 쓴다. 전자는 finalize
# 단계에서 WIN32_EXECUTABLE 을 다시 켜 버려 콘솔이 붙지 않고, 그러면 QTest 의
# PASS/FAIL 출력이 어디에도 보이지 않는다(ctest 는 통과로 뜨는데 이유를 알 수 없음).
# AUTOMOC/AUTORCC 는 최상위에서 전역으로 켜 두었으므로 그대로 적용된다.
add_executable(tst_usagelogic
    tst_usagelogic.cpp
    ${CMAKE_SOURCE_DIR}/usagescanner.cpp
    ${CMAKE_SOURCE_DIR}/usagemerge.cpp
    ${CMAKE_SOURCE_DIR}/usagecalibrator.cpp
    ${CMAKE_SOURCE_DIR}/usageapiclient.cpp
    ${CMAKE_SOURCE_DIR}/credentialsreader.cpp
    ${CMAKE_SOURCE_DIR}/resources.qrc      # getPricingForModel 이 읽는 model_pricing.json
)

target_include_directories(tst_usagelogic PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(tst_usagelogic PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(tst_usagelogic PRIVATE
    Qt${QT_VERSION_MAJOR}::Test
    Qt${QT_VERSION_MAJOR}::Network      # UsageApiClient
    Qt${QT_VERSION_MAJOR}::Concurrent   # UsageScanner
)

add_test(NAME usagelogic COMMAND tst_usagelogic)
