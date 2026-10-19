#pragma once

/* The only place that knows whether Tracy exists.  Call sites use Tracy's own
 * C++ macros; in a non-Tracy build the few that Executor uses expand to
 * nothing.  Do not include <tracy/...> anywhere else. */
#if defined(EXECUTOR_TRACY) && EXECUTOR_TRACY
#  include <tracy/Tracy.hpp>
#else
#  define ZoneScoped
#  define ZoneTransientN(varname, name, active)
#endif
