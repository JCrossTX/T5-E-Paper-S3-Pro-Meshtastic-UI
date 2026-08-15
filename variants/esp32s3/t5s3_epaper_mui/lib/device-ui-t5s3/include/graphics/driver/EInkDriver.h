#pragma once

/**
 * EINKDriver - device-ui DisplayDriver on epdiy, the factory display stack
 * for the LilyGO T5 E-Paper S3 Pro (ED047TC1, epd_board_v7).
 *
 * PIPELINE (factory structure, verbatim): LVGL renders the full 540x960
 * portrait screen in L8 (RENDER_MODE_FULL). The flush converts L8 to the
 * 4bpp grayscale buffer the factory uses - 16 real grays, no dithering -
 * then epd_draw_rotated_image() places it (rotation is epdiy's own, set
 * once via EPD_ROT_INVERTED_PORTRAIT), and ONE update drives the panel with
 * the mode the refresh setting selects:
 *
 *     Fast    MODE_DU     factory UI_REFRESH_MODE_FAST
 *     Normal  MODE_GL16   factory UI_REFRESH_MODE_NORMAL
 *     Neat    tracked all-white + GL16, then draw + MODE_GC16
 *
 * Rails run a short keep-alive window (kRailKeepAliveMs) instead of the
 * factory's per-update cycle, saving the ~100-150 ms PWRGOOD handshake
 * between consecutive interactions - function-identical output; sleep and
 * shutdown always force them down. There is no rate gate, no ghost-debt
 * counter and no change tracking here: factory has none, and epdiy's
 * internal difference buffer already confines waveforms to changed rows.
 */

#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverConfig.h"
#include "graphics/driver/EInkRefreshPolicy.h"
#include "util/ILog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "epd_highlevel.h"
#include "epdiy.h"

// Arduino core 3.x HAL: the NG bus handle for the bus Wire.begin() created.
extern "C" void *i2cBusHandle(uint8_t i2c_num);

#ifndef T5_PORTRAIT_W
#define T5_PORTRAIT_W 540
#endif
#ifndef T5_PORTRAIT_H
#define T5_PORTRAIT_H 960
#endif

// Factory's VCOM for this panel (the NVS default LilyGO ships).
#ifndef T5_EPD_VCOM_MV
#define T5_EPD_VCOM_MV 1560
#endif

// Pixel clock. 20 MHz is epdiy's shipped default for this exact panel
// (factory derates to 17). The guard in driveUpdate() steps the clock down
// 1 MHz on any starvation report (floor 5) and repaints, every step logged,
// so the glass converges to whatever the hardware truly sustains.
#ifndef T5_EPD_PIXEL_CLOCK_MHZ
#define T5_EPD_PIXEL_CLOCK_MHZ 20
#endif

template <class EINK> class EINKDriver : public DisplayDriver, public EINKDriverBase
{
  public:
    EINKDriver(uint16_t width, uint16_t height);
    explicit EINKDriver(const DisplayDriverConfig &cfg);
    ~EINKDriver() override;

    void init(DeviceGUI *gui) override;
    void task_handler(void) override;

    bool hasTouch(void) override { return eink->hasTouch(); }
    bool hasButton(void) override { return eink->hasButton(); }
    bool hasLight(void) override { return eink->hasLight(); }

    // Runtime state, not a property of the technology. While the screen has
    // timed out this also keeps the 20 s keepalive off the wire.
    bool isPowersaving(void) override { return powerSaving; }
    void setPowersaving(bool on) { powerSaving = on; }

    uint8_t getBrightness(void) override { return eink->getBrightness(); }
    void setBrightness(uint8_t b) override { eink->setBrightness(b); }

    uint16_t getScreenTimeout(void) override { return screenTimeout / 1000; }
    void setScreenTimeout(uint16_t t) override { screenTimeout = t * 1000; }

    void printConfig(void) override;

  protected:
    static void display_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
    static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data);

    uint32_t screenTimeout = 30 * 1000;
    // True only while the UI has stopped refreshing. The panel keeps showing
    // its last frame either way - that is the point of e-paper.
    bool powerSaving = false;
    // Captured before the timeout zeroes the frontlight, restored on wake.
    uint8_t wakeBrightness = 0;

  private:
    void initPanel(void);

    // Factory sequences, ported call-for-call:
    static void fullClean(void);     // the deghost scrub
    static void refreshScreen(void); // the side-key action
    static void driveUpdate(enum EpdDrawMode mode);

    // Installed on EINKDriverBase in init(); the firmware's sleep and reboot
    // paths call them through there.
    static void parkForSleepImpl(void);
    static void resumeFromSleepImpl(void);

    static EINK *eink;
    static EpdiyHighlevelState hl;
    static bool panelReady;
    static uint8_t *grayFb; // 4bpp, portrait geometry
    static int pixelClockMHz;           // current rate; self-derates on starvation
    static volatile bool repairPending; // a derated update left artifacts; repaint

    // Rail keep-alive: factory cycles the TPS65185 rails around every
    // update; epdiy's poweron/poweroff have no per-update requirement, so
    // the rails stay up for a short window after each update and consecutive
    // interactions drive back-to-back. task_handler drops them when the
    // window expires; the sleep/shutdown paths force them down.
    static bool railsOn;
    static uint32_t railsDeadlineMs;
    static constexpr uint32_t kRailKeepAliveMs = 3000;
    static void railsUp(void);
    static void railsMaybeDown(void);

    uint8_t *drawBuf = nullptr; // LVGL's L8 render target (one full frame, PSRAM)
    size_t drawBufBytes = 0;
};

