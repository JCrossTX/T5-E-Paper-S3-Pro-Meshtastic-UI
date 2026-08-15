#pragma once

/**
 * E-paper refresh policy, with no dependency on the panel driver.
 *
 * The view needs to say "the keyboard is open, keep updates in FAST" and
 * "pay off the ghosting debt now" without pulling in the panel driver's
 * headers. On a build with no e-paper the flags are set and read, and the
 * display path simply never consults them.
 */

#include <stdint.h>

// Refresh intent: the factory's three refresh modes. EINKDriver maps these
// onto the same epdiy waveforms the factory uses; a host build just records
// them.
enum class EinkRefresh : uint8_t {
    Fast,   // MODE_DU: cheapest, accumulates ghosting
    Normal, // MODE_GL16: keeps contrast up without a full flash
    Neat    // full clean + MODE_GC16: pays off the ghosting debt
};

class EINKDriverBase
{
  public:
    // Every flush drives one panel update; these choose its waveform.
    // Callers set intent, the driver's flush consumes it.
    static void setRefresh(EinkRefresh m) { refreshMode = m; }
    static EinkRefresh getRefresh(void) { return refreshMode; }

    // Forces FAST (MODE_DU) regardless of the refresh setting, for as long
    // as it is set. For text entry only: disable on keyboard close and
    // follow with requestNeatRefresh() to scrub the accumulated ghosting.
    static void enableUnlimitedFast(bool on) { unlimitedFast = on; }
    static bool unlimitedFastEnabled(void) { return unlimitedFast; }

    // The next flush runs the factory NEAT sequence: full clean, then GC16.
    static void requestNeatRefresh(void) { neatPending = true; }

    // The power-off splash, factory order: raw scrub, white flash-erase,
    // then the image as the final frame the panel holds while the device is
    // off. The view raises this before its final synchronous render; the
    // flush plays the sequence and makes it terminal. VCOM stays at the
    // calibrated value throughout.
    static void requestShutdownSplash(void) { shutdownSplash = true; }

    // Set while the splash is on glass, so nothing repaints over it.
    static void suspendRefresh(bool on) { refreshSuspended = on; }
    static bool refreshIsSuspended(void) { return refreshSuspended; }

    // Sleep hooks, called from the firmware's sleep path. The driver
    // installs the implementations in init(), which is what lets this
    // header name no panel-driver and no board type. Park: wait for any
    // waveform pass to finish, drop the panel rails, sleep the touch
    // controller.
    static void setSleepHooks(void (*park)(void), void (*resume)(void))
    {
        parkHook = park;
        resumeHook = resume;
    }
    static void parkForSleep(void)
    {
        if (parkHook)
            parkHook();
    }
    static void resumeFromSleep(void)
    {
        if (resumeHook)
            resumeHook();
    }

    // Set around each waveform pass so the sleep path can wait one out.
    static volatile bool refreshBusy;

    // The capacitive HOME key: raised from the GT911's home-button callback,
    // which the board adapter registers in initTouch(); the view consumes it
    // from its task_handler.
    static void notifyHomeButton(void) { homePending = true; }
    static bool takeHomeButton(void)
    {
        const bool was = homePending;
        homePending = false;
        return was;
    }

  protected:
    static bool homePending;
    static EinkRefresh refreshMode;
    static bool unlimitedFast;
    static bool neatPending;
    static bool refreshSuspended;
    static bool shutdownSplash;
    static void (*parkHook)(void);
    static void (*resumeHook)(void);
};

// Shared across every instantiation - there is exactly one panel. NORMAL is
// the factory's shipped default; the boot frames render before any user
// setting arrives, and only GL16 can produce the mid-grays the disabled
// chrome uses.
inline EinkRefresh EINKDriverBase::refreshMode = EinkRefresh::Normal;
inline bool EINKDriverBase::unlimitedFast = false;
inline bool EINKDriverBase::neatPending = false;
inline bool EINKDriverBase::homePending = false;
inline bool EINKDriverBase::refreshSuspended = false;
inline bool EINKDriverBase::shutdownSplash = false;
inline volatile bool EINKDriverBase::refreshBusy = false;
inline void (*EINKDriverBase::parkHook)(void) = nullptr;
inline void (*EINKDriverBase::resumeHook)(void) = nullptr;
