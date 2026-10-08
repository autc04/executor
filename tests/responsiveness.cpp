#include "gtest/gtest.h"

#include <vdriver/responsiveness.h>

using namespace Executor;

namespace
{

/* The deterministic tests work on an explicit host timeline starting at 0.
 * 60 ticks == 1000 ms. */
class ResponsivenessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& r = Responsiveness::instance();
        r.setForceCaptured(false);
        r.setEnabled(true);
        r.setTimeoutTicks(60);
        r.setButtonDownAt(false, 0);
        r.resetAt(0);
    }
};

TEST_F(ResponsivenessTest, FreshInstanceIsNotCaptured)
{
    EXPECT_FALSE(Responsiveness::instance().isCaptured());
}

TEST_F(ResponsivenessTest, CaptureIsEnteredAfterTheTimeout)
{
    auto& r = Responsiveness::instance();

    EXPECT_FALSE(r.pollAt(999));
    EXPECT_TRUE(r.pollAt(1000));
    EXPECT_TRUE(r.isCaptured());
}

TEST_F(ResponsivenessTest, TimeoutConvertsTicksToMilliseconds)
{
    auto& r = Responsiveness::instance();
    r.setTimeoutTicks(6); /* 6 ticks == 100 ms */

    EXPECT_FALSE(r.pollAt(99));
    EXPECT_TRUE(r.pollAt(100));
}

TEST_F(ResponsivenessTest, HeldButtonBlocksEnteringCapture)
{
    auto& r = Responsiveness::instance();

    r.setButtonDownAt(true, 0);
    EXPECT_FALSE(r.pollAt(100000)); /* a tracking loop must not enter capture */

    r.setButtonDownAt(false, 100000);
    EXPECT_FALSE(r.pollAt(100000)); /* not yet: the clock starts at the release */
    EXPECT_TRUE(r.pollAt(101000));
}

/* The sequence from the design discussion: pump, mouse down, a ten second
 * tracking loop, mouse up, pump.  Capture must only begin if more than one
 * second separates the mouse-up from the pump. */
TEST_F(ResponsivenessTest, TrackingLoopTimeDoesNotCountTowardsTheTimeout)
{
    auto& r = Responsiveness::instance();

    r.noteResponseAt(0);              /* 1. pump events              */
    r.setButtonDownAt(true, 1);       /* 2. mouse down               */
    EXPECT_FALSE(r.pollAt(10001));    /* 3. ten second tracking loop */
    r.setButtonDownAt(false, 10001);  /* 4. mouse up                 */

    /* The ten second loop must not make the app look stuck: 998 ms after the
     * release, with still no pump, we are not captured. */
    EXPECT_FALSE(r.pollAt(10999));

    r.noteResponseAt(11000);          /* 5. pump events, 999 ms after 4. */
    EXPECT_FALSE(r.pollAt(11000));
}

TEST_F(ResponsivenessTest, TrackingLoopFollowedByARealGapEntersCapture)
{
    auto& r = Responsiveness::instance();

    r.noteResponseAt(0);
    r.setButtonDownAt(true, 1);
    EXPECT_FALSE(r.pollAt(10001));
    r.setButtonDownAt(false, 10001);

    EXPECT_FALSE(r.pollAt(10999)); /* 999 ms after release: not yet */
    EXPECT_TRUE(r.pollAt(11001));  /* 1000 ms after release: captured */

    r.noteResponseAt(11500);
    EXPECT_FALSE(r.pollAt(11500)); /* the pump leaves capture again */
}

TEST_F(ResponsivenessTest, HeldButtonDoesNotLeaveCapture)
{
    auto& r = Responsiveness::instance();

    EXPECT_TRUE(r.pollAt(1000)); /* captured */

    r.setButtonDownAt(true, 1100);
    EXPECT_TRUE(r.pollAt(1100));  /* a press must not leave capture */
    EXPECT_TRUE(r.pollAt(11000)); /* ...not even through a long drag */

    r.setButtonDownAt(false, 11100);
    EXPECT_TRUE(r.pollAt(11200)); /* a release must not leave it either */
    EXPECT_TRUE(r.isCaptured());
}

