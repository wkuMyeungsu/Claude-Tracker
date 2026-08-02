# GUI 없이 검증 가능한 로직만 모아 테스트한다. 화면이 필요한 코드
# (src/ui, src/app)는 링크하지 않는다 — 계산 로직을 src/core 로 갈라 둔
# 이유가 이것이다.
#
# qt_add_executable() 이 아니라 일반 add_executable() 을 쓴다. 전자는 finalize
# 단계에서 WIN32_EXECUTABLE 을 다시 켜 버려 콘솔이 붙지 않고, 그러면 QTest 의
# PASS/FAIL 출력이 어디에도 보이지 않는다(ctest 는 통과로 뜨는데 이유를 알 수 없음).
# AUTOMOC/AUTORCC 는 최상위에서 전역으로 켜 두었으므로 그대로 적용된다.
add_executable(tst_usagelogic
    UsageLogicTest.cpp
    ${CMAKE_SOURCE_DIR}/src/core/UsageMerger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/QuotaCalibrator.cpp
    ${CMAKE_SOURCE_DIR}/src/data/UsageScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/data/UsageApiClient.cpp
    ${CMAKE_SOURCE_DIR}/src/data/CredentialsReader.cpp
    # 요율 조회가 :/model_pricing.json 을 읽으므로 리소스도 함께 컴파일한다
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_include_directories(tst_usagelogic PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/data
)
target_compile_definitions(tst_usagelogic PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(tst_usagelogic PRIVATE
    Qt${QT_VERSION_MAJOR}::Test
    Qt${QT_VERSION_MAJOR}::Network      # UsageApiClient
    Qt${QT_VERSION_MAJOR}::Concurrent   # UsageScanner
)

add_test(NAME usagelogic COMMAND tst_usagelogic)
