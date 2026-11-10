#pragma once

#include <cstdint>
#include <optional>

namespace Executor::control
{

struct KeyStroke
{
    uint8_t key = 0;        /* Mac virtual key code (MKV_*) */
    uint16_t modifiers = 0; /* Mac modifier flags, e.g. shiftKey */
};

/* Maps a printable ASCII character to a key stroke for the currently selected
 * KCHR layout, or nullopt if the character cannot be produced.
 *
 * Must be called on the emulator thread.  The reverse map is rebuilt whenever
 * the KCHR resource changes (--keyboards). */
std::optional<KeyStroke> lookupChar(char c);

}
