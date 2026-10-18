# Ubuntu 22.04 安装或更新： bash service/install.sh

cmake_minimum_required(VERSION 3.16)
project(netdebug-service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(X11 REQUIRED COMPONENTS Xext Xtst Xfixes)
find_package(ZLIB REQUIRED)

find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg)
if(NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIBRARY)
  message(FATAL_ERROR "未找到 libturbojpeg，请安装 libturbojpeg0-dev")
endif()

add_executable(netdebug-service
  src/main.cpp
  src/config.cpp
  src/display.cpp
  src/server.cpp
)

target_include_directories(netdebug-service PRIVATE
  src
  ${TURBOJPEG_INCLUDE_DIR}
  ${X11_INCLUDE_DIR}
)

target_compile_definitions(netdebug-service PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(netdebug-service PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(netdebug-service PRIVATE
  X11::X11
  X11::Xext
  X11::Xtst
  X11::Xfixes
  ${TURBOJPEG_LIBRARY}
  ZLIB::ZLIB
  Threads::Threads
)

include(GNUInstallDirs)
install(TARGETS netdebug-service RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  systemd/netdebug.service
  systemd/netdebug-import.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/netdebug
)
