#pragma once

#include <string>

namespace Executor::mcp
{

std::string base64Encode(const std::string &bytes);
std::string base64Decode(const std::string &text);

}
