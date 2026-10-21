#pragma once

/* The only place that knows whether Tracy exists.  Call sites use Tracy's own
 * C++ API; in a non-Tracy build the few names Executor uses are stubbed out.
 * Do not include <tracy/...> anywhere else. */
#if defined(EXECUTOR_TRACY) && EXECUTOR_TRACY
#  include <tracy/Tracy.hpp>
#else
#  define ZoneScoped
#  define ZoneScopedN(name)
#  define ZoneTransientN(varname, name, active)

namespace tracy
{
inline void SetThreadName(const char*) {}
}
#endif
