cmake_minimum_required(VERSION 3.20)

# Headless: no window, no OpenGL, no X11 development packages. Used for the
# simulation self-test on CI and on machines without a graphics driver.
option(REALISM_HEADLESS "Build the headless self-test target (no window or OpenGL)" OFF)

project(Realism VERSION 3.0.1 LANGUAGES CXX)
if(WIN32 AND NOT REALISM_HEADLESS)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Pick exactly one platform backend.
list(FILTER SOURCES EXCLUDE REGEX "src/platform/(win32|x11|headless)\\.cpp$")
if(REALISM_HEADLESS)
  list(APPEND SOURCES src/platform/headless.cpp)
elseif(WIN32)
  list(APPEND SOURCES src/platform/win32.cpp)
else()
  list(APPEND SOURCES src/platform/x11.cpp)
endif()

if(WIN32 AND NOT REALISM_HEADLESS)
  add_executable(Realism WIN32 ${SOURCES} res/realism.rc)
  target_link_libraries(Realism PRIVATE opengl32 winmm shell32 user32 gdi32 ole32)
  target_compile_definitions(Realism PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN REALISM_VERSION="${PROJECT_VERSION}")
else()
  add_executable(Realism ${SOURCES})
  find_package(Threads REQUIRED)
  target_compile_definitions(Realism PRIVATE REALISM_VERSION="${PROJECT_VERSION}")
  if(REALISM_HEADLESS)
    target_link_libraries(Realism PRIVATE Threads::Threads)
  else()
    find_package(X11 REQUIRED)
    target_link_libraries(Realism PRIVATE ${X11_LIBRARIES} GL Threads::Threads)
  endif()
endif()

if(REALISM_HEADLESS)
  target_compile_definitions(Realism PRIVATE REALISM_HEADLESS=1)
endif()

if(MSVC)
  # /arch:AVX2: the i7-10750H and every Windows PC from the last decade has it.
  target_compile_options(Realism PRIVATE /W3 /O2 /Ob3 /arch:AVX2 /fp:fast /MP /utf-8 /permissive-)
  set_property(TARGET Realism PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(Realism PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers -O3 -mavx2 -mfma -ffast-math -fno-finite-math-only)
  if(WIN32 AND NOT REALISM_HEADLESS)
    # MinGW: static runtime so the .exe runs on its own.
    target_link_options(Realism PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
endif()

# Settings and saves are written next to the executable's data folder at run time.
add_custom_command(TARGET Realism POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/settings.cfg $<TARGET_FILE_DIR:Realism>/settings.cfg)

# Headless builds can be checked with `ctest --test-dir build-headless`.
if(REALISM_HEADLESS)
  enable_testing()
  add_test(NAME realism-selftest COMMAND Realism --selftest)
  set_tests_properties(realism-selftest PROPERTIES TIMEOUT 900)
endif()
