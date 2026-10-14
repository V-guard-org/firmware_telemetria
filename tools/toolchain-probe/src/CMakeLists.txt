# Com ESP-IDF, o PlatformIO ignora build_src_filter: quem escolhe as fontes é o CMake.
# Cada env do ESP32 passa -DPROBE_APP=<pasta> via board_build.cmake_extra_args
# (veja platformio.ini) e aqui só a pasta daquele app entra no build.
if(NOT DEFINED PROBE_APP)
    set(PROBE_APP vguard)
endif()

file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/${PROBE_APP}/*.c ${CMAKE_CURRENT_LIST_DIR}/${PROBE_APP}/*.cpp)

if(NOT app_sources)
    message(FATAL_ERROR "Nenhuma fonte em src/${PROBE_APP}/ (PROBE_APP=${PROBE_APP})")
endif()

idf_component_register(SRCS ${app_sources})
