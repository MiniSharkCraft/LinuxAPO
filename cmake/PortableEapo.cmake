# Upstream remains pristine. Only selected DSP translation units and their
# relative headers are copied into the build directory, preserving notices.
set(EAPO_PORT ${CMAKE_CURRENT_BINARY_DIR}/eapo-port)
file(MAKE_DIRECTORY ${EAPO_PORT}/filters ${EAPO_PORT}/helpers)
set(EAPO_SOURCES)
# FilterConfiguration consumes only immutable sizing values. Adapt generated
# copies to a narrow context instead of coupling them to SkyAPO's legacy global
# FilterEngine factory shim. Upstream source stays pristine; checked tokens make
# upstream drift fail at configure time rather than silently skip this fix.
foreach(configuration_file FilterConfiguration.h FilterConfiguration.cpp)
  set(configuration_path ${EAPO_PORT}/${configuration_file})
  file(READ "${EAPO}/${configuration_file}" configuration_content)
  if(configuration_file STREQUAL "FilterConfiguration.h")
    string(FIND "${configuration_content}"
      "FilterConfiguration(FilterEngine* engine" expected_signature)
  else()
    string(FIND "${configuration_content}" "#include \"FilterEngine.h\""
      expected_include)
  endif()
  if((configuration_file STREQUAL "FilterConfiguration.h" AND
      expected_signature EQUAL -1) OR
     (configuration_file STREQUAL "FilterConfiguration.cpp" AND
      expected_include EQUAL -1))
    message(FATAL_ERROR
      "Upstream FilterConfiguration changed; review its Linux context adaptation")
  endif()
  string(REPLACE "FilterEngine" "FilterConfigurationContext"
    configuration_content "${configuration_content}")
  if(configuration_file STREQUAL "FilterConfiguration.cpp")
    string(REPLACE "#include \"FilterConfigurationContext.h\""
      "#include <FilterConfigurationContext.h>" configuration_content
      "${configuration_content}")
    string(REPLACE "#include <FilterEngine.h>" "" configuration_content
      "${configuration_content}")
  endif()
  string(FIND "${configuration_content}" "FilterConfigurationContext"
    configuration_context_found)
  if(configuration_context_found EQUAL -1)
    message(FATAL_ERROR "Could not decouple upstream FilterConfiguration context")
  endif()
  if(EXISTS "${configuration_path}")
    file(READ "${configuration_path}" previous_configuration_content)
  else()
    set(previous_configuration_content "")
  endif()
  if(NOT "${previous_configuration_content}" STREQUAL
     "${configuration_content}")
    file(WRITE "${configuration_path}" "${configuration_content}")
  endif()
endforeach()
list(APPEND EAPO_SOURCES ${EAPO_PORT}/FilterConfiguration.cpp)
set(EAPO_FILTERS PreampFilter PreampFilterFactory BiQuad BiQuadFilter
    BiQuadFilterFactory IIRFilter IIRFilterFactory DelayFilter DelayFilterFactory
    ChannelFilter ChannelFilterFactory CopyFilter CopyFilterFactory
    LoudnessCorrectionFilter LoudnessCorrectionFilterFactory)
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
    set(upstream_filter ${EAPO}/filters/${component}.${ext})
    if(component MATCHES "^LoudnessCorrection")
      set(upstream_filter
          ${EAPO}/filters/loudnessCorrection/${component}.${ext})
    endif()
    configure_file(${upstream_filter}
      ${EAPO_PORT}/filters/${component}.${ext} COPYONLY)
  endforeach()
  list(APPEND EAPO_SOURCES ${EAPO_PORT}/filters/${component}.cpp)
endforeach()

# Keep the upstream implementation/parser, while replacing its COM endpoint
# reader and Win32 synchronization with the Linux provider and nonblocking
# event adapter. The checked-in upstream submodule remains pristine.
set(LOUDNESS_PORT ${EAPO_PORT}/filters)
configure_file(src/platform/linux/VolumeController.h
  ${LOUDNESS_PORT}/VolumeController.h COPYONLY)
configure_file(${EAPO}/filters/loudnessCorrection/ParameterArchive.h
  ${LOUDNESS_PORT}/ParameterArchive.h COPYONLY)
