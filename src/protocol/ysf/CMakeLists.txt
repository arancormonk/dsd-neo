add_library(dsd-neo_proto_ysf)

target_sources(dsd-neo_proto_ysf PRIVATE
  ysf.c
)

target_include_directories(dsd-neo_proto_ysf
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_proto_ysf PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_core)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_proto_ysf PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_proto_ysf PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_proto_ysf PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_proto_ysf PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_proto_ysf PRIVATE /WX)
    endif()
  endif()
endif()
