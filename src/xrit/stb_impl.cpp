// Single translation unit that instantiates the vendored stb_image decoder.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
