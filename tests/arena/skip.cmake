# psQuake -- a ctest that reports Skipped (exit 77, SKIP_RETURN_CODE) with a
# reason, for the arena tests when no arena was built.
#   cmake -DPSQ_SKIP_MESSAGE=<text> -P skip.cmake
message("${PSQ_SKIP_MESSAGE}")
cmake_language(EXIT 77)
