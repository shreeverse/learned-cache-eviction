# Applied globally so third-party code (GoogleTest) is instrumented too; TSan in particular
# reports false positives when only part of the program is instrumented.
if(LC_SANITIZER)
  if(LC_SANITIZER STREQUAL "thread" OR LC_SANITIZER STREQUAL "address,undefined"
     OR LC_SANITIZER STREQUAL "address" OR LC_SANITIZER STREQUAL "undefined")
    message(STATUS "Sanitizers: ${LC_SANITIZER}")
  else()
    message(FATAL_ERROR "Unsupported LC_SANITIZER='${LC_SANITIZER}'")
  endif()
  add_compile_options(-fsanitize=${LC_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${LC_SANITIZER})
  if(LC_SANITIZER MATCHES "undefined")
    # Make UBSan findings fail the test instead of just printing a message.
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()
