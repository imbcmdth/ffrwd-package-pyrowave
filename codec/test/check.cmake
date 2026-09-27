# The package's test, what the manifest's `test` runs: builds the native test
# of the codec (see CMakeLists.txt beside this) and runs its `checks`. A CMake
# script, so the one command runs from cmd.exe and from a POSIX shell alike:
#
#   cmake -P codec/test/check.cmake
#
# Needs clang and clang++ on PATH (llvm-mingw on Windows), Ninja or make, a
# checkout of the PyroWave fork with wgpu-native in webgpu/external/ (or
# PYROWAVE_WEBGPU_NATIVE_DIR set), and a GPU. Environment:
#
#   PYROWAVE_DIR        the checkout; default ../pyrowave beside the package
#   PYROWAVE_TEST_BUILD the build directory; default ffrwd-pyrowave-codec-test
#                       under TEMP, TMPDIR or /tmp

cmake_minimum_required(VERSION 3.20)

get_filename_component(package "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if (DEFINED ENV{PYROWAVE_DIR})
	set(pyrowave "$ENV{PYROWAVE_DIR}")
else()
	get_filename_component(pyrowave "${package}/../pyrowave" ABSOLUTE)
endif()
if (NOT EXISTS "${pyrowave}/webgpu/CMakeLists.txt")
	message(FATAL_ERROR "no PyroWave checkout with webgpu/ at ${pyrowave}; set PYROWAVE_DIR")
endif()

if (DEFINED ENV{PYROWAVE_TEST_BUILD})
	set(build "$ENV{PYROWAVE_TEST_BUILD}")
else()
	foreach (var TEMP TMPDIR)
		if (DEFINED ENV{${var}})
			set(tmp "$ENV{${var}}")
			break()
		endif()
	endforeach()
	if (NOT tmp)
		set(tmp /tmp)
	endif()
	set(build "${tmp}/ffrwd-pyrowave-codec-test")
endif()
file(TO_CMAKE_PATH "${build}" build)

find_program(ninja ninja)
find_program(mingw_make mingw32-make)
if (ninja)
	set(generator Ninja)
elseif (CMAKE_HOST_WIN32 AND mingw_make)
	set(generator "MinGW Makefiles")
else()
	set(generator "Unix Makefiles")
endif()

set(extra)
if (DEFINED ENV{PYROWAVE_WEBGPU_NATIVE_DIR})
	list(APPEND extra "-DPYROWAVE_WEBGPU_NATIVE_DIR=$ENV{PYROWAVE_WEBGPU_NATIVE_DIR}")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -S "${package}/codec/test" -B "${build}" -G "${generator}"
	        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
	        "-DPYROWAVE_DIR=${pyrowave}" ${extra}
	RESULT_VARIABLE status)
if (NOT status EQUAL 0)
	message(FATAL_ERROR "configuring the native test in ${build} failed")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel RESULT_VARIABLE status)
if (NOT status EQUAL 0)
	message(FATAL_ERROR "building the native test in ${build} failed")
endif()

execute_process(COMMAND "${build}/pyrowave-codec-test" checks RESULT_VARIABLE status)
if (NOT status EQUAL 0)
	message(FATAL_ERROR "pyrowave-codec-test checks exited ${status}")
endif()
