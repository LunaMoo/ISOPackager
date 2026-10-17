cmake_minimum_required(VERSION 3.20)

project(ISOPackager VERSION 0.1.0 DESCRIPTION "Compress, extract, repack and benchmark disc images" LANGUAGES C CXX)

# ISOPackager links mCSO (maxcso), CHDm (CHDmodern) and iPBP statically, from the submodules in external/.
# Everything is built from source, so the same tree builds on Windows, Linux, macOS, FreeBSD,
# Android, iOS and UWP without system packages.

if(ANDROID OR IOS OR CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|visionOS")
	set(ISOPK_MOBILE ON)
else()
	set(ISOPK_MOBILE OFF)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	set(ISOPK_UWP ON)
else()
	set(ISOPK_UWP OFF)
endif()

if(ISOPK_MOBILE OR ISOPK_UWP)
	set(ISOPK_CLI_DEFAULT OFF)
else()
	set(ISOPK_CLI_DEFAULT ON)
endif()
if(WIN32 AND NOT ISOPK_UWP)
	set(ISOPK_BACKEND_DEFAULT win32)
else()
	set(ISOPK_BACKEND_DEFAULT sdl2)
endif()

option(ISOPK_BUILD_GUI "Build the ImGui application" ON)
option(ISOPK_BUILD_CLI "Build the isopackager-cli command line tool" ${ISOPK_CLI_DEFAULT})
option(ISOPK_BUILD_TESTS "Build the round-trip self-test" ${ISOPK_CLI_DEFAULT})
option(ISOPK_WITH_CSO "Include CSO/ZSO/DAX support (mCSO)" ON)
option(ISOPK_CSO_PORTABLE "Build mCSO on uvlite (src/compat) instead of libuv, without 7-zip's file I/O. Needed for UWP; works anywhere" ${ISOPK_UWP})
set(ISOPK_GUI_BACKEND ${ISOPK_BACKEND_DEFAULT} CACHE STRING "GUI platform layer: win32 (Win32 + Direct3D 11) or sdl2 (SDL2 + SDL_Renderer)")
set_property(CACHE ISOPK_GUI_BACKEND PROPERTY STRINGS win32 sdl2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One C runtime for everything: the static CRT (/MT), like mCSO.dll, so the exe needs no
# redistributable. The CMP0091 default makes the subprojects that ask for an older CMake honor it.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(ISOPK_UWP)
	# UWP apps must use the DLL CRT.
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(ISOPK_UWP)
	# Store apps compile with /sdl (deprecations become errors) by default; the bundled C libraries use
	# plain fopen & co.
	add_compile_options(/sdl-)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
	# Ports (X11 for the GUI) install to /usr/local.
	include_directories(SYSTEM /usr/local/include)
	link_directories(/usr/local/lib)
endif()

set(EXT ${CMAKE_CURRENT_SOURCE_DIR}/external)
foreach(sub maxcso CHDmodern iPBP imgui)
	if(NOT EXISTS ${EXT}/${sub}/.git AND NOT EXISTS ${EXT}/${sub}/README.md)
		message(FATAL_ERROR "external/${sub} is empty: run  git submodule update --init --recursive")
	endif()
endforeach()

# ---------------------------------------------------------------------------
# mCSO (maxcso). Must come first: CHDmodern reuses its zlib.
# ---------------------------------------------------------------------------
if(ISOPK_WITH_CSO)
	set(MCSO_SHARED OFF CACHE BOOL "" FORCE)
	set(MCSO_BUILD_CLI OFF CACHE BOOL "" FORCE)
	set(MCSO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	add_subdirectory(${EXT}/maxcso ${CMAKE_BINARY_DIR}/ext/maxcso EXCLUDE_FROM_ALL)
	set(ISOPK_ZLIB mcso_zlib)
	if(ISOPK_CSO_PORTABLE)
		# libuv uses Win32 desktop APIs that UWP doesn't have, but maxcso only needs 16 of its
		# functions (loop, thread pool, mutexes, file I/O): uvlite provides exactly those.
		add_library(isopk_uvlite STATIC src/compat/uvlite/uvlite.cpp)
		target_include_directories(isopk_uvlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/compat/uvlite)
		target_link_libraries(isopk_uvlite PUBLIC Threads::Threads)
		foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
			get_target_property(libs mcso_core ${prop})
			if(libs)
				list(TRANSFORM libs REPLACE "^uv_a$" isopk_uvlite)
				set_target_properties(mcso_core PROPERTIES ${prop} "${libs}")
			endif()
		endforeach()
		# 7-zip's file streams aren't used by its deflate encoder (the only part maxcso needs).
		get_target_property(srcs mcso_7zip SOURCES)
		list(FILTER srcs EXCLUDE REGEX "(FileIO|FileStreams)\\.cpp$")
		set_target_properties(mcso_7zip PROPERTIES SOURCES "${srcs}")
		if(ISOPK_UWP)
			target_compile_options(mcso_7zip PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/compat/uwp_7zip_shim.h")
		endif()
	endif()
	if(ISOPK_UWP)
		# maxcso asks for the static CRT in its own folder; UWP only allows the DLL one.
		foreach(t uv_a mcso_zlib mcso_lz4 mcso_libdeflate mcso_zopfli mcso_7zip mcso_core mcso)
			if(TARGET ${t})
				set_target_properties(${t} PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
			endif()
		endforeach()
	endif()
else()
	# Without mCSO, build zlib from CHDmodern's copy for ourselves (and CHDmodern).
	set(Z ${EXT}/CHDmodern/third_party/zlib)
	add_library(isopk_zlib STATIC ${Z}/adler32.c ${Z}/crc32.c ${Z}/deflate.c ${Z}/inffast.c
		${Z}/inflate.c ${Z}/inftrees.c ${Z}/trees.c ${Z}/zutil.c)
	target_include_directories(isopk_zlib PUBLIC ${Z})
	target_compile_definitions(isopk_zlib PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:HAVE_UNISTD_H>)
	set(ISOPK_ZLIB isopk_zlib)
endif()

# ---------------------------------------------------------------------------
# CHDm (CHDmodern)
#
# Linking all three libraries into one program brings two copies of some third-party code:
#  * zlib: maxcso bundles 1.2.11, CHDmodern 1.3.2. Same API, so CHDmodern uses maxcso's.
#  * LZMA SDK match finder (LzFind.c): maxcso's 7-zip deflate encoder bundles the 2017 version,
#    CHDmodern's LZMA codec the 2024 one. They export the same function names (MatchFinder_*)
#    with different struct layouts, so the linker would silently mix them and corrupt memory.
#    CHDmodern's copy is built here with those symbols renamed (lzma_rename.h).
# ---------------------------------------------------------------------------
set(LZ ${EXT}/CHDmodern/third_party/lzma/C)
add_library(isopk_chdm_lzma STATIC ${LZ}/CpuArch.c ${LZ}/LzFind.c ${LZ}/LzFindOpt.c ${LZ}/LzmaDec.c ${LZ}/LzmaEnc.c)
target_include_directories(isopk_chdm_lzma PUBLIC ${EXT}/CHDmodern/third_party)
target_compile_definitions(isopk_chdm_lzma PUBLIC Z7_ST)
if(MSVC)
	target_compile_options(isopk_chdm_lzma PRIVATE /w "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/lzma_rename.h")
else()
	target_compile_options(isopk_chdm_lzma PRIVATE -w -include "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lzma_rename.h")
endif()

set(CHDMODERN_SHARED OFF CACHE BOOL "" FORCE)
set(CHDMODERN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CHDMODERN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHDMODERN_INSTALL OFF CACHE BOOL "" FORCE)
set(CHDMODERN_ZLIB_TARGET ${ISOPK_ZLIB} CACHE STRING "" FORCE)
set(CHDMODERN_LZMA_TARGET isopk_chdm_lzma CACHE STRING "" FORCE)
add_subdirectory(${EXT}/CHDmodern ${CMAKE_BINARY_DIR}/ext/CHDmodern EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# iPBP
# ---------------------------------------------------------------------------
set(IPBP_SHARED OFF CACHE BOOL "" FORCE)
set(IPBP_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(IPBP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/iPBP ${CMAKE_BINARY_DIR}/ext/iPBP EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# isopk_core: everything except the user interface
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(isopk_core STATIC
	src/core/formats.cpp
	src/core/fileio.cpp
	src/core/image.cpp
	src/core/engine.cpp
	src/core/queue.cpp
	src/core/bench.cpp
	src/core/scan.cpp
	src/core/settings.cpp
	src/core/sysinfo.cpp
	src/core/util.cpp)
target_include_directories(isopk_core PUBLIC src)
target_compile_definitions(isopk_core PUBLIC
	ISOPK_VERSION="${PROJECT_VERSION}"
	$<$<BOOL:${ISOPK_WITH_CSO}>:ISOPK_WITH_CSO=1>
	$<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE>)
target_link_libraries(isopk_core PUBLIC chdmodern ipbp ${ISOPK_ZLIB} Threads::Threads)
if(ISOPK_WITH_CSO)
	target_link_libraries(isopk_core PUBLIC mcso mcso_lz4)
endif()
if(MSVC)
	target_compile_options(isopk_core PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus)
	target_compile_definitions(isopk_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(isopk_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(ANDROID)
	target_link_libraries(isopk_core PUBLIC log)
endif()

# ---------------------------------------------------------------------------
# Command line tool
# ---------------------------------------------------------------------------
if(ISOPK_BUILD_CLI)
	add_executable(isopackager_cli src/cli/main.cpp)
	target_link_libraries(isopackager_cli PRIVATE isopk_core)
	set_target_properties(isopackager_cli PROPERTIES OUTPUT_NAME isopackager-cli)
	if(MSVC)
		target_compile_options(isopackager_cli PRIVATE /utf-8)
		# Windows 10 1903+ gives the process a UTF-8 code page, so argv and fopen() take UTF-8.
		target_sources(isopackager_cli PRIVATE src/platform/windows/utf8.manifest)
	endif()
	install(TARGETS isopackager_cli RUNTIME DESTINATION bin)
endif()

if(ISOPK_BUILD_TESTS)
	enable_testing()
	add_executable(isopk_selftest tests/selftest.cpp)
	target_link_libraries(isopk_selftest PRIVATE isopk_core)
	if(MSVC)
		target_sources(isopk_selftest PRIVATE src/platform/windows/utf8.manifest)
	endif()
	add_test(NAME selftest COMMAND isopk_selftest ${CMAKE_CURRENT_BINARY_DIR}/selftest_tmp)
endif()

# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
if(ISOPK_BUILD_GUI)
	set(IMGUI ${EXT}/imgui)
	add_library(isopk_imgui STATIC
		${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp)
	target_include_directories(isopk_imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
	target_compile_definitions(isopk_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

	set(GUI_SOURCES
		src/gui/app.cpp
		src/gui/app_panels.cpp
		src/gui/file_browser.cpp
		src/gui/widgets.cpp)

	if(ISOPK_GUI_BACKEND STREQUAL "win32")
		target_sources(isopk_imgui PRIVATE ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
		add_executable(isopackager WIN32 ${GUI_SOURCES} src/gui/main_win32.cpp src/platform/windows/isopackager.rc
			src/platform/windows/utf8.manifest)
		set_target_properties(isopackager PROPERTIES OUTPUT_NAME ISOPackager)
		target_link_libraries(isopackager PRIVATE d3d11 dxgi d3dcompiler dwmapi ole32 shell32 uuid comdlg32)
	else()
		set(SDL_TEST OFF CACHE BOOL "" FORCE)
		set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
		set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
		set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
		if(ANDROID)
			# SDLActivity loads libSDL2.so and then libmain.so (our app).
			set(SDL_SHARED ON CACHE BOOL "" FORCE)
			set(SDL_STATIC OFF CACHE BOOL "" FORCE)
			set(ISOPK_SDL_TARGET SDL2::SDL2)
		else()
			set(SDL_SHARED OFF CACHE BOOL "" FORCE)
			set(SDL_STATIC ON CACHE BOOL "" FORCE)
			set(ISOPK_SDL_TARGET SDL2::SDL2-static)
		endif()
		if(ISOPK_UWP)
			# SDL's WinRT sources are C++/CX (/ZW), which can't be C++20.
			set(CMAKE_CXX_STANDARD 17)
		endif()
		add_subdirectory(${EXT}/SDL ${CMAKE_BINARY_DIR}/ext/SDL EXCLUDE_FROM_ALL)
		set(CMAKE_CXX_STANDARD 20)
		target_sources(isopk_imgui PRIVATE ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
		target_link_libraries(isopk_imgui PUBLIC ${ISOPK_SDL_TARGET})
		if(ANDROID)
			add_library(isopackager SHARED ${GUI_SOURCES} src/gui/main_sdl.cpp)
			set_target_properties(isopackager PROPERTIES OUTPUT_NAME main)
		elseif(APPLE)
			add_executable(isopackager MACOSX_BUNDLE ${GUI_SOURCES} src/gui/main_sdl.cpp)
			set_target_properties(isopackager PROPERTIES
				OUTPUT_NAME ISOPackager
				MACOSX_BUNDLE_GUI_IDENTIFIER io.github.lunamoo.isopackager
				MACOSX_BUNDLE_BUNDLE_NAME ISOPackager
				MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
				MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
				MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/apple/Info.plist.in
				XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER io.github.lunamoo.isopackager)
			target_link_libraries(isopackager PRIVATE SDL2::SDL2main)
		elseif(ISOPK_UWP)
			# SDL's WinRT entry point is C++/CX: compile it into the app with /ZW.
			# Only that file: C++/CX can't be C++20, which CHDmodern needs everywhere else.
			add_library(isopk_winrt_main OBJECT ${EXT}/SDL/src/main/winrt/SDL_winrt_main_NonXAML.cpp)
			set_target_properties(isopk_winrt_main PROPERTIES CXX_STANDARD 17)
			target_compile_options(isopk_winrt_main PRIVATE /ZW /EHsc)
			target_include_directories(isopk_winrt_main PRIVATE ${EXT}/SDL/include)
			add_executable(isopackager WIN32 ${GUI_SOURCES} src/gui/main_sdl.cpp $<TARGET_OBJECTS:isopk_winrt_main>)
			set_target_properties(isopackager PROPERTIES OUTPUT_NAME ISOPackager)
		else()
			add_executable(isopackager WIN32 ${GUI_SOURCES} src/gui/main_sdl.cpp)
			if(NOT WIN32)
				set_target_properties(isopackager PROPERTIES OUTPUT_NAME isopackager)
			endif()
			target_link_libraries(isopackager PRIVATE SDL2::SDL2main)
		endif()
		target_compile_definitions(isopackager PRIVATE ISOPK_SDL=1)
	endif()
	target_link_libraries(isopackager PRIVATE isopk_core isopk_imgui)
	if(MSVC)
		target_compile_options(isopackager PRIVATE /utf-8)
	endif()
	if(NOT ANDROID)
		install(TARGETS isopackager RUNTIME DESTINATION bin BUNDLE DESTINATION .)
	endif()
endif()
