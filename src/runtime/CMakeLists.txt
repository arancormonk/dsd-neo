# Runtime module library
add_library(dsd-neo_runtime)

target_sources(dsd-neo_runtime PRIVATE
  config.cpp
  config_user.cpp
  config_schema.c
  config_expand.c
  exitflag.c
  log.cpp
  mem.cpp
  comp.c
  ring.cpp
  input_ring.cpp
  worker_pool.cpp
  rt_sched.cpp
  unicode.cpp
  cli/args.c
  cli/usage.c
  cli/oneshot_dmr_t3.c
  bootstrap/system.c
  bootstrap/audio.c
  ${CMAKE_CURRENT_BINARY_DIR}/version/git_ver.c
  ui_async_stubs.c
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

# Link platform abstraction library for threading primitives
target_link_libraries(dsd-neo_runtime PUBLIC dsd-neo_platform)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_runtime PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(dsd-neo_runtime PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                               -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                               -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                               -Wtautological-compare)
    elseif(MSVC)
        target_compile_options(dsd-neo_runtime PRIVATE /W4)
    endif()
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_runtime PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_runtime PRIVATE /WX)
        endif()
    endif()
endif()
