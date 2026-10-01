add_library(lc_ml
  feature_tracker.cpp
  predictor.cpp
  recording_predictor.cpp)
target_include_directories(lc_ml PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_ml PUBLIC lc_trace PRIVATE lc_warnings)

if(LC_WITH_LIGHTGBM)
  target_sources(lc_ml PRIVATE lightgbm_predictor.cpp)
  target_link_libraries(lc_ml PRIVATE _lightgbm)
  # Lets the policy factory offer "learned" only when it can be built.
  target_compile_definitions(lc_ml PUBLIC LC_HAVE_LIGHTGBM=1)
endif()
