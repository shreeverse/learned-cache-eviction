add_library(lc_trace
  oracle_general_reader.cpp
  csv_reader.cpp
  reader_factory.cpp
  next_access.cpp)
target_include_directories(lc_trace PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_trace PRIVATE lc_warnings)