// ---------------------------------------------------------------------------
// statics
// ---------------------------------------------------------------------------
template <class EINK> EINK *EINKDriver<EINK>::eink = nullptr;
template <class EINK> EpdiyHighlevelState EINKDriver<EINK>::hl;
template <class EINK> bool EINKDriver<EINK>::panelReady = false;
template <class EINK> uint8_t *EINKDriver<EINK>::grayFb = nullptr;
template <class EINK> int EINKDriver<EINK>::pixelClockMHz = T5_EPD_PIXEL_CLOCK_MHZ;
template <class EINK> volatile bool EINKDriver<EINK>::repairPending = false;
template <class EINK> bool EINKDriver<EINK>::railsOn = false;
template <class EINK> uint32_t EINKDriver<EINK>::railsDeadlineMs = 0;

template <class EINK> void EINKDriver<EINK>::railsUp(void)
{
    if (!railsOn) {
        epd_poweron(); // full factory bring-up: WAKEUP/PWRUP/VCOM, PWRGOOD, PG
        railsOn = true;
    }
    railsDeadlineMs = millis() + kRailKeepAliveMs;
}

template <class EINK> void EINKDriver<EINK>::railsMaybeDown(void)
{
    if (railsOn && !refreshBusy && (int32_t)(millis() - railsDeadlineMs) > 0) {
        epd_poweroff();
        railsOn = false;
    }
}

template <class EINK>
EINKDriver<EINK>::EINKDriver(uint16_t width, uint16_t height) : DisplayDriver(width, height)
{
    if (!eink)
        eink = new EINK();
}

template <class EINK>
EINKDriver<EINK>::EINKDriver(const DisplayDriverConfig &cfg) : EINKDriver(cfg.width(), cfg.height())
{
}

template <class EINK> EINKDriver<EINK>::~EINKDriver()
{
    if (panelReady) {
        epd_poweroff();
        panelReady = false;
    }
}

// ---------------------------------------------------------------------------
// panel bring-up - factory order, line for line
// ---------------------------------------------------------------------------
template <class EINK> void EINKDriver<EINK>::initPanel(void)
{
    if (panelReady)
        return;

    eink->preInit(); // board: frontlight pin, CS parking - see T5EpaperBoard.h

    // The one deliberate transport difference from factory bring-up: the
    // factory's epdiy fork creates its own legacy-driver I2C master, which
    // cannot coexist with Arduino core 3.x's Wire (IDF 5 forbids legacy + NG
    // in one binary). epdiy 2.1.3's official bus-injection API shares the
    // bus Wire.begin() already owns - same chips, same registers, same
    // sequencing.
    static EpdI2cConfig i2cCfg;
    static EpdInitConfig initCfg;
    i2cCfg.bus_handle = (i2c_master_bus_handle_t)i2cBusHandle(0);
    initCfg.i2c = &i2cCfg;
    if (!i2cCfg.bus_handle) {
        ILOG_CRIT("EINKDriver: Wire bus handle unavailable - running headless");
        return;
    }

    epd_init_with_config(&epd_board_v7, &ED047TC1, EPD_LUT_64K, &initCfg);
    epd_set_vcom(T5_EPD_VCOM_MV);
    hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    epd_set_lcd_pixel_clock_MHz(pixelClockMHz);

    if (epd_rotated_display_width() != T5_PORTRAIT_W || epd_rotated_display_height() != T5_PORTRAIT_H) {
        ILOG_CRIT("EINKDriver: rotated panel is %dx%d, expected %dx%d - running headless",
                  epd_rotated_display_width(), epd_rotated_display_height(), T5_PORTRAIT_W, T5_PORTRAIT_H);
        return;
    }

    // Factory's decode buffer: 4bpp in rotated (portrait) geometry.
    const size_t graySize = (size_t)((T5_PORTRAIT_W + 1) / 2) * T5_PORTRAIT_H;
    grayFb = (uint8_t *)heap_caps_calloc(1, graySize, MALLOC_CAP_SPIRAM);
    if (!grayFb) {
        ILOG_CRIT("EINKDriver: no PSRAM for %u B gray buffer - running headless", (unsigned)graySize);
        return;
    }

    // Boot clear, factory order: poweron, clear, poweroff.
    epd_poweron();
    epd_clear();
    epd_poweroff();

    eink->postInit(); // touch, in the same phase factory inits its GT911

    panelReady = true;
    ILOG_INFO("EINKDriver: epdiy up - %dx%d portrait, VCOM %d mV, %d MHz, 4bpp", T5_PORTRAIT_W, T5_PORTRAIT_H,
              (int)T5_EPD_VCOM_MV, pixelClockMHz);
}

