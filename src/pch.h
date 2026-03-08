#pragma once

#include <glad/glad.h>

// GLAD must be included before GLFW
#include <GLFW/glfw3.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>

#include <imgui/backends/imgui_impl_glfw.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <imgui/imconfig.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/imstb_rectpack.h>
#include <imgui/imstb_textedit.h>
#include <imgui/imstb_truetype.h>
#include <imgui/misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NDEBUG
#define DEBUG
#endif

#ifdef DEBUG
#define assert_release(x) assert(x)
#else
#define assert_release(x) \
	do { \
		if (!(x)) { \
			std::cerr << "Assertion failed: " #x ", file " << __FILE__ << ", line " << __LINE__ \
				  << std::endl; \
			std::abort(); \
		} \
	} while (0)
#endif

#define UNUSED(x) ((void)(x));

#define DELETE_COPY(name) \
	name(name const&) = delete; \
	name& operator=(name const&) = delete;
#define DELETE_MOVE(name) name(name&&) = delete;
#define DELETE_COPY_MOVE(name) DELETE_COPY(name) DELETE_MOVE(name)
#define DEFAULT_MOVE(name) \
	name(name&&) = default; \
	name& operator=(name&&) = default;

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long u64;
typedef signed long long s64;
typedef float f32;
typedef double f64;
