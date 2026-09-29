/*
 * fanyi — stb_impl.cpp
 * stb 单头库的实现 TU（从 rapidocr.cpp 拆出，降低单 TU 编译内存峰值）。
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image_resize2.h"
