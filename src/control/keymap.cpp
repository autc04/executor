#include <control/keymap.h>

#include <EventMgr.h>
#include <ToolboxEvent.h>

#include <osevent/osevent.h>

#include <map>

using namespace Executor;
using namespace Executor::control;

namespace
{

std::map<char, KeyStroke> buildMap(Ptr kchr)
{
    std::map<char, KeyStroke> map;

    if(!kchr)
        return map;

    /* The US layouts only need these; dead keys and layouts that need other
     * combinations are best-effort (see the design document). */
    static const uint16_t modifierSets[] = {
        0,
        shiftKey,
        optionKey,
        optionKey | shiftKey,
    };

    for(uint16_t mods : modifierSets)
        for(int key = 0; key < 128; key++)
        {
            GUEST<uint32_t> state = 0;
            uint32_t translated = C_KeyTranslate(kchr, mods | key, &state);

            /* The low byte is the character; a second character (a dead-key
             * completion) is in the next byte. */
            for(int bits : { 0, 16 })
            {
                unsigned char ch = (translated >> bits) & 0xFF;

                if(ch >= 0x20 && ch < 0x7F
                   && map.find(static_cast<char>(ch)) == map.end())
                    map[static_cast<char>(ch)] = KeyStroke{ (uint8_t)key, mods };
            }
        }

    return map;
}

}

std::optional<KeyStroke> Executor::control::lookupChar(char c)
{
    static Ptr cachedKchr = nullptr;
    static std::map<char, KeyStroke> map;

    Ptr kchr = ROMlib_kchr_ptr();
    if(kchr != cachedKchr)
    {
        cachedKchr = kchr;
        map = buildMap(kchr);
    }

    auto it = map.find(c);
    if(it == map.end())
        return std::nullopt;
    return it->second;
}
