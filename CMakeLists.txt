cmake_minimum_required(VERSION 3.16)
project(FTYS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Qt setup
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 COMPONENTS Core Widgets Gui Concurrent REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
  # NOMINMAX matters here specifically: this codebase calls std::min/
  # std::max/std::clamp throughout (ImageBuffer.h and most of proc/), and
  # <windows.h> -- which Qt or FFmpeg/cfitsio headers can drag in
  # transitively -- defines min/max as macros unless told not to, which
  # breaks every one of those calls. WIN32_LEAN_AND_MEAN just keeps
  # <windows.h> itself cheaper to include. Both are no-ops on Linux/macOS.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
  # MSVC's /std:c++17 (which CMAKE_CXX_STANDARD 17 above translates to)
  # does NOT, on its own, make the __cplusplus preprocessor macro report
  # 201703L -- MSVC leaves it at the ancient 199711L by default for
  # backward compatibility, and only /Zc:__cplusplus opts a translation
  # unit into reporting the real value. Qt6 headers (qcompilerdetection.h)
  # check __cplusplus directly and hard-error with "Qt requires a C++17
  # compiler, and a suitable value for __cplusplus" if it's wrong, even
  # though the code is, in fact, being compiled as C++17. GCC/Clang have
  # no equivalent gap (their __cplusplus is always accurate), so this is
  # a no-op there.
  #
  # /permissive- is a separate, similarly mandatory flag: Qt6 headers
  # detect they're being compiled by cl.exe and hard-error with "On MSVC
  # you must pass the /permissive- option to the compiler" unless it's
  # set, regardless of which Qt kit (MSVC- or MinGW-built) supplied the
  # headers -- it switches cl.exe into standards-conformant mode, which
  # Qt6's template-heavy headers (QAnyStringView, the <=> comparison
  # helpers, etc.) rely on and MSVC's older non-conformant default parser
  # can't handle correctly. Also a no-op on GCC/Clang, which are
  # conformant by default.
  add_compile_options(/Zc:__cplusplus /permissive-)
endif()

# ---------------------------------------------------------------------------
# Third-party dependency discovery.
#
# OpenCV and TIFF are found the same way on every platform: OpenCV ships
# its own CMake config package (OpenCVConfig.cmake) wherever it's installed
# from (apt, vcpkg, or the official Windows SDK build), and CMake itself
# ships a builtin FindTIFF.cmake module -- both resolved via plain
# find_package() below, verified against this repo's apt-installed OpenCV/
# libtiff on Linux.
#
# FFmpeg/cfitsio/fftw3 don't have that luxury: no CMake config package on
# any platform this project has tested, so Linux keeps using pkg-config
# (unchanged from before, still what every `ctest` run here exercises).
# Windows has no pkg-config on a typical toolchain, so those three are
# instead located with plain find_path()/find_library() against
# CMAKE_PREFIX_PATH -- the same mechanism vcpkg's own toolchain file
# relies on for any port that doesn't ship a CMake config package, so it
# works against a vcpkg install without this project having to hardcode
# vcpkg-specific target names it has had no way to verify (this port has
# only been build-tested on Linux -- see docs/DEVELOPMENT.md's Windows
# port notes for what to check first if any of these lookups fail).
# ---------------------------------------------------------------------------
find_package(OpenCV REQUIRED)
find_package(TIFF REQUIRED)

if(WIN32)
  find_path(AVFORMAT_INCLUDE_DIRS NAMES libavformat/avformat.h)
  find_library(AVFORMAT_LIBRARIES NAMES avformat)
  find_path(AVCODEC_INCLUDE_DIRS NAMES libavcodec/avcodec.h)
  find_library(AVCODEC_LIBRARIES NAMES avcodec)
  find_path(AVUTIL_INCLUDE_DIRS NAMES libavutil/avutil.h)
  find_library(AVUTIL_LIBRARIES NAMES avutil)
  find_path(SWSCALE_INCLUDE_DIRS NAMES libswscale/swscale.h)
  find_library(SWSCALE_LIBRARIES NAMES swscale)
  find_path(CFITSIO_INCLUDE_DIRS NAMES fitsio.h)
  find_library(CFITSIO_LIBRARIES NAMES cfitsio)
  find_path(FFTW3_INCLUDE_DIRS NAMES fftw3.h)
  find_library(FFTW3_LIBRARIES NAMES fftw3)

  foreach(_dep AVFORMAT AVCODEC AVUTIL SWSCALE CFITSIO FFTW3)
    if(NOT ${_dep}_INCLUDE_DIRS OR NOT ${_dep}_LIBRARIES)
      message(FATAL_ERROR
        "Could not locate ${_dep} via find_path()/find_library() on CMAKE_PREFIX_PATH. "
        "If building via vcpkg: confirm the matching port is installed for your triplet "
        "and that -DCMAKE_TOOLCHAIN_FILE points at vcpkg's scripts/buildsystems/vcpkg.cmake "
        "-- see the \"Build (Windows)\" section of README.md.")
    endif()
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AVFORMAT REQUIRED libavformat)
  pkg_check_modules(AVCODEC REQUIRED libavcodec)
  pkg_check_modules(AVUTIL REQUIRED libavutil)
  pkg_check_modules(SWSCALE REQUIRED libswscale)
  pkg_check_modules(CFITSIO REQUIRED cfitsio)
  pkg_check_modules(FFTW3 REQUIRED fftw3)
endif()

# GCC/Clang-only warning flags -- MSVC's cl.exe doesn't understand -Wall/
# -Wextra (they're not silently ignored, they error out), so this project's
# usual warnings are opt-in per-compiler rather than unconditional. Applied
# below via ${LS_WARN_FLAGS} instead of the literal flags.
if(MSVC)
  set(LS_WARN_FLAGS "")
