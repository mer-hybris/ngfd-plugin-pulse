cmake_minimum_required(VERSION 2.6)

project(ngfd-plugin-pulse)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(NGFD_PLUGIN REQUIRED ngf-plugin)
pkg_check_modules(PULSEAUDIO REQUIRED libpulse)
pkg_check_modules(SNDFILE REQUIRED sndfile)

include_directories(${GLIB_INCLUDE_DIRS})
include_directories(${NGFD_PLUGIN_INCLUDE_DIRS})
include_directories(${PULSEAUDIO_INCLDUE_DIRS})
include_directories(${SNDFILE_INCLUDE_DIRS})

add_library(ngfd_pulse SHARED src/pulse.c)

target_link_libraries(ngfd_pulse ${NGFD_PLUGIN_LIBRARIES})
target_link_libraries(ngfd_pulse ${PULSEAUDIO_LIBRARIES})
target_link_libraries(ngfd_pulse ${SNDFILE_LIBRARIES})

install(TARGETS ngfd_pulse LIBRARY DESTINATION lib/ngf)
