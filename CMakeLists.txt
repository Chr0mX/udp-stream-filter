cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# --- Added: OpenCV (core+imgproc) and libjpeg-turbo, statically linked -----
# Configure with the vcpkg static triplet so these get baked into the
# plugin DLL directly, e.g.:
#   cmake --preset windows-x64 ^
#     -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake ^
#     -DVCPKG_TARGET_TRIPLET=x64-windows-static
find_package(OpenCV REQUIRED COMPONENTS core imgproc)

# vcpkg (Windows) exports a CMake config package for libjpeg-turbo; native
# package managers (apt, Homebrew) generally only ship a pkg-config file, so
# fall back to that when the CONFIG package isn't available.
find_package(libjpeg-turbo CONFIG QUIET)
if(libjpeg-turbo_FOUND)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE libjpeg-turbo::turbojpeg-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(turbojpeg REQUIRED IMPORTED_TARGET libturbojpeg)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::turbojpeg)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})

# Match vcpkg's static triplet's CRT linkage (/MT) so there's no
# RuntimeLibrary mismatch at link time, and so the installed plugin needs
# no VC++ redistributable on the end user's machine.
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
# ----------------------------------------------------------------------------

# NOTE: replace src/plugin-main.c with your plugin source file and update
# this line to match its filename.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/udp_stream_filter.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