set(parameter_archive ${LOUDNESS_PORT}/ParameterArchive.h)
file(READ ${parameter_archive} content)
string(REPLACE "#include <vector>" "#include <vector>\n#include <cstring>"
  content "${content}")
string(REPLACE "StringHelper::toWString(input, CP_ACP)"
  "StringHelper::toWString(input, 65001)" content "${content}")
string(REPLACE "size_t noBytes = _serializedParamters.length() * sizeof(std::wstring);"
  "size_t noBytes = _serializedParamters.length() * sizeof(wchar_t);"
  content "${content}")
string(REPLACE "CopyMemory(&outputParameter[0], reinterpret_cast<char*>(&_serializedParamters[0]), noBytes);"
  "std::memcpy(outputParameter.data(), _serializedParamters.data(), noBytes);"
  content "${content}")
foreach(windows_only "CP_ACP" "CopyMemory" "sizeof(std::wstring)")
  string(FIND "${content}" "${windows_only}" remaining)
  if(NOT remaining EQUAL -1)
    message(FATAL_ERROR
      "Could not apply Linux ParameterArchive portability fix: ${windows_only}")
  endif()
endforeach()
file(WRITE ${parameter_archive} "${content}")
set(loudness_header ${LOUDNESS_PORT}/LoudnessCorrectionFilter.h)
file(READ ${loudness_header} content)
string(REPLACE "\r\n" "\n" content "${content}")
string(REPLACE "#include <filters/BiQuad.h>"
  "#include <filters/BiQuad.h>\n#include <EapoSyncCompat.h>\n#include <atomic>"
  content "${content}")
string(REPLACE "void* _parameterUpdateThreadHandle;"
  "void* _parameterUpdateThreadHandle = nullptr;" content "${content}")
string(REPLACE "void* _stopParameterUpdateThreadEvent;"
  "void* _stopParameterUpdateThreadEvent = nullptr;" content "${content}")
string(REPLACE "void* _parameterchangedEvent;"
  "void* _parameterchangedEvent = nullptr;" content "${content}")
string(REPLACE "double _attFactor;"
  "std::atomic<double> _attFactor{1.0};" content "${content}")
foreach(required "#include <EapoSyncCompat.h>"
                 "std::atomic<double> _attFactor{1.0};")
  string(FIND "${content}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Could not adapt upstream LoudnessCorrection header")
  endif()
endforeach()
file(WRITE ${loudness_header} "${content}")

set(loudness_source ${LOUDNESS_PORT}/LoudnessCorrectionFilter.cpp)
file(READ ${loudness_source} content)
string(REPLACE "\r\n" "\n" content "${content}")
function(skyapo_replace_required input needle replacement description)
  string(FIND "${${input}}" "${needle}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Could not apply upstream LoudnessCorrection patch: ${description}")
  endif()
  string(REPLACE "${needle}" "${replacement}" updated "${${input}}")
  set(${input} "${updated}" PARENT_SCOPE)
endfunction()
skyapo_replace_required(content "#include \"stdafx.h\""
  "#include \"stdafx.h\"\n#include <stdexcept>\n#include <limits>" "include standard headers")
string(FIND "${content}" "gain = 0;" zero_gain_offset)
if(zero_gain_offset EQUAL -1)
  message(FATAL_ERROR "Could not initialize zero-difference LoudnessCorrection preAmp")
