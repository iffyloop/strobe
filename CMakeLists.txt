cmake_minimum_required(VERSION 3.25)

if(WIN32)
	set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")
	set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows" ${CMAKE_PREFIX_PATH})
	set(X_VCPKG_APPLOCAL_DEPS_INSTALL TRUE CACHE BOOL "")
endif()

project(strobe VERSION 0.1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for LSPs")

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

find_package(OpenGL REQUIRED)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/glfw)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/nativefiledialog-extended)

set(
	PROJECT_HEADERS
	"src/app.h"
	"src/fly_camera.h"
	"src/gl_render_pass.h"
	"src/gl_shader.h"
	"src/gl_vertex_buffers.h"
	"src/io_utils.h"
	"src/sg_editor.h"
	"src/sg_compiler.h"
	"src/sg_node.h"
	"src/sg_plugins.h"
	"src/sg_renderer.h"
)
set(
	PROJECT_SOURCES
	"src/main.cpp"
	"src/app.cpp"
	"src/fly_camera.cpp"
	"src/gl_render_pass.cpp"
	"src/gl_shader.cpp"
	"src/gl_vertex_buffers.cpp"
	"src/io_utils.cpp"
	"src/sg_editor.cpp"
	"src/sg_compiler.cpp"
	"src/sg_node.cpp"
	"src/sg_plugins.cpp"
	"src/sg_renderer.cpp"
)

set(
  EXTERN_HEADERS
  extern/glad/include
  extern/glm
  extern/imgui
  extern/json/include
  extern/stb
  extern
)
file(
  GLOB EXTERN_SOURCES
  extern/glad/src/glad.c
  extern/imgui/imgui.cpp
  extern/imgui/imgui_demo.cpp
  extern/imgui/imgui_draw.cpp
  extern/imgui/imgui_tables.cpp
  extern/imgui/imgui_widgets.cpp
  extern/imgui/misc/cpp/imgui_stdlib.cpp
  extern/imgui/backends/imgui_impl_glfw.cpp
  extern/imgui/backends/imgui_impl_opengl3.cpp
  extern/implementation.c
)

file(GLOB PROJECT_CONFIGS CMakeLists.txt .gitattributes .gitignore .gitmodules)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES} ${PROJECT_HEADERS} ${EXTERN_SOURCES} ${PROJECT_CONFIGS})

# Hide console window on Windows while keeping console subsystem (allows main() function)
if(WIN32)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
	)
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE "src/pch.h")
set_source_files_properties(${EXTERN_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS true)
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS IMGUI_IMPL_OPENGL_LOADER_GLAD)
set_source_files_properties(${EXTERN_SOURCES} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
set_target_properties(
	${PROJECT_NAME} PROPERTIES
	CXX_STANDARD 23
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF
	COMPILE_WARNING_AS_ERROR ON
)
if(MSVC)
	# target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	src
	${EXTERN_HEADERS}
)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL glfw nfd)

if(APPLE)
	# Configure Info.plist
	configure_file(
		${CMAKE_CURRENT_SOURCE_DIR}/branding/Info.plist
		${CMAKE_CURRENT_BINARY_DIR}/Info.plist
		COPYONLY
	)

	# Configure the bundle fixup script
	configure_file(
		${CMAKE_CURRENT_SOURCE_DIR}/cmake/FixBundleMac.cmake.in
		${CMAKE_CURRENT_BINARY_DIR}/FixBundleMac.cmake
		@ONLY
	)

	# Create proper macOS bundle structure manually
	install(TARGETS ${PROJECT_NAME}
		RUNTIME DESTINATION ${PROJECT_NAME}.app/Contents/MacOS
	)

	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
		DESTINATION ${PROJECT_NAME}.app/Contents
	)

	install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/branding/icon.icns"
		DESTINATION ${PROJECT_NAME}.app/Contents/Resources
	)

	install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders
		DESTINATION ${PROJECT_NAME}.app/Contents/Resources
	)

	install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/branding/Assets.car"
		DESTINATION ${PROJECT_NAME}.app/Contents/Resources
	)

	# Set RPATH for bundle
	set_target_properties(${PROJECT_NAME} PROPERTIES
		INSTALL_RPATH "@executable_path/../Frameworks"
		BUILD_WITH_INSTALL_RPATH TRUE
	)

	# Use DragNDrop generator to create DMG from the .app bundle
	set(CPACK_GENERATOR "DragNDrop")
	set(CPACK_DMG_VOLUME_NAME "${PROJECT_NAME}")
	set(CPACK_DMG_FORMAT "UDBZ")

	# Run the bundle fixup script after installation
	install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixBundleMac.cmake")

	include(CPack)
elseif(WIN32)
	# Configure the bundle fixup script
	configure_file(
		${CMAKE_CURRENT_SOURCE_DIR}/cmake/FixBundleWindows.cmake.in
		${CMAKE_CURRENT_BINARY_DIR}/FixBundleWindows.cmake
		@ONLY
	)

	# Add Windows icon resource
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/branding/icon.ico")
		enable_language(RC)
		target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/branding/app.rc")
	endif()

	# Install executable
	install(TARGETS ${PROJECT_NAME}
		RUNTIME DESTINATION .
	)

	# Install shaders
	install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders
		DESTINATION .
	)

	# Use ZIP generator for Windows
	set(CPACK_GENERATOR "ZIP")
	set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-windows-x64")

	# Run the bundle fixup script after installation
	install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixBundleWindows.cmake")

	include(CPack)
endif()
