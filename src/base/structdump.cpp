#include <base/structdump.h>
#include <base/logging.h>
#include <base/mactype.h>
#include <syn68k_public.h>

#include <ostream>
#include <vector>

namespace Executor
{
namespace structdump
{
namespace
{
std::vector<TypeDesc>& registry()
{
    static std::vector<TypeDesc> r;
    return r;
}

void ensureInitialized()
{
    static bool done = false;
    if(!done)
    {
        done = true;
        registerAllStructDumps();
    }
}
}

void registerType(const TypeDesc& type)
{
    registry().push_back(type);
}

void registerTypes(const TypeDesc* types, std::size_t count)
{
    for(std::size_t i = 0; i < count; i++)
        registry().push_back(types[i]);
}

const TypeDesc* findType(const std::string& name)
{
    ensureInitialized();
    for(const auto& type : registry())
        if(name == type.name)
            return &type;
    return nullptr;
}

bool dumpType(std::ostream& os, const std::string& name, uint32_t guestAddr, int count)
{
    const TypeDesc* type = findType(name);
    if(!type)
        return false;

    const uint8_t* host = (const uint8_t*)SYN68K_TO_US(guestAddr);

    for(int i = 0; i < count; i++)
    {
        os << type->name << " @ 0x" << std::hex << (guestAddr + (uint32_t)(i * type->size))
           << std::dec << ": ";
        if(!logging::validAddress(host + i * type->size))
            os << "? (invalid address)";
        else
            type->print(os, host + i * type->size);
        os << "\n";
    }
    return true;
}
}
}
