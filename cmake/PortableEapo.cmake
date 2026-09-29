# Upstream remains pristine. Only selected DSP translation units and their
# relative headers are copied into the build directory, preserving notices.
set(EAPO_PORT ${CMAKE_CURRENT_BINARY_DIR}/eapo-port)
file(MAKE_DIRECTORY ${EAPO_PORT}/filters ${EAPO_PORT}/helpers)
set(EAPO_SOURCES)
configure_file(${EAPO}/FilterConfiguration.cpp
  ${EAPO_PORT}/FilterConfiguration.cpp COPYONLY)
configure_file(${EAPO}/FilterConfiguration.h
  ${EAPO_PORT}/FilterConfiguration.h COPYONLY)
list(APPEND EAPO_SOURCES ${EAPO_PORT}/FilterConfiguration.cpp)
set(EAPO_FILTERS PreampFilter PreampFilterFactory BiQuad BiQuadFilter
    BiQuadFilterFactory IIRFilter IIRFilterFactory DelayFilter DelayFilterFactory
    ChannelFilter ChannelFilterFactory CopyFilter CopyFilterFactory)
if(FFTW3F_FOUND)
  list(APPEND EAPO_FILTERS ConvolutionFilter GraphicEQFilter GraphicEQFilterFactory)
  configure_file(${EAPO}/helpers/GainIterator.cpp
    ${EAPO_PORT}/helpers/GainIterator.cpp COPYONLY)
  configure_file(${EAPO}/helpers/GainIterator.h
    ${EAPO_PORT}/helpers/GainIterator.h COPYONLY)
  list(APPEND EAPO_SOURCES ${EAPO_PORT}/helpers/GainIterator.cpp)
endif()
foreach(component IN LISTS EAPO_FILTERS)
  foreach(ext cpp h)
    configure_file(${EAPO}/filters/${component}.${ext}
      ${EAPO_PORT}/filters/${component}.${ext} COPYONLY)
  endforeach()
  list(APPEND EAPO_SOURCES ${EAPO_PORT}/filters/${component}.cpp)
endforeach()
file(READ ${EAPO}/filters/BiQuad.h biquad)
string(REPLACE "#include <climits>" "#include <climits>\n#include <cfloat>" biquad "${biquad}")
string(REPLACE "__declspec(align(16))" "alignas(16)" biquad "${biquad}")
file(WRITE ${EAPO_PORT}/filters/BiQuad.h "${biquad}")

if(FFTW3F_FOUND)
  file(READ ${EAPO_PORT}/filters/ConvolutionFilter.cpp convolution)
  string(REPLACE "#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n" ""
         convolution "${convolution}")
  # FFTW planner calls occur only during off-thread graph construction. The
  # upstream threads helper is a separate optional library and unnecessary.
  string(REPLACE "fftwf_make_planner_thread_safe();" "" convolution "${convolution}")
  file(WRITE ${EAPO_PORT}/filters/ConvolutionFilter.cpp "${convolution}")

  file(READ ${EAPO_PORT}/filters/GraphicEQFilter.cpp graphic_eq)
  string(REPLACE "#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n" ""
         graphic_eq "${graphic_eq}")
  string(REPLACE "fftwf_make_planner_thread_safe();" "" graphic_eq "${graphic_eq}")
  string(REPLACE "delete buf;" "delete[] buf;" graphic_eq "${graphic_eq}")
  file(WRITE ${EAPO_PORT}/filters/GraphicEQFilter.cpp "${graphic_eq}")
  list(APPEND EAPO_SOURCES
       ${EAPO}/libHybridConv-0.1.1/libHybridConv_eapo.cpp)
endif()
