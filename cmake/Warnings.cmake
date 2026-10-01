# Interface target: link it PRIVATE to our own targets only, so third-party code isn't affected.
add_library(lc_warnings INTERFACE)
target_compile_options(lc_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference)
if(LC_WARNINGS_AS_ERRORS)
  target_compile_options(lc_warnings INTERFACE -Werror)
endif()
