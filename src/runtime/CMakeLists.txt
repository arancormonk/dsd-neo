# Runtime module library
add_library(dsd-neo_runtime)

target_sources(dsd-neo_runtime PRIVATE
  config.cpp
  log.cpp
  mem.cpp
  ring.cpp
  input_ring.cpp
  worker_pool.cpp
  rt_sched.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/version/git_ver.c
)

# Generate git version source within this target's build dir
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/version)
configure_file(
  ${PROJECT_SOURCE_DIR}/src/runtime/version/git_ver.c.in
  ${CMAKE_CURRENT_BINARY_DIR}/version/git_ver.c
  @ONLY)

target_include_directories(dsd-neo_runtime
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)
