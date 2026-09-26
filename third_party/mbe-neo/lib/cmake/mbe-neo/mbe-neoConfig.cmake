# Minimal mbe-neoConfig.cmake generated for local testing
get_filename_component(_mbe_prefix "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
get_filename_component(_mbe_prefix "${_mbe_prefix}" DIRECTORY)
set(MBE_INCLUDE_DIR "${_mbe_prefix}/include" CACHE PATH "fake mbe include dir" FORCE)

add_library(mbe_neo::mbe_shared UNKNOWN IMPORTED)
set_target_properties(mbe_neo::mbe_shared PROPERTIES IMPORTED_LOCATION "${_mbe_prefix}/lib/mbe_neo_fake.lib")

