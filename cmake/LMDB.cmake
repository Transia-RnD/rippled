FetchContent_Declare(
  lmdb
  URL        https://github.com/LMDB/lmdb/archive/refs/tags/LMDB_0.9.31.tar.gz
)
FetchContent_MakeAvailable(lmdb)
if(NOT TARGET lmdb)
  add_library(lmdb STATIC
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c
  )
  target_include_directories(lmdb PUBLIC
    ${lmdb_SOURCE_DIR}/libraries/liblmdb
  )
  set_target_properties(lmdb PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
  )
  if(NOT WIN32)
    target_link_libraries(lmdb PUBLIC pthread)
  endif()
  add_library(NIH::lmdb ALIAS lmdb)
endif()
