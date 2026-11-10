#include "png.h"

#include <png.h>

#include <csetjmp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace Executor::mcp
{

namespace
{

void writeData(png_structp png, png_bytep data, png_size_t length)
{
    auto *out = static_cast<std::string *>(png_get_io_ptr(png));
    out->append(reinterpret_cast<const char *>(data), length);
}

void flushData(png_structp)
{
}

}

std::string encodePngRGBA(int width, int height, const std::string &rgba)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                              nullptr, nullptr);
    if(!png)
        throw std::runtime_error("libpng: cannot create write struct");

    png_infop info = png_create_info_struct(png);
    if(!info)
    {
        png_destroy_write_struct(&png, nullptr);
        throw std::runtime_error("libpng: cannot create info struct");
    }

    std::string out;
    std::vector<png_bytep> rows((size_t)height);

    /* libpng reports errors with longjmp; do not let a C++ exception unwind
     * through its frames. */
    if(setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        throw std::runtime_error("libpng: failed to encode the screenshot");
    }

    png_set_write_fn(png, &out, writeData, flushData);

    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for(int y = 0; y < height; y++)
        rows[(size_t)y] =
            (png_bytep)(rgba.data() + (size_t)y * (size_t)width * 4);

    png_write_image(png, rows.data());
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);

    return out;
}

}
