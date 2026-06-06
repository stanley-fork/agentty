#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "maya::maya" for configuration "Release"
set_property(TARGET maya::maya APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(maya::maya PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmaya.a"
  )

list(APPEND _cmake_import_check_targets maya::maya )
list(APPEND _cmake_import_check_files_for_maya::maya "${_IMPORT_PREFIX}/lib/libmaya.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
