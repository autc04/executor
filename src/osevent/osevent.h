#if !defined(_RSYS_OSEVENT_H_)
#define _RSYS_OSEVENT_H_

#include <EventMgr.h>
#include <OSEvent.h>

namespace Executor
{
extern OSErr ROMlib_PPostEvent(INTEGER evcode, LONGINT evmsg,
                                  GUEST<EvQElPtr> *qelp, LONGINT when, Point where, INTEGER butmods);

extern Ptr ROMlib_kchr_ptr(void);

extern void invalidate_kchr_ptr(void);

enum
{
    NUMPAD_ENTER = 0x3
};

enum
{
    sizeof_KeyMap = 16
}; /* can't use sizeof(LM(KeyMap)) */

extern void ROMlib_eventinit();

extern void display_keyboard_choices(void);

extern bool ROMlib_set_keyboard(const char *keyboardname);
extern Boolean ROMlib_bewaremovement;
extern void ROMlib_showhidecursor(void);

extern uint16_t ROMlib_right_to_left_key_map(uint16_t what);

/* Translate a host mouse-wheel event into a simulated click on the scroll
 * bar that is responsible for the window under the pointer.  deltaX > 0
 * means wheel right, deltaY > 0 means wheel up; h/v are guest coordinates. */
extern void ROMlib_handleWheelEvent(int deltaX, int deltaY, int h, int v);

bool ROMlib_GetKey(uint8_t mkvkey);
void ROMlib_SetKey(uint8_t mkvkey, bool down);
short ROMlib_GetModifiers();

void ROMlib_SetAutokey(uint32_t message);

extern bool hle_get_event(EventRecord *evt, bool remflag);
extern void hle_init(void);
extern void hle_reinit(void);
extern void hle_reset(void);
}
#endif
