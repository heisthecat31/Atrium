# Copyright 2026, Atrium contributors.
# SPDX-License-Identifier: MIT
#
# Atrium streaming driver. The build copies this folder to src/xrt/drivers/atrium_stream in the
# pinned Monado tree (patch 0004 adds it to Monado's build) and passes ATRIUM_ALVR_DIR: a folder
# with ALVR's server_core library (alvr_server_core.dll + .dll.lib on Windows,
# libalvr_server_core.so on Linux) and alvr_server_core.h generated from its C API.

if(NOT ATRIUM_ALVR_DIR)
	message(FATAL_ERROR "Atrium stream: set ATRIUM_ALVR_DIR to the built ALVR server_core")
endif()

add_library(alvr_server_core SHARED IMPORTED GLOBAL)
if(WIN32)
	set_target_properties(
		alvr_server_core
		PROPERTIES IMPORTED_LOCATION "${ATRIUM_ALVR_DIR}/alvr_server_core.dll"
			   IMPORTED_IMPLIB "${ATRIUM_ALVR_DIR}/alvr_server_core.dll.lib"
		)
else()
	set_target_properties(
		alvr_server_core PROPERTIES IMPORTED_LOCATION "${ATRIUM_ALVR_DIR}/libalvr_server_core.so"
		)
endif()
target_include_directories(alvr_server_core INTERFACE "${ATRIUM_ALVR_DIR}")

add_library(
	drv_atrium_stream STATIC
	atrium_stream.c
	atrium_stream_controller.c
	atrium_stream_hmd.c
	atrium_stream_interface.h
	atrium_stream_internal.h
	)
target_link_libraries(drv_atrium_stream PRIVATE xrt-interfaces aux_util aux_math aux_os)
target_link_libraries(drv_atrium_stream PUBLIC alvr_server_core)
target_include_directories(drv_atrium_stream PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
