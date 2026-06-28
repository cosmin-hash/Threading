cmake_minimum_required(VERSION 3.16)
project(WorkStealingViz LANGUAGES CXX)

# Qt location is supplied by the builder, not hardcoded, so the project is
# portable. Provide it at configure time via either:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="<path>/Qt/<ver>/<compiler>"
# or by setting the CMAKE_PREFIX_PATH environment variable.
if(DEFINED ENV{CMAKE_PREFIX_PATH} AND NOT DEFINED CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "$ENV{CMAKE_PREFIX_PATH}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# --- Qt (works with either Qt 6 or Qt 5) -----------------------------------
# Find whichever major version is available, then use it via QT_VERSION_MAJOR.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# --- std::thread needs the platform threading library (pthread on Unix) -----
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/MainWindow.cpp        src/MainWindow.h
    src/Canvas.cpp            src/Canvas.h
    src/WorkStealingPool.cpp  src/WorkStealingPool.h
    src/Shapes.cpp            src/Shapes.h
    src/Token.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Threads::Threads
)

# Platform packaging: no console window on Windows, a proper .app on macOS.
# (WIN32_EXECUTABLE is ignored off Windows; MACOSX_BUNDLE is ignored off macOS.)
set_target_properties(${PROJECT_NAME} PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE    TRUE
)

# On Windows, deploy the Qt runtime (DLLs, plugins) next to the executable after
# every build so build/<App>.exe can be launched directly without Qt on PATH.
if(WIN32)
  get_target_property(_qmake_exe Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
  get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
  find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
  if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND "${WINDEPLOYQT_EXECUTABLE}"
              --$<IF:$<CONFIG:Debug>,debug,release>
              --no-translations
              "$<TARGET_FILE:${PROJECT_NAME}>"
      COMMENT "Deploying Qt runtime with windeployqt"
      VERBATIM)
  else()
    message(WARNING "windeployqt not found; Qt DLLs will not be deployed automatically.")
  endif()
endif()