else()
  set(LS_WARN_FLAGS -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# core library: image buffer, frame source abstraction (no Qt, no GUI deps)
# ---------------------------------------------------------------------------
add_library(ls_core STATIC
  src/core/ImageBuffer.cpp
)
target_include_directories(ls_core PUBLIC src)
target_compile_options(ls_core PRIVATE ${LS_WARN_FLAGS})

# ---------------------------------------------------------------------------
# io library: SER / AVI / FITS readers
# ---------------------------------------------------------------------------
add_library(ls_io STATIC
  src/io/SerReader.cpp
  src/io/AviReader.cpp
  src/io/FitsReader.cpp
  src/io/FrameSourceFactory.cpp
  src/io/ImageWriter.cpp
)
target_include_directories(ls_io PUBLIC src
  ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS}
  ${CFITSIO_INCLUDE_DIRS} ${TIFF_INCLUDE_DIRS})
target_link_libraries(ls_io PUBLIC ls_core
  ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES}
  ${CFITSIO_LIBRARIES} ${TIFF_LIBRARIES})
target_link_directories(ls_io PUBLIC
  ${AVFORMAT_LIBRARY_DIRS} ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS} ${SWSCALE_LIBRARY_DIRS}
  ${CFITSIO_LIBRARY_DIRS} ${TIFF_LIBRARY_DIRS})
target_compile_options(ls_io PRIVATE ${LS_WARN_FLAGS})

# ---------------------------------------------------------------------------
# proc library: quality, alignment, stacking, drizzle, sharpening, color
# ---------------------------------------------------------------------------
set(LS_PROC_SOURCES
  src/proc/QualityMetric.cpp
  src/proc/FrameSelector.cpp
  src/proc/Alignment.cpp
  src/proc/MultiPointAlignment.cpp
  src/proc/Stacker.cpp
  src/proc/Drizzle.cpp
  src/proc/WaveletSharpen.cpp
  src/proc/RichardsonLucy.cpp
  src/proc/ColorStretch.cpp
  src/proc/ChromaticAberration.cpp
)
set(LS_PROC_READY TRUE)
foreach(_src ${LS_PROC_SOURCES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    set(LS_PROC_READY FALSE)
  endif()
endforeach()

if(LS_PROC_READY)
  add_library(ls_proc STATIC ${LS_PROC_SOURCES})
  # OpenCV_INCLUDE_DIRS/OpenCV_LIBS (mixed-case, LIBS not LIBRARIES) is
  # find_package(OpenCV)'s own naming, not pkg_check_modules's -- see the
  # dependency-discovery comment above. No OpenCV_LIBRARY_DIRS to add:
  # OpenCV_LIBS already resolves to full paths / imported targets.
  target_include_directories(ls_proc PUBLIC src ${OpenCV_INCLUDE_DIRS} ${FFTW3_INCLUDE_DIRS})
  target_link_libraries(ls_proc PUBLIC ls_core ${OpenCV_LIBS} ${FFTW3_LIBRARIES} Threads::Threads)
  target_link_directories(ls_proc PUBLIC ${FFTW3_LIBRARY_DIRS})
  target_compile_options(ls_proc PRIVATE ${LS_WARN_FLAGS})
else()
  message(STATUS "proc sources not yet present -- skipping ls_proc target")
endif()

# ---------------------------------------------------------------------------
# gui library: Qt widgets
# ---------------------------------------------------------------------------
set(LS_GUI_SOURCES
  src/gui/MainWindow.cpp
  src/gui/PreviewWidget.cpp
  src/gui/CurvesWidget.cpp
  src/gui/AlignmentInspectorDialog.cpp
  src/gui/QualityGraphWidget.cpp
  src/gui/QualityInspectorDialog.cpp
  src/gui/ColorAdjustmentDialog.cpp
  src/gui/PipelineController.cpp
  src/gui/ImageConvert.cpp
  src/gui/resources.qrc
)
set(LS_GUI_READY TRUE)
foreach(_src ${LS_GUI_SOURCES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    set(LS_GUI_READY FALSE)
  endif()
endforeach()

if(LS_GUI_READY)
  add_library(ls_gui STATIC ${LS_GUI_SOURCES})
  target_include_directories(ls_gui PUBLIC src)
  target_link_libraries(ls_gui PUBLIC ls_core ls_io ls_proc Qt6::Core Qt6::Widgets Qt6::Gui Qt6::Concurrent)
  target_compile_options(ls_gui PRIVATE ${LS_WARN_FLAGS})

  # -------------------------------------------------------------------------
  # main application
  # -------------------------------------------------------------------------
  set(FTYS_APP_SOURCES src/app/main.cpp)
  if(WIN32)
    # Gives ftys.exe a real icon (assets/ftys.rc -> assets/ftys.ico) instead
    # of the generic default one -- ignored on Linux/macOS, where the app
    # icon comes from assets/ftys_logo.png via the Qt resource file instead.
    list(APPEND FTYS_APP_SOURCES assets/ftys.rc)
  endif()
  add_executable(ftys ${FTYS_APP_SOURCES})
  target_link_libraries(ftys PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Widgets Qt6::Gui)
  if(WIN32)
    # No console window alongside the GUI window. No effect on Linux/macOS.
    set_target_properties(ftys PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
else()
  message(STATUS "GUI sources not yet present -- skipping ls_gui/ftys targets")
endif()

# ---------------------------------------------------------------------------
# tests (simple CLI smoke tests, no GUI)
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