// ---------------------------------------------------------------------------
// lvgl wiring
// ---------------------------------------------------------------------------
template <class EINK> void EINKDriver<EINK>::init(DeviceGUI *gui)
{
    DisplayDriver::init(gui);

    // THE LVGL TICK SOURCE. device-ui provides no tick on Arduino; every
    // working MUI board gets it from its display driver. Without it
    // lv_tick_get() is 0 forever and no LVGL timer ever fires - no refresh,
    // no input polling.
    lv_tick_set_cb(xTaskGetTickCount);

    initPanel();

    display = lv_display_create(T5_PORTRAIT_W, T5_PORTRAIT_H);
    if (!display) {
        ILOG_CRIT("EINKDriver: lv_display_create failed");
        return;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_L8);

    // One full L8 frame in PSRAM. FULL render mode is factory's
    // full_refresh=1: LVGL re-renders the whole screen per refresh cycle and
    // hands it over in one flush - a straight full-frame conversion with no
    // partial-area bookkeeping.
    drawBufBytes = (size_t)T5_PORTRAIT_W * T5_PORTRAIT_H;
    drawBuf = (uint8_t *)heap_caps_aligned_alloc(16, drawBufBytes, MALLOC_CAP_SPIRAM);
    if (!drawBuf) {
        ILOG_CRIT("EINKDriver: no PSRAM for %u B draw buffer", (unsigned)drawBufBytes);
        lv_display_delete(display);
        display = nullptr;
        return;
    }
    lv_display_set_buffers(display, drawBuf, nullptr, drawBufBytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, EINKDriver::display_flush);

    if (eink->hasTouch()) {
        touch = lv_indev_create();
        lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch, EINKDriver::touchpad_read);
        lv_indev_set_display(touch, display);
    }

    // The firmware's sleep and reboot paths park the panel through these.
    EINKDriverBase::setSleepHooks(&EINKDriver::parkForSleepImpl, &EINKDriver::resumeFromSleepImpl);
}

// ---------------------------------------------------------------------------
// the factory pipeline: L8 -> 4bpp -> rotated draw -> one mode-selected update
// ---------------------------------------------------------------------------
template <class EINK> void EINKDriver<EINK>::driveUpdate(enum EpdDrawMode mode)
{
    // Rails via the keep-alive manager; refreshBusy lets the sleep/reboot
    // park wait a waveform out.
    const float temperature = epd_ambient_temperature();
    refreshBusy = true;
    railsUp();
    enum EpdDrawError err = epd_hl_update_screen(&hl, mode, temperature);
    railsDeadlineMs = millis() + kRailKeepAliveMs; // window restarts at completion
    refreshBusy = false;
    if (err != EPD_DRAW_SUCCESS)
        ILOG_ERROR("EINKDriver: update err=%X mode=%X", (unsigned)err, (unsigned)mode);
    if (err & EPD_DRAW_EMPTY_LINE_QUEUE) {
        // The hardware's own verdict that the clock is too fast for the feed
        // bandwidth: apply epdiy's documented remedy and schedule one clean
        // repaint of whatever this update mangled.
        if (pixelClockMHz > 5) {
            pixelClockMHz -= 1;
            epd_set_lcd_pixel_clock_MHz(pixelClockMHz);
            ILOG_ERROR("EINKDriver: line starvation - pixel clock derated to %d MHz", pixelClockMHz);
        }
        repairPending = true;
    }
}

/** Factory full-clean scrub: 10 black pushes, 10 white, 2 no-op, 12 ms
 *  each - the deghost scrub inside refreshScreen(). */
