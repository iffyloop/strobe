#ifndef NDEBUG
#ifndef _WIN32
#define GLAD_WRAPPER_ENABLED
#define GLAD_WRAPPER_IMPLEMENTATION
#endif
#endif
#include <glad/glad.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
