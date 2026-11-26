add_executable(DriftShipEngineTest
  DriftShipEngineTest.cpp
  ${REPO_ROOT}/src/game/DriftShipEngine.cpp
  ${REPO_ROOT}/src/game/DriftShipCrew.cpp
)

target_include_directories(DriftShipEngineTest PRIVATE
  ${REPO_ROOT}/src/game
)

target_link_libraries(DriftShipEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DriftShipEngineTest)
