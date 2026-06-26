add_executable(StatsModelTest
  StatsModelTest.cpp
)

target_include_directories(StatsModelTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(StatsModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsModelTest)
