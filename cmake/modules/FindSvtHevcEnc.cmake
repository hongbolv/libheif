include(LibFindMacros)
libfind_pkg_check_modules(SvtHevcEnc_PKGCONF SvtHevcEnc)

find_path(SvtHevcEnc_INCLUDE_DIR
    NAMES EbApi.h
    HINTS ${SvtHevcEnc_PKGCONF_INCLUDE_DIRS} ${SvtHevcEnc_PKGCONF_INCLUDEDIR}
    PATH_SUFFIXES svt-hevc
)

find_library(SvtHevcEnc_LIBRARY
    NAMES SvtHevcEnc libSvtHevcEnc
    HINTS ${SvtHevcEnc_PKGCONF_LIBRARY_DIRS} ${SvtHevcEnc_PKGCONF_LIBDIR}
)

set(SvtHevcEnc_PROCESS_LIBS SvtHevcEnc_LIBRARY)
set(SvtHevcEnc_PROCESS_INCLUDES SvtHevcEnc_INCLUDE_DIR)
libfind_process(SvtHevcEnc)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SvtHevcEnc
    REQUIRED_VARS
        SvtHevcEnc_INCLUDE_DIR
        SvtHevcEnc_LIBRARIES
)
