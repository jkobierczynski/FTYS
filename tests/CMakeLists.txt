add_executable(test_io test_io.cpp)
target_link_libraries(test_io PRIVATE ls_core ls_io)
add_test(NAME test_io COMMAND test_io)

if(TARGET ls_proc)
  add_executable(test_proc test_proc.cpp)
  target_link_libraries(test_proc PRIVATE ls_core ls_proc)
  add_test(NAME test_proc COMMAND test_proc)
endif()

if(TARGET ls_gui)
  add_executable(test_integration test_integration.cpp)
  target_link_libraries(test_integration PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Gui Qt6::Concurrent)
  add_test(NAME test_integration COMMAND test_integration)

  # Not part of ctest -- a manual CLI driver for exercising the pipeline
  # against a real capture file with per-stage memory reporting.
  add_executable(manual_pipeline_run manual_pipeline_run.cpp)
  target_link_libraries(manual_pipeline_run PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Gui Qt6::Concurrent)

  add_executable(wavelet_diagnostic wavelet_diagnostic.cpp)
  target_link_libraries(wavelet_diagnostic PRIVATE ls_core ls_proc Qt6::Gui)

  # Not part of ctest -- headless verification that the alignment-inspector
  # PipelineController methods return sensible data before trusting the GUI
  # dialog that wraps them.
  add_executable(inspector_verify inspector_verify.cpp)
  target_link_libraries(inspector_verify PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Gui Qt6::Concurrent)

  # Not part of ctest -- headless real-data validation for the FITS/TIFF/
  # 16-bit export feature: runs the real pipeline, exports every format x
  # bit-depth combination, and round-trips the FITS output through
  # FitsReader to confirm the writer/reader conventions actually agree.
  add_executable(export_validation export_validation.cpp)
  target_link_libraries(export_validation PRIVATE ls_gui ls_core ls_io ls_proc Qt6::Core Qt6::Gui Qt6::Concurrent)
endif()
