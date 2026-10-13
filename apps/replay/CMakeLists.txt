add_executable(lcache-replay main.cpp)
target_link_libraries(lcache-replay PRIVATE lc_server lc_sim lc_policy lc_ml lc_trace
                      lc_cachesim_lib lc_asio CLI11::CLI11 lc_warnings)
