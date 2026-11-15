##
## This file is part of the WeActStudio.U3LogicAnalyzer project.
##
## u3ctl — headless CLI controller for the U3 logic analyzer.
## Talks to libsigrok / libsigrokdecode directly via their C APIs.
## See agent-control-plan.md for the design.
##
## Build:  cmake -B build_u3ctl -S tools/u3ctl && cmake --build build_u3ctl
## (requires the repo's install/ prefix with libsigrok + libsigrokdecode,
##  produced by build.sh steps 1-3)

cmake_minimum_required(VERSION 3.13)
project(u3ctl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)

# The repo's install prefix (built by build.sh steps 1-3).
get_filename_component(U3CTL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../install" ABSOLUTE)
set(U3CTL_PREFIX "${U3CTL_PREFIX}" CACHE PATH "install prefix of the repo")

find_library(SIGROK_LIB NAMES sigrok HINTS "${U3CTL_PREFIX}/lib" REQUIRED)
find_library(SIGROKDECODE_LIB NAMES sigrokdecode HINTS "${U3CTL_PREFIX}/lib" REQUIRED)

add_executable(u3ctl
	u3ctl.c
	capture.c
	streaming.c
	sessionfile.c
	decode.c
	query.c
	render.c
	ipc.c
)

find_package(ZLIB REQUIRED)
target_link_libraries(u3ctl PRIVATE ZLIB::ZLIB)

target_compile_definitions(u3ctl PRIVATE
	U3CTL_PREFIX_PATH="${U3CTL_PREFIX}"
)

target_include_directories(u3ctl PRIVATE
	"${U3CTL_PREFIX}/include"
	${GLIB_INCLUDE_DIRS}
)

target_link_libraries(u3ctl PRIVATE
	"${SIGROK_LIB}"
	"${SIGROKDECODE_LIB}"
	${GLIB_LINK_LIBRARIES}
)

# libsigrok's own install name is @rpath/libsigrok.4.dylib — point the rpath
# at the repo's install prefix so the binary runs without DYLD_* env vars.
set_target_properties(u3ctl PROPERTIES
	BUILD_RPATH "${U3CTL_PREFIX}/lib"
	INSTALL_RPATH "${U3CTL_PREFIX}/lib"
)

install(TARGETS u3ctl RUNTIME DESTINATION bin)
