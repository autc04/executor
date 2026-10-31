#include "gtest/gtest.h"

#include "compat.h"

#ifdef EXECUTOR
#include <ToolboxEvent.h>
#include <OSEvent.h>
#include <ResourceMgr.h>
#include <WindowMgr.h>
#include <ControlMgr.h>
#include <SysErr.h>
#include <osevent/osevent.h>
#else
#include <Events.h>
#include <Resources.h>
#endif

TEST(Events, FlushThenGet)
{
    FlushEvents(everyEvent, 0);

    EventRecord e;
    bool gotEvent = GetOSEvent(everyEvent, &e);
    long ticks = TickCount();

    EXPECT_FALSE(gotEvent);
    EXPECT_EQ(nullEvent, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
}


TEST(Events, PostKey)
{
    FlushEvents(everyEvent, 0);

    PostEvent(keyDown, 0 | 'a');

    EventRecord e;
    bool gotEvent = GetOSEvent(everyEvent, &e);
    long ticks = TickCount();

    EXPECT_TRUE(gotEvent);
    EXPECT_EQ(keyDown, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
    EXPECT_EQ(0 | 'a', e.message);
    EXPECT_EQ(btnState, e.modifiers & 0xFF80);

    EXPECT_EQ(0, (ptr_from_longint<uint8_t*>(0x174))[0]);
    PostEvent(keyUp, 0 | 'a');


    gotEvent = GetOSEvent(everyEvent, &e);
    ticks = TickCount();

    EXPECT_FALSE(gotEvent);
    EXPECT_EQ(nullEvent, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
}



TEST(Events, PostKeyMod)
{
    FlushEvents(everyEvent, 0);

        // set command key in KeyMap
    (ptr_from_longint<uint8_t*>(0x174))[6] |= 0x80;

    PostEvent(keyDown, 0 | 'a');

        // clear command key in KeyMap
    (ptr_from_longint<uint8_t*>(0x174))[6] &= ~0x80;

    EventRecord e;
    bool gotEvent = GetOSEvent(everyEvent, &e);
    long ticks = TickCount();

    EXPECT_TRUE(gotEvent);
    EXPECT_EQ(keyDown, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
    EXPECT_EQ(0 | 'a', e.message);
    EXPECT_EQ(btnState | cmdKey, e.modifiers & 0xFF80);

    EXPECT_EQ(0, (ptr_from_longint<uint8_t*>(0x174))[0]);

    gotEvent = GetOSEvent(everyEvent, &e);
    ticks = TickCount();

    EXPECT_FALSE(gotEvent);
    EXPECT_EQ(nullEvent, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
}

TEST(Events, Button)
{
    EXPECT_FALSE(Button());
    auto saveButton = *ptr_from_longint<uint8_t*>(0x172);
    EXPECT_NE(0, saveButton);

    *ptr_from_longint<uint8_t*>(0x172) = 0;
    EXPECT_TRUE(Button());

    *ptr_from_longint<uint8_t*>(0x172) = saveButton;
    EXPECT_FALSE(Button());
}


TEST(Events, ButtonMod)
{
    FlushEvents(everyEvent, 0);

    EXPECT_FALSE(Button());
    auto saveButton = *ptr_from_longint<uint8_t*>(0x172);
    EXPECT_NE(0, saveButton);

    *ptr_from_longint<uint8_t*>(0x172) = 0;
    EXPECT_TRUE(Button());

    PostEvent(keyDown, 0 | 'a');

    EventRecord e;
    bool gotEvent = GetOSEvent(everyEvent, &e);
    long ticks = TickCount();

    EXPECT_TRUE(gotEvent);
    EXPECT_EQ(keyDown, e.what);
    EXPECT_LE(e.when, ticks);
    EXPECT_GT(e.when + 5, ticks);
    EXPECT_EQ(0 | 'a', e.message);
    EXPECT_EQ(0, e.modifiers & 0xFF80);

    *ptr_from_longint<uint8_t*>(0x172) = saveButton;
    EXPECT_FALSE(Button());
}


TEST(Events, PostNullChar)
{
    FlushEvents(everyEvent, 0);

    PostEvent(keyDown, 0);

    EventRecord e;
    bool gotEvent = GetOSEvent(everyEvent, &e);

    EXPECT_TRUE(gotEvent);
    EXPECT_EQ(keyDown, e.what);
    EXPECT_EQ(0, e.message);
}

namespace
{
struct TestWindow
{
    WindowPtr window;

    explicit TestWindow(const Rect &bounds)
        : window(NewWindow(nullptr, &bounds, (StringPtr)"", true, 0,
                           (WindowPtr)-1, false, 0))
    {
    }
    ~TestWindow()
    {
        if(window)
            DisposeWindow(window);
    }
};

Rect contentBounds()
{
    Rect r = {50, 50, 250, 450}; /* 400 x 200 content */
    return r;
}

/* A vertical scroll bar at the right edge of a document. */
Rect documentScrollBar()
{
    Rect r = {0, 384, 200, 400};
    return r;
}

/* The native test environment does not call sb_ctl_init(), so drawing a
 * scroll bar would use uninitialised arrow images.  Create it hidden and mark
 * it visible afterwards; the wheel handler does not draw. */
ControlHandle newScrollBar(WindowPtr w, const Rect &r)
{
    ControlHandle c = NewControl(w, &r, (StringPtr)"", false,
                                 0, 0, 100, scrollBarProc, 0);
    if(c)
        (*c)->contrlVis = 255;
    return c;
}
}

TEST(Events, WheelClicksDocumentScrollBar)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0xFF;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    Rect sb = documentScrollBar();
    ControlHandle bar = newScrollBar(w.window, sb);
    ASSERT_NE(nullptr, bar);

    /* The pointer is in the document body, far away from the bar. */
    ROMlib_handleWheelEvent(0, +1, 150, 150);

    EventRecord e;
    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseDown, e.what);
    /* Top arrow: local (392, 8), global (442, 58). */
    EXPECT_EQ(58, e.where.v);
    EXPECT_EQ(442, e.where.h);
    /* btnState means the button is up; mouseDown clears it. */
    EXPECT_EQ(0, e.modifiers & btnState);

    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseUp, e.what);
    EXPECT_NE(0, e.modifiers & btnState);

    /* Wheel down selects the bottom arrow: local (392, 192), global (442, 242). */
    ROMlib_handleWheelEvent(0, -1, 150, 150);

    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseDown, e.what);
    EXPECT_EQ(242, e.where.v);
    EXPECT_EQ(442, e.where.h);

    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseUp, e.what);

    FlushEvents(everyEvent, 0);
    LM(MBState) = savedButton;
}