template <class EINK> void EINKDriver<EINK>::fullClean(void)
{
    const int t = 12; // factory refresh timer
    refreshBusy = true;
    railsUp();
    for (int i = 0; i < 10; i++)
        epd_push_pixels(epd_full_screen(), t, 0);
    for (int i = 0; i < 10; i++)
        epd_push_pixels(epd_full_screen(), t, 1);
    for (int i = 0; i < 2; i++)
        epd_push_pixels(epd_full_screen(), t, 2);
    railsDeadlineMs = millis() + kRailKeepAliveMs;
    refreshBusy = false;
}

/** Factory refresh-screen sequence - the side-key action: full clean, flash
 *  white with DU, redraw the current image with GL16. */
template <class EINK> void EINKDriver<EINK>::refreshScreen(void)
{
    if (!panelReady || !grayFb)
        return;
    EpdRect all = {.x = 0, .y = 0, .width = epd_rotated_display_width(), .height = epd_rotated_display_height()};
    fullClean();
    epd_hl_set_all_white(&hl);
    driveUpdate(MODE_DU);
    epd_draw_rotated_image(all, grayFb, epd_hl_get_framebuffer(&hl));
    driveUpdate(MODE_GL16);
}

template <class EINK> void EINKDriver<EINK>::display_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area; // FULL render mode: always the whole screen

    if (!panelReady || !grayFb) {
        lv_display_flush_ready(disp);
        return;
    }

    // Screen timeout: keep accepting frames, drive nothing. The wake path
    // invalidates the whole screen and FULL mode re-renders everything, so
    // the first flush after wake carries the complete current state.
    if (refreshSuspended) {
        lv_display_flush_ready(disp);
        return;
    }

    // L8 -> 4bpp, two pixels per byte, portrait geometry - the same buffer
    // the factory fills, produced linearly instead of per-pixel calls.
    const int32_t w = T5_PORTRAIT_W;
    const int32_t h = T5_PORTRAIT_H;
    const int32_t stride = (w + 1) / 2;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *src = px_map + (size_t)y * w;
        uint8_t *dst = grayFb + (size_t)y * stride;
        for (int32_t x = 0; x < w; x += 2) {
            // low nibble = even pixel, high nibble = odd (epdiy 4bpp layout)
            uint8_t b = (uint8_t)(src[x] >> 4);
            if (x + 1 < w)
                b |= (uint8_t)(src[x + 1] & 0xF0);
            *dst++ = b;
        }
    }

    // Rotated draw into the highlevel framebuffer, then one update whose
    // mode is the refresh setting. Order matters in the NEAT branches:
    // factory NEAT runs a TRACKED transition to white first - one the
    // difference buffer records - and only then draws the frame and drives
    // GC16, so every pixel's (from -> to) is known. The raw scrub belongs
    // only inside refreshScreen(), where factory pairs it with set_all_white.
    EpdRect all = {.x = 0, .y = 0, .width = epd_rotated_display_width(), .height = epd_rotated_display_height()};

    if (shutdownSplash) {
        // Factory power-off sequence: full clean -> all-white MODE_GC16 ->
        // draw image MODE_GL16. The raw scrub neutralizes accumulated charge
        // and the GC16 pass flash-erases the field BEFORE the image, so
        // nothing of the old screen ghosts through the white field. All of
        // it runs at the calibrated VCOM.
        shutdownSplash = false;
        neatPending = false;
        fullClean();
        epd_hl_set_all_white(&hl);
        driveUpdate(MODE_GC16);
        epd_draw_rotated_image(all, grayFb, epd_hl_get_framebuffer(&hl));
        driveUpdate(MODE_GL16);
        // TERMINAL: the splash is the last update before the firmware parks
        // the panel and deep sleeps. Later flushes are swallowed by the
        // refreshSuspended gate; wake from forever-sleep is a reboot.
        refreshSuspended = true;
    } else if (neatPending) {
        // Factory NEAT: tracked white + draw + GC16.
        neatPending = false;
        epd_hl_set_all_white(&hl);
        driveUpdate(MODE_GL16);
        epd_draw_rotated_image(all, grayFb, epd_hl_get_framebuffer(&hl));
        driveUpdate(MODE_GC16);
    } else if (refreshMode == EinkRefresh::Fast || unlimitedFast) {
        epd_draw_rotated_image(all, grayFb, epd_hl_get_framebuffer(&hl));
        driveUpdate(MODE_DU); // factory FAST
    } else {
        epd_draw_rotated_image(all, grayFb, epd_hl_get_framebuffer(&hl));
        driveUpdate(MODE_GL16); // factory NORMAL
    }
    lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// task handler: screen timeout, BOOT wake, side key
