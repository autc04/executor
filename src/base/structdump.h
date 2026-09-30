#pragma once
#include <cstddef>
#include <iosfwd>
#include <string>

namespace Executor
{
namespace structdump
{

// Metadata for one dumpable type, registered by the generated structdump code.
struct TypeDesc
{
    const char* name;
    std::size_t size;
    void (*print)(std::ostream& os, const void* p);
};

void registerType(const TypeDesc& type);
void registerTypes(const TypeDesc* types, std::size_t count);

// Populates the registry from all generated modules.  Idempotent; called
// lazily by findType()/dumpType().
void registerAllStructDumps();

// Look a type up by name; nullptr if unknown.  Names are the generated type
// names (e.g. "HFileParam", "ParamBlockRec").
const TypeDesc* findType(const std::string& name);

// Print `count` consecutive values of the named type starting at the guest
// address `guestAddr`.  Returns false if the type is unknown.
bool dumpType(std::ostream& os, const std::string& name, uint32_t guestAddr, int count = 1);

}
}
