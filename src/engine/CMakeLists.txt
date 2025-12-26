# Engine module library
add_library(dsd-neo_dispatch)

target_sources(dsd-neo_dispatch PRIVATE
  ../core/frames/protocol_dispatch.c
  ../core/frames/dispatch/dispatch_dmr.c
  ../core/frames/dispatch/dispatch_dpmr.c
  ../core/frames/dispatch/dispatch_dstar.c
  ../core/frames/dispatch/dispatch_edacs.c
  ../core/frames/dispatch/dispatch_m17.c
  ../core/frames/dispatch/dispatch_nxdn.c
  ../core/frames/dispatch/dispatch_p25p1.c
  ../core/frames/dispatch/dispatch_p25p2.c
  ../core/frames/dispatch/dispatch_provoice.c
  ../core/frames/dispatch/dispatch_x2tdma.c
  ../core/frames/dispatch/dispatch_ysf.c
)

target_include_directories(dsd-neo_dispatch
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_dispatch PUBLIC
  dsd-neo_core
  dsd-neo_proto_dmr
  dsd-neo_proto_dpmr
  dsd-neo_proto_dstar
  dsd-neo_proto_edacs
  dsd-neo_proto_m17
  dsd-neo_proto_nxdn
  dsd-neo_proto_p25
  dsd-neo_proto_provoice
  dsd-neo_proto_x2tdma
  dsd-neo_proto_ysf
)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_dispatch PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(dsd-neo_dispatch PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                               -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                               -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                               -Wtautological-compare)
    elseif(MSVC)
        target_compile_options(dsd-neo_dispatch PRIVATE /W4)
    endif()
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_dispatch PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_dispatch PRIVATE /WX)
        endif()
    endif()
endif()

add_library(dsd-neo_engine)

target_sources(dsd-neo_engine PRIVATE
  engine.c
)

target_include_directories(dsd-neo_engine
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_engine PUBLIC
  dsd-neo_dispatch
  dsd-neo_core
  dsd-neo_runtime
  dsd-neo_io_control
  dsd-neo_io_radio
  dsd-neo_proto_dmr
  dsd-neo_proto_m17
  dsd-neo_proto_p25
)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_engine PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(dsd-neo_engine PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                               -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                               -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                               -Wtautological-compare)
    elseif(MSVC)
        target_compile_options(dsd-neo_engine PRIVATE /W4)
    endif()
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_engine PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_engine PRIVATE /WX)
        endif()
    endif()
endif()