endif()
math(EXPR after_zero_gain "${zero_gain_offset} + 9")
string(SUBSTRING "${content}" 0 ${zero_gain_offset} before_zero_gain)
string(SUBSTRING "${content}" ${after_zero_gain} -1 after_zero_gain)
set(content "${before_zero_gain}gain = 0;\n\t\tpreAmp = 0;${after_zero_gain}")
skyapo_replace_required(content "_stopParameterUpdateThreadEvent = CreateEvent(NULL, true, false, NULL);\n\t_parameterchangedEvent = CreateEvent(NULL, true, false, NULL);\n\t_parameterUpdateThreadHandle = CreateThread(NULL, 0, &parameterUpdateThread, this, 0, NULL);"
  "_stopParameterUpdateThreadEvent = CreateEvent(NULL, true, false, NULL);\n\tif (!_stopParameterUpdateThreadEvent)\n\t\tthrow std::runtime_error(\"cannot create loudness stop event\");\n\t_parameterchangedEvent = CreateEvent(NULL, true, false, NULL);\n\tif (!_parameterchangedEvent)\n\t{\n\t\tCloseHandle(_stopParameterUpdateThreadEvent);\n\t\t_stopParameterUpdateThreadEvent = nullptr;\n\t\tthrow std::runtime_error(\"cannot create loudness update event\");\n\t}\n\t_parameterUpdateThreadHandle = CreateThread(NULL, 0, &parameterUpdateThread, this, 0, NULL);\n\tif (!_parameterUpdateThreadHandle)\n\t{\n\t\tCloseHandle(_parameterchangedEvent);\n\t\tCloseHandle(_stopParameterUpdateThreadEvent);\n\t\t_parameterchangedEvent = nullptr;\n\t\t_stopParameterUpdateThreadEvent = nullptr;\n\t\tthrow std::runtime_error(\"cannot create loudness update thread\");\n\t}"
  "${content}" "initialize worker handles")
skyapo_replace_required(content "_tempResult *= _attFactor;"
  "_tempResult *= _attFactor.load(std::memory_order_relaxed);" "atomic attenuation")
skyapo_replace_required(content
  "float volOld(lCorrection->_parameters.referenceLevel);"
  "float volOld(std::numeric_limits<float>::quiet_NaN());"
  "apply first available endpoint volume")
skyapo_replace_required(content "TryEnterCriticalSection(&_parameterUpdateSection);"
  "EnterCriticalSection(&_parameterUpdateSection);" "serialize coefficient writer")
skyapo_replace_required(content [=[	if (WaitForSingleObject(_parameterchangedEvent, 0) == WAIT_OBJECT_0)
	{
		for (unsigned i = 0; i < _channelCount; i++)
		{
			_lowShelfBiquads[i].setCoefficients(_aLS, _a0LS);
			_highShelfBiquads[i].setCoefficients(_aHS, _a0HS);
		}
		_neutral = upDateNeutral();
		ResetEvent(_parameterchangedEvent);
	}]=] [=[	if (WaitForSingleObject(_parameterchangedEvent, 0) == WAIT_OBJECT_0 &&
		TryEnterCriticalSection(&_parameterUpdateSection))
	{
		for (unsigned i = 0; i < _channelCount; i++)
		{
			_lowShelfBiquads[i].setCoefficients(_aLS, _a0LS);
			_highShelfBiquads[i].setCoefficients(_aHS, _a0HS);
		}
		_neutral = upDateNeutral();
		ResetEvent(_parameterchangedEvent);
		LeaveCriticalSection(&_parameterUpdateSection);
	}]=] "nonblocking coefficient handoff")
file(WRITE ${loudness_source} "${content}")

set(loudness_factory ${LOUDNESS_PORT}/LoudnessCorrectionFilterFactory.cpp)
file(READ ${loudness_factory} content)
skyapo_replace_required(content
  "TraceF(L\"Adding loudness correction filter\");"
  "LogHelper::log(__FILE__, __LINE__, this, true, L\"Adding loudness correction filter\");"
  "Linux variadic logging compatibility")
file(WRITE ${loudness_factory} "${content}")

file(READ ${EAPO}/filters/BiQuad.h biquad)
string(REPLACE "#include <climits>" "#include <climits>\n#include <cfloat>" biquad "${biquad}")
string(REPLACE "#define IS_DENORMAL(d) (abs(d) < DBL_MIN)"
       "#define IS_DENORMAL(d) (std::abs(d) < DBL_MIN)" biquad "${biquad}")
string(REPLACE "__declspec(align(16))" "alignas(16)" biquad "${biquad}")
string(FIND "${biquad}"
  "#define IS_DENORMAL(d) (std::abs(d) < DBL_MIN)" biquad_denormal_patch)
if(biquad_denormal_patch EQUAL -1)
  message(FATAL_ERROR "Could not apply the Linux BiQuad denormal patch")
endif()
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
