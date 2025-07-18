project(minizip C)

include(CheckFunctionExists)
include(CheckIncludeFile)

# Find Python executable
find_package(Python3 QUIET)
if(NOT Python3_FOUND)
  find_package(Python QUIET)
endif()

if(Python3_FOUND)
  set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
elseif(Python_FOUND)
  set(PYTHON_EXECUTABLE ${Python_EXECUTABLE})
else()
  # Fallback to just 'python'
  set(PYTHON_EXECUTABLE python)
endif()

# Add custom command to always run the header fix script before building
add_custom_target(fix_minizip_headers
  COMMAND ${CMAKE_COMMAND} -E echo "Running header fix script..."
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fix_headers.py
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Fixing minizip-ng headers"
  VERBATIM
)

add_library(minizip STATIC
  minizip-ng/mz.h
  minizip-ng/compat/ioapi.c
  minizip-ng/compat/ioapi.h
  minizip-ng/compat/unzip.c
  minizip-ng/compat/unzip.h
  minizip-ng/compat/zip.c
  minizip-ng/compat/zip.h
  minizip-ng/mz_crypt.c
  minizip-ng/mz_crypt.h
  minizip-ng/mz_os.c
  minizip-ng/mz_os.h
  minizip-ng/mz_strm.c
  minizip-ng/mz_strm.h
  minizip-ng/mz_strm_buf.c
  minizip-ng/mz_strm_buf.h
# minizip-ng/mz_strm_bzip.c
# minizip-ng/mz_strm_bzip.h
# minizip-ng/mz_strm_libcomp.c
# minizip-ng/mz_strm_libcomp.h
# minizip-ng/mz_strm_lzma.c
# minizip-ng/mz_strm_lzma.h
  minizip-ng/mz_strm_mem.c
  minizip-ng/mz_strm_mem.h
  minizip-ng/mz_strm_os.h
# minizip-ng/mz_strm_pkcrypt.c
# minizip-ng/mz_strm_pkcrypt.h
  minizip-ng/mz_strm_split.c
  minizip-ng/mz_strm_split.h
# minizip-ng/mz_strm_wzaes.c
# minizip-ng/mz_strm_wzaes.h
  minizip-ng/mz_strm_zlib.c
  minizip-ng/mz_strm_zlib.h
# minizip-ng/mz_strm_zstd.c
# minizip-ng/mz_strm_zstd.h
  minizip-ng/mz_zip.c
  minizip-ng/mz_zip.h
  minizip-ng/mz_zip_rw.c
  minizip-ng/mz_zip_rw.h
)

# Make the minizip library depend on the header fix
add_dependencies(minizip fix_minizip_headers)

dolphin_disable_warnings(minizip)

if (UNIX)
  target_sources(minizip PRIVATE
    minizip-ng/mz_os_posix.c
    minizip-ng/mz_strm_os_posix.c
  )
endif()

if (WIN32)
  target_sources(minizip PRIVATE
    minizip-ng/mz_os_win32.c
    minizip-ng/mz_strm_os_win32.c
  )
endif()

# Unused files
#  minizip-ng/minigzip.c
#  minizip-ng/minizip.c
#  minizip-ng/mz_crypt_apple.c
#  minizip-ng/mz_crypt_openssl.c
#  minizip-ng/mz_crypt_winvista.c
#  minizip-ng/mz_crypt_winxp.c

target_include_directories(minizip PUBLIC minizip-ng minizip-ng/compat)

target_compile_definitions(minizip PRIVATE HAVE_ZLIB ZLIB_COMPAT MZ_ZIP_NO_CRYPTO MZ_ZIP_NO_ENCRYPTION)
if (UNIX)
  target_compile_definitions(minizip PRIVATE _POSIX_C_SOURCE=200112L)
  target_compile_definitions(minizip PRIVATE __USE_LARGEFILE64 _LARGEFILE64_SOURCE)
endif()

check_include_file(stdint.h HAVE_STDINT_H)
if (HAVE_STDINT_H)
  target_compile_definitions(minizip PRIVATE HAVE_STDINT_H)
endif()

check_include_file(inttypes.h HAVE_INTTYPES_H)
if (HAVE_INTTYPES_H)
  target_compile_definitions(minizip PRIVATE HAVE_INTTYPES_H)
endif()

check_function_exists(fseeko HAVE_FSEEKO)
if (NOT HAVE_FSEEKO)
  target_compile_definitions(minizip PRIVATE NO_FSEEKO)
endif()

target_link_libraries(minizip PUBLIC ZLIB::ZLIB)

add_library(minizip-ng::minizip-ng ALIAS minizip)
