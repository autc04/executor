#include "base64.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Executor::mcp
{

namespace
{
const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64Encode(const std::string &bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    size_t i = 0;
    for(; i + 2 < bytes.size(); i += 3)
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8
                   | (uint8_t)bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }

    if(i + 1 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    }
    else if(i + 2 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }

    return out;
}

std::string base64Decode(const std::string &text)
{
    static int table[256];
    static bool initialized = false;

    if(!initialized)
    {
        std::fill(std::begin(table), std::end(table), -1);
        for(int i = 0; i < 64; i++)
            table[(uint8_t)kAlphabet[i]] = i;
        initialized = true;
    }

    std::string out;
    out.reserve(text.size() / 4 * 3);

    uint32_t acc = 0;
    int bits = 0;

    for(char c : text)
    {
        int v = table[(uint8_t)c];

        if(v < 0)
            continue; /* padding and whitespace */

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if(bits >= 8)
        {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }

    return out;
}

}