TEST_F(ResponsivenessTest, PumpingLeavesCaptureEvenWhileHeld)
{
    auto& r = Responsiveness::instance();

    EXPECT_TRUE(r.pollAt(1000));
    r.setButtonDownAt(true, 1100);

    r.noteResponseAt(1150);
    EXPECT_FALSE(r.pollAt(1150));
    EXPECT_FALSE(r.isCaptured());
}

TEST_F(ResponsivenessTest, DisabledNeverCaptures)
{
    auto& r = Responsiveness::instance();
    r.setEnabled(false);

    EXPECT_FALSE(r.pollAt(100000));
    EXPECT_FALSE(r.isCaptured());
}

TEST_F(ResponsivenessTest, NoteResponseRearms)
{
    auto& r = Responsiveness::instance();
    r.noteResponseAt(0);

    EXPECT_FALSE(r.pollAt(999));
    EXPECT_TRUE(r.pollAt(1000));
}

TEST_F(ResponsivenessTest, ResetClearsCaptureAndRestartsTheClock)
{
    auto& r = Responsiveness::instance();

    EXPECT_TRUE(r.pollAt(1000));
    r.resetAt(2000);

    EXPECT_FALSE(r.isCaptured());
    EXPECT_FALSE(r.pollAt(2999));
    EXPECT_TRUE(r.pollAt(3000));
}

TEST_F(ResponsivenessTest, ReleaseNeverExitsCaptureOnItsOwn)
{
    auto& r = Responsiveness::instance();

    EXPECT_TRUE(r.pollAt(1000));    /* captured */

    r.setButtonDownAt(true, 2000);  /* press   */
    r.setButtonDownAt(false, 3000); /* release */

    /* No pump happened, so capture must survive both the press and the release,
     * even long afterwards. */
    EXPECT_TRUE(r.pollAt(12000));
    EXPECT_TRUE(r.isCaptured());

    /* Only a pump clears it. */
    r.noteResponseAt(12000);
    EXPECT_FALSE(r.pollAt(12000));
}

TEST_F(ResponsivenessTest, EventWaitIsActiveForItsWholeDuration)
{
    auto& r = Responsiveness::instance();

    {
        Responsiveness::ActiveScope active; /* e.g. inside WaitNextEvent */
        /* However long the wait lasts, it is not a stuck application. */
        EXPECT_FALSE(r.pollAt(100000000));
        EXPECT_FALSE(r.pollAt(200000000));
    }

    /* Once the wait ends, the quiet clock starts from the exit. */
    unsigned long last = r.lastResponseMs();
    EXPECT_FALSE(r.pollAt(last + 999));
    EXPECT_TRUE(r.pollAt(last + 1000));
}

TEST_F(ResponsivenessTest, EnteringAnEventWaitClearsCapture)
{
    auto& r = Responsiveness::instance();

    EXPECT_TRUE(r.pollAt(1000)); /* captured */

    {
        Responsiveness::ActiveScope active;
        EXPECT_FALSE(r.pollAt(100000000)); /* waiting for events: not captured */
    }
}

TEST_F(ResponsivenessTest, ForceCapturedBypassesTimeoutButtonAndEnabled)
{
    auto& r = Responsiveness::instance();
    r.setForceCaptured(true);
    r.setEnabled(false);
    r.setButtonDownAt(true, 0);

    EXPECT_TRUE(r.poll());
    EXPECT_TRUE(r.isCaptured());
}

TEST_F(ResponsivenessTest, PollUsesTheStoredButtonState)
{
    auto& r = Responsiveness::instance();
    r.setTimeoutTicks(0); /* any elapsed time counts */
    r.resetAt(0);

    r.setButtonDownAt(true, 0);
    EXPECT_FALSE(r.poll()); /* fresh and held: must not enter */

    r.setButtonDownAt(false, 0);
    EXPECT_TRUE(r.poll()); /* released: enter */
}

}
