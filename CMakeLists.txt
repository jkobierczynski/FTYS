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
find_package(PkgConfig REQUIRED)

pkg_check_modules(OPENCV REQUIRED opencv4)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(CFITSIO REQUIRED cfitsio)
pkg_check_modules(FFTW3 REQUIRED fftw3)
pkg_check_modules(TIFF REQUIRED libtiff-4)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# core library: image buffer, frame source abstraction (no Qt, no GUI deps)
# ---------------------------------------------------------------------------
add_library(ls_core STATIC
  src/core/ImageBuffer.cpp
)
target_include_directories(ls_core PUBLIC src)
target_compile_options(ls_core PRIVATE -Wall -Wextra)

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
target_compile_options(ls_io PRIVATE -Wall -Wextra)

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
  target_include_directories(ls_proc PUBLIC src ${OPENCV_INCLUDE_DIRS} ${FFTW3_INCLUDE_DIRS})
  target_link_libraries(ls_proc PUBLIC ls_core ${OPENCV_LIBRARIES} ${FFTW3_LIBRARIES} Threads::Threads)
  target_link_directories(ls_proc PUBLIC ${OPENCV_LIBRARY_DIRS} ${FFTW3_LIBRARY_DIRS})
  target_compile_options(ls_proc PRIVATE -Wall -Wextra)
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
  target_compile_options(ls_gui PRIVATE -Wall -Wextra)

  # -------------------------------------------------------------------------
  # main application
  # -------------------------------------------------------------------------
  add_executable(ftys src/app/main.cpp)
  target_link_libraries(ftys PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Widgets Qt6::Gui)
else()
  message(STATUS "GUI sources not yet present -- skipping ls_gui/ftys targets")
endif()

# ---------------------------------------------------------------------------
# tests (simple CLI smoke tests, no GUI)
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
