#pragma once

#include <rsys/filesystem.h>

namespace Executor
{

// Whether the last component of `p` can be used to address the directory entry
// it came from on this host.
//
// A directory enumeration hands back names; addressing one means handing the
// name back to the host's path parser. On Windows those are not the same thing:
// the parser remaps DOS device names ("aux", "nul", "com1", ... — with or
// without an extension), strips trailing dots and spaces, and cannot carry
// `<>:"|?*` or control characters. Such an entry exists on the host but no
// guest can reach it, so it is not listed. On other hosts every name a
// directory yields is addressable.
bool isAddressableFilename(const fs::path& p);

}
