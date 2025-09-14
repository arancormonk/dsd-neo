##
# Try to find MBE (legacy) or mbelib-neo (modern)
#
# Defines on success:
#  - MBE_FOUND:         System has an MBE-compatible library
#  - MBE_INCLUDE_DIR:   Include directory to use with `#include <mbelib.h>`
#  - MBE_LIBRARY:       Library to link against
#
# This finder supports both the legacy mbelib layout (header at include/mbelib.h,
# library name "mbe"/"libmbe") and the new mbelib-neo layout (header at
# include/mbelib-neo/mbelib.h, library name "mbe-neo").
##

# Prefer mbelib-neo header layout, but accept legacy too. Using PATH_SUFFIXES
# returns the directory that directly contains mbelib.h so that existing
# includes of <mbelib.h> continue to work.
find_path(
  MBE_INCLUDE_DIR
  NAMES mbelib.h
  PATH_SUFFIXES mbelib-neo
)

# Library names in order of preference: modern first, then legacy.
set(_MBE_CANDIDATE_LIB_NAMES
  mbe-neo
  mbe
  libmbe
  # Windows static library name when shared also exists
  mbe-neo-static
)
find_library(MBE_LIBRARY NAMES ${_MBE_CANDIDATE_LIB_NAMES})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MBE DEFAULT_MSG MBE_LIBRARY MBE_INCLUDE_DIR)

mark_as_advanced(MBE_INCLUDE_DIR MBE_LIBRARY)