TEST(Events, WheelPrefersBarUnderPointer)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0xFF;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    Rect right = documentScrollBar();
    Rect left = {0, 0, 200, 16};

    ASSERT_NE(nullptr, newScrollBar(w.window, right));
    ASSERT_NE(nullptr, newScrollBar(w.window, left));

    /* Pointer over the left bar; the right bar is merely "the next one". */
    ROMlib_handleWheelEvent(0, +1, 58, 150);

    EventRecord e;
    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseDown, e.what);
    EXPECT_EQ(58, e.where.v);
    EXPECT_EQ(58, e.where.h);

    FlushEvents(everyEvent, 0);
    LM(MBState) = savedButton;
}

TEST(Events, WheelClicksHorizontalScrollBar)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0xFF;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    Rect sb = {184, 0, 200, 400};
    ASSERT_NE(nullptr, newScrollBar(w.window, sb));

    /* Pointer is above the horizontal bar; the "nearest below" rule finds it. */
    ROMlib_handleWheelEvent(+1, 0, 150, 150);

    EventRecord e;
    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseDown, e.what);
    /* Right arrow: local (392, 192), global (442, 242). */
    EXPECT_EQ(242, e.where.v);
    EXPECT_EQ(442, e.where.h);

    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseUp, e.what);

    ROMlib_handleWheelEvent(-1, 0, 150, 150);

    ASSERT_TRUE(GetOSEvent(everyEvent, &e));
    EXPECT_EQ(mouseDown, e.what);
    /* Left arrow: local (8, 192), global (58, 242). */
    EXPECT_EQ(242, e.where.v);
    EXPECT_EQ(58, e.where.h);

    FlushEvents(everyEvent, 0);
    LM(MBState) = savedButton;
}

TEST(Events, WheelWithoutScrollBarDoesNothing)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0xFF;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    ROMlib_handleWheelEvent(0, +1, 150, 150);

    EventRecord e;
    EXPECT_FALSE(GetOSEvent(everyEvent, &e));

    LM(MBState) = savedButton;
}