// ---------------------------------------------------------------------------
template <class EINK> void EINKDriver<EINK>::task_handler(void)
{
    DisplayDriver::task_handler();

    // Rail keep-alive expiry - independent of every gate below, so the rails
    // always come down within the window of the last update.
    railsMaybeDown();

    // Screen timeout. Expiry: the view shows the power-off screen, ONE clean
    // pass drives it to glass, and only then do refreshes suspend and the
    // frontlight drop - order matters, or the OFF image would be swallowed
    // by its own suspension. Activity: restore, repaint everything, NEAT.
    if (screenTimeout > 0 && hasLight()) {
        const bool idle = lv_display_get_inactive_time(NULL) > screenTimeout;
        if (idle && !powerSaving) {
            wakeBrightness = getBrightness();
            powerSaving = true;
            if (DisplayDriver::view)
                DisplayDriver::view->blankScreen(true);
            requestNeatRefresh(); // held image deserves a clean GC16
            lv_refr_now(NULL);    // drive it BEFORE suspending
            setBrightness(0);
            suspendRefresh(true);
        } else if (!idle && powerSaving) {
            powerSaving = false;
            suspendRefresh(false);
            setBrightness(wakeBrightness);
            if (DisplayDriver::view)
                DisplayDriver::view->blankScreen(false);
            lv_obj_invalidate(lv_screen_active());
            requestNeatRefresh();
        }
    }

    // A starved update left blanked lines on glass (and the clock has been
    // derated). One full repaint through the NEAT path restores the frame;
    // consumed here rather than inside the flush, which must not recurse.
    if (repairPending && panelReady && !refreshSuspended) {
        repairPending = false;
        lv_obj_invalidate(lv_screen_active());
        requestNeatRefresh();
    }

    // BOOT wakes the display: the pin is the board's wake key, and nothing
    // else routes a press into LVGL's inactivity timer.
    if (panelReady) {
        static uint8_t lastBoot = 1; // idle high, pulled up
        const uint8_t boot = digitalRead(BUTTON_PIN) ? 1 : 0;
        if (lastBoot && !boot)
            lv_display_trigger_activity(NULL);
        lastBoot = boot;
    }

    // Side key = screen refresh, factory's assignment: poll the expander INT
    // line every 300 ms, touch I2C only when it is low. pinMode once - core
    // 3.x's periman refuses reads on unconfigured pins.
    if (panelReady) {
        static uint32_t lastPollMs = 0;
        static bool intPinReady = false;
        if (!intPinReady) {
            pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
            intPinReady = true;
        }
        const uint32_t now = millis();
        if ((uint32_t)(now - lastPollMs) >= 300) {
            lastPollMs = now;
            if (digitalRead(BOARD_PCA9535_INT) == LOW && eink->sideKeyPressed()) {
                ILOG_INFO("EINKDriver: side-key refresh");
                refreshScreen();
                lv_display_trigger_activity(NULL);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// sleep/reboot parking. Factory sleeps the panel and touch before the CPU
// stops. The radio is deliberately untouched: light sleep keeps the SX1262
// receiving via DIO1 wake, and deep sleep powers it down on its own.
// ---------------------------------------------------------------------------
template <class EINK> void EINKDriver<EINK>::parkForSleepImpl(void)
{
    const uint32_t deadline = millis() + 3000;
    while (refreshBusy && (int32_t)(millis() - deadline) < 0)
        delay(10);
    if (panelReady) {
        epd_poweroff(); // unconditional: sleep never leaves keep-alive rails up
        railsOn = false;
    }
    if (eink)
        eink->sleepTouch();
}

template <class EINK> void EINKDriver<EINK>::resumeFromSleepImpl(void)
{
    // Rails come back with the next update's poweron; only touch needs waking.
    if (eink)
        eink->wakeTouch();
}

template <class EINK> void EINKDriver<EINK>::touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    // No transform: the GT911 reports in the 540x960 portrait frame, as the
    // factory's own LVGL callback does.
    static int16_t lastX = 0, lastY = 0;
    int16_t x, y;
    if (eink && eink->getTouch(x, y)) {
        lastX = x;
        lastY = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = lastX;
    data->point.y = lastY;
}

template <class EINK> void EINKDriver<EINK>::printConfig(void)
{
    ILOG_DEBUG("EINKDriver: epdiy %dx%d portrait 4bpp, VCOM %d, mode %d", T5_PORTRAIT_W, T5_PORTRAIT_H,
               (int)T5_EPD_VCOM_MV, (int)refreshMode);
}
