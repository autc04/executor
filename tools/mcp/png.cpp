#include "png.h"

#include <algorithm>
#include <cstdint>
#include <string>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

namespace Executor::mcp
{

namespace
{

uint32_t crc32(const std::string &data, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool initialized = false;

    if(!initialized)
    {
        for(uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for(int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        initialized = true;
    }

    crc = ~crc;

    for(uint8_t b : data)
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);

    return ~crc;
}

uint32_t adler32(const std::string &data)
{
    uint32_t a = 1, b = 0;

    for(uint8_t byte : data)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }

    return (b << 16) | a;
}

void appendBE32(std::string &out, uint32_t v)
{
    out += (char)((v >> 24) & 0xFF);
    out += (char)((v >> 16) & 0xFF);
    out += (char)((v >> 8) & 0xFF);
    out += (char)(v & 0xFF);
}

void appendChunk(std::string &png, const char *type, const std::string &data)
{
    appendBE32(png, (uint32_t)data.size());

    std::string typed(type, 4);
    typed += data;

    png += typed;
    appendBE32(png, crc32(typed));
}

/* A zlib stream for the PNG IDAT chunk.  Uses real compression when zlib is
 * available and stored (uncompressed) deflate blocks otherwise. */
std::string deflateZlib(const std::string &raw)
{
#ifdef HAVE_ZLIB
    uLongf bound = compressBound(raw.size());
    std::string out;
    out.resize(bound);

    if(compress2(reinterpret_cast<Bytef *>(out.data()), &bound,
                 reinterpret_cast<const Bytef *>(raw.data()), raw.size(), 6)
       == Z_OK)
    {
        out.resize(bound);
        return out;
    }
    /* fall through to the stored form */
#endif

    std::string z;
    z += (char)0x78; /* zlib header, no preset dictionary */
    z += (char)0x01;

    size_t offset = 0;
    while(offset < raw.size())
    {
        size_t n = std::min<size_t>(65535, raw.size() - offset);
        bool last = offset + n == raw.size();

        z += (char)(last ? 1 : 0);
        z += (char)(n & 0xFF);
        z += (char)((n >> 8) & 0xFF);
        z += (char)((~n) & 0xFF);
        z += (char)(((~n) >> 8) & 0xFF);
        z.append(raw, offset, n);
        offset += n;
    }

    appendBE32(z, adler32(raw));
    return z;
}

}

std::string encodePngRGBA(int width, int height, const std::string &rgba)
{
    std::string raw;
    raw.reserve((size_t)height * (1 + (size_t)width * 4));

    for(int y = 0; y < height; y++)
    {
        raw += '\0'; /* filter type 0 */
        raw.append(rgba, (size_t)y * width * 4, (size_t)width * 4);
    }

    std::string z = deflateZlib(raw);

    std::string ihdr;
    appendBE32(ihdr, (uint32_t)width);
    appendBE32(ihdr, (uint32_t)height);
    ihdr += (char)8; /* bit depth */
    ihdr += (char)6; /* colour type: RGBA */
    ihdr += (char)0; /* compression */
    ihdr += (char)0; /* filter */
    ihdr += (char)0; /* interlace */

    std::string png = "\x89PNG\r\n\x1a\n";
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", z);
    appendChunk(png, "IEND", std::string());

    return png;
}

}
