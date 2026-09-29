# Upstream remains pristine. Only selected DSP translation units and their
# relative headers are copied into the build directory, preserving notices.
set(EAPO_PORT ${CMAKE_CURRENT_BINARY_DIR}/eapo-port)
file(MAKE_DIRECTORY ${EAPO_PORT}/filters)
set(EAPO_FILTERS PreampFilter PreampFilterFactory BiQuad BiQuadFilter
    BiQuadFilterFactory IIRFilter IIRFilterFactory DelayFilter DelayFilterFactory)
set(EAPO_SOURCES)
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
