#pragma once

#include <string>

namespace Executor::mcp
{

/* Encodes width*height bytes of RGBA (R, G, B, A order) as a PNG. */
std::string encodePngRGBA(int width, int height, const std::string &rgba);

}
