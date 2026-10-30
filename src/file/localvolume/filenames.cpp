#include "filenames.h"

#include <cwctype>
#include <string>

using namespace Executor;

#ifdef _WIN32
namespace
{
// Win32 matches a device name against the part of the file name before the
// first dot or colon, after trimming trailing spaces. COM0/LPT0 are devices in
// Wine's parser while Microsoft's documentation names COM1-LPT9; either way
// they are not addressable, so both are treated as devices. Microsoft also
// treats the superscript digits as digits here, which Wine does not.
bool isReservedDeviceName(const std::wstring& name)
{
    std::wstring stem = name;

    auto stop = stem.find_first_of(L".:");
    if(stop != std::wstring::npos)
        stem.resize(stop);
    while(!stem.empty() && stem.back() == L' ')
        stem.pop_back();

    for(wchar_t& c : stem)
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));

    if(stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul"
       || stem == L"conin$" || stem == L"conout$")
        return true;

    if(stem.size() == 4
       && (stem.compare(0, 3, L"com") == 0 || stem.compare(0, 3, L"lpt") == 0))
    {
        const wchar_t digit = stem[3];
        return (digit >= L'0' && digit <= L'9')
            || digit == 0x00B9 || digit == 0x00B2 || digit == 0x00B3;
    }

    return false;
}

bool win32NameIsAddressable(const std::wstring& name)
{
    if(name.empty())
        return true;

    // Win32 drops trailing dots and spaces while resolving, so a name that ends
    // in either does not denote itself.
    if(name.back() == L'.' || name.back() == L' ')
        return false;

    for(wchar_t c : name)
        if(c < 0x20 || std::wcschr(L"<>:\"/\\|?*", c) != nullptr)
            return false;

    return !isReservedDeviceName(name);
}
}
#endif

bool Executor::isAddressableFilename(const fs::path& p)
{
#ifdef _WIN32
    return win32NameIsAddressable(p.filename().wstring());
#else
    (void)p;
    return true;
#endif
}
