#if !defined(__rsys_dial_h__)
#define __rsys_dial_h__

#include <DialogMgr.h>
#include <base/traps.h>


namespace Executor
{
struct icon_item_template_t
{
    GUEST_STRUCT;
    GUEST<int16_t> count;
    GUEST<Handle> h;
    GUEST<Rect> r;
    GUEST<uint8_t> type;
    GUEST<uint8_t> len;
    GUEST<int16_t> res_id;
};

extern Boolean C_ROMlib_myfilt(DialogPtr dlg, EventRecord *evt,
                                      GUEST<INTEGER> *ith);
EXTERN_PASCAL_FUNCTION_PTR(ROMlib_myfilt);

static_assert(sizeof(icon_item_template_t) == 18);
}
#endif /* !defined (__rsys_dial_h__) */
