project(obs-outputs)

option(USE_SSL "Enable rtmps support with OpenSSL" OFF)

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

if (USE_SSL)
	find_package(SSL QUIET)
	find_package(ZLIB QUIET)
endif()

if (SSL_FOUND AND ZLIB_FOUND)
	add_definitions(-DCRYPTO -DUSE_OPENSSL)
	include_directories(${SSL_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS})
else()
	if (USE_SSL)
		message(WARNING "SSL enabled by user, but OpenSSL was not found")
	endif()
	unset(SSL_LIBRARIES)
	unset(ZLIB_LIBRARIES)
	add_definitions(-DNO_CRYPTO)
endif()


if(WIN32)
	set(obs-outputs_PLATFORM_DEPS
		ws2_32
		winmm
		Iphlpapi)
endif()

if(MSVC)
	set(obs-outputs_PLATFORM_DEPS
		${obs-outputs_PLATFORM_DEPS}
		w32-pthreads)
endif()

if(NOT WIN32)
	set_source_files_properties(${obs-outputs_librtmp_SOURCES} PROPERTIES
		COMPILE_FLAGS "-fvisibility=hidden")
endif()

set(obs-outputs_HEADERS
	obs-output-ver.h)
set(obs-outputs_SOURCES
	obs-outputs.c
	rtmp-stream.c
	ffmpeg-helper.c)
	
add_library(obs-outputs MODULE
	${obs-outputs_SOURCES}
	${obs-outputs_HEADER})
target_link_libraries(obs-outputs
	libobs
	${SSL_LIBRARIES}
	${ZLIB_LIBRARIES}
	libff
	${obs-ffmpeg_PLATFORM_DEPS}
	${FFMPEG_LIBRARIES})

install_obs_plugin_with_data(obs-outputs data)
