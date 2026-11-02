# Windows 控制端。在仓库根目录：
# cmake -S win11 -B win11/build -G Ninja -DCMAKE_PREFIX_PATH=<Qt>/6.x/mingw_64
# cmake --build win11/build

cmake_minimum_required(VERSION 3.16)
project(NetDebug LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)

add_executable(NetDebug WIN32
  src/main.cpp
  src/protocol/protocol.h
  src/protocol/protocol.cpp
  src/session/discovery.h
  src/session/discovery.cpp
  src/session/session.h
  src/session/session.cpp
  src/ui/filetree.h
  src/ui/filetree.cpp
  src/ui/screenview.h
  src/ui/screenview.cpp
  src/ui/uploadpicker.h
  src/ui/uploadpicker.cpp
  src/ui/mainwindow.h
  src/ui/mainwindow.cpp
)

target_include_directories(NetDebug PRIVATE src)
target_link_libraries(NetDebug PRIVATE
  Qt6::Widgets
  Qt6::Network
  Qt6::Concurrent
  ole32
  shell32
  uuid
)

if(MINGW)
  target_compile_options(NetDebug PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra)
  target_link_options(NetDebug PRIVATE -static-libgcc -static-libstdc++)
endif()

get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
if(_qmake)
  get_filename_component(_qtbin "${_qmake}" DIRECTORY)
  find_program(WINDEPLOYQT windeployqt HINTS "${_qtbin}")
  if(WINDEPLOYQT)
    add_custom_command(TARGET NetDebug POST_BUILD
      COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime "$<TARGET_FILE:NetDebug>"
      COMMENT "收集 Qt 运行库")
  endif()
endif()

# Qt 官方库本身还依赖 MinGW 运行库。静态链接只覆盖本程序，盖不住这些 dll。
if(MINGW)
  get_filename_component(_mingwbin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(_runtime libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    if(EXISTS "${_mingwbin}/${_runtime}")
      add_custom_command(TARGET NetDebug POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_mingwbin}/${_runtime}" "$<TARGET_FILE_DIR:NetDebug>/${_runtime}")
    endif()
  endforeach()
endif()