TEST(Events, WheelIgnoredWhenButtonIsDown)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    Rect sb = documentScrollBar();
    ASSERT_NE(nullptr, newScrollBar(w.window, sb));

    ROMlib_handleWheelEvent(0, +1, 150, 150);

    EventRecord e;
    EXPECT_FALSE(GetOSEvent(everyEvent, &e));

    LM(MBState) = savedButton;
}

TEST(Events, WheelRequiresWindowManager)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    auto savedExist = LM(WWExist);
    LM(MBState) = 0xFF;
    LM(WWExist) = EXIST_NO;

    TestWindow w(contentBounds());
    ASSERT_NE(nullptr, w.window);

    Rect sb = documentScrollBar();
    ASSERT_NE(nullptr, newScrollBar(w.window, sb));

    ROMlib_handleWheelEvent(0, +1, 150, 150);

    EventRecord e;
    EXPECT_FALSE(GetOSEvent(everyEvent, &e));

    LM(WWExist) = savedExist;
    LM(MBState) = savedButton;
}

TEST(Events, WheelIgnoresCoveredScrollBar)
{
    FlushEvents(everyEvent, 0);

    auto savedButton = LM(MBState);
    LM(MBState) = 0xFF;

    TestWindow doc(contentBounds());
    ASSERT_NE(nullptr, doc.window);

    Rect sb = documentScrollBar();
    ASSERT_NE(nullptr, newScrollBar(doc.window, sb));

    /* A window in front covers the scroll bar, but not the pointer. */
    Rect cover = {50, 430, 250, 500};
    TestWindow front(cover);
    ASSERT_NE(nullptr, front.window);

    ROMlib_handleWheelEvent(0, +1, 150, 150);

    EventRecord e;
    EXPECT_FALSE(GetOSEvent(everyEvent, &e));

    FlushEvents(everyEvent, 0);
    LM(MBState) = savedButton;
}

TEST(Events, KeyTranslate)
{
    Handle h = GetResource('KCHR', 0);
    
    uint32_t state = 0;
    uint32_t result = 0;

    // translate the 'a' key
    result = KeyTranslate(*h, 0, PTR(state));
    EXPECT_EQ('a', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // result for keyup is the same
    result = KeyTranslate(*h, 0x80, PTR(state));
    EXPECT_EQ('a', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // translate the 'u' key
    result = KeyTranslate(*h, 0x20, PTR(state));
    EXPECT_EQ('u', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // result for keyup is the same
    result = KeyTranslate(*h, 0x80 | 0x20, PTR(state));
    EXPECT_EQ('u', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // add the shift key to get 'U'
    result = KeyTranslate(*h, shiftKey | 0x20, PTR(state));
    EXPECT_EQ('U', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // same for keyup
    result = KeyTranslate(*h, shiftKey | 0x80 | 0x20, PTR(state));
    EXPECT_EQ('U', result & 0x00FF00FF);
    EXPECT_EQ(0, state);

    // translate option-u; that's a dead key. No output, state changed.
    result = KeyTranslate(*h, optionKey | 0x20, PTR(state));
    EXPECT_EQ(0, result & 0x00FF00FF);
    EXPECT_NE(0, state);

    // next 'u' keypress gives umlaut, resets state
    result = KeyTranslate(*h, 0x20, PTR(state));
    EXPECT_EQ(0x9f, result & 0x00FF00FF);   // ü (umlaut u)
    EXPECT_EQ(0, state);

    // on keyup: no dead keys, we get just the dots
    result = KeyTranslate(*h, optionKey | 0x80 | 0x20, PTR(state));
    EXPECT_EQ(0xac, result & 0x00FF00FF);   // umlaut dots
    EXPECT_EQ(0, state);

    // start again with option-u
    result = KeyTranslate(*h, optionKey | 0x20, PTR(state));
    EXPECT_EQ(0, result & 0x00FF00FF);
    EXPECT_NE(0, state);

    // translating the corresponding keyup leaves the state unchanged
    auto unchangedState = state;
    result = KeyTranslate(*h, optionKey | 0x80 | 0x20, PTR(state));
    EXPECT_EQ(0xac, result & 0x00FF00FF);   // umlaut dots
    EXPECT_EQ(unchangedState, state);

    // ... so we can still get the umlaut
    result = KeyTranslate(*h, 0x20, PTR(state));
    EXPECT_EQ(0x9f, result & 0x00FF00FF);   // ü (umlaut u)
    EXPECT_EQ(0, state);

}
