add_executable(lcache-export main.cpp)
target_link_libraries(lcache-export PRIVATE lc_cachesim_lib lc_sim lc_policy lc_ml lc_trace
                      CLI11::CLI11 lc_warnings)
