#pragma once

/**
 * T5EpaperBoard - the board adapter EINKDriver is templated on.
 *
 * Owns what epdiy does not: the GT911 touch controller, the PT4103
 * frontlight, the PCA9535 side-key readout and CS parking before panel
 * bring-up. The panel itself (timing, waveforms, rails, expander
 * sequencing) is epdiy's epd_board_v7.
 */

#include <Arduino.h>
#include <TouchDrvGT911.hpp>
#include <Wire.h>

// LilyGO pin map (T5 E-Paper S3 Pro V2).
#define T5_I2C_SDA 39
#define T5_I2C_SCL 40
#define T5_TOUCH_INT 3
#define T5_TOUCH_RST 9
#define T5_BL_EN 11 // PT4103 frontlight
#define T5_LORA_CS 46
#define T5_SD_CS 12
#define T5_PCA9535_INT 38

// Vendor frontlight duty cycles.
static const uint8_t T5_BACKLIGHT_LEVELS[4] = {0, 50, 100, 230};

#if defined(T5_S3_EPAPER_PRO)
// Defined by the variant: publishes the current frontlight duty so the
// sleep and shutdown paths can restore the user's level.
void t5BacklightSetDuty(uint8_t duty);
#endif

class T5EpaperBoard
{
  public:
    bool hasTouch(void) const { return touchOk; }
    bool hasButton(void) const { return true; } // BOOT (GPIO0) + PCA9535 side key
    bool hasLight(void) const { return true; }  // PT4103 frontlight

    // Buses up, before initPanel(). Both SPI chip selects are de-asserted:
    // LoRa and microSD share the SPI bus with display-adjacent traffic.
    void preInit(void)
    {
        pinMode(T5_LORA_CS, OUTPUT);
        digitalWrite(T5_LORA_CS, HIGH);
        pinMode(T5_SD_CS, OUTPUT);
        digitalWrite(T5_SD_CS, HIGH);

        Wire.begin(T5_I2C_SDA, T5_I2C_SCL);
        initBacklight();
    }

    // After initPanel(), once the I2C-side rails are up.
    void postInit(void) { initTouch(); }

    // The GT911 reports directly in the 540x960 portrait frame; no transform.
    bool getTouch(int16_t &x, int16_t &y)
    {
        if (!touchOk || !touchEnabled)
            return false;
        // Gate on the INT line before touching the bus: under LOW_LEVEL_QUERY
        // isPressed() is a bare digitalRead, so an idle screen costs no I2C
        // on a bus shared with the expander, charger, gauge and RTC. The
        // capacitive HOME key still arrives through the same gate.
        if (!touch.isPressed())
            return false;
        return touch.getPoint(&x, &y, 1) > 0;
    }

    void setTouchEnabled(bool on) { touchEnabled = on; }

    // Side key state: PCA9535 input port 1, bit 2, pressed = low. The caller
    // has already gated on the expander INT line.
    bool sideKeyPressed(void)
    {
        Wire.beginTransmission(0x20);
        Wire.write(0x01); // REG_INPUT_PORT1
        if (Wire.endTransmission() != 0)
            return false;
        if (Wire.requestFrom((uint8_t)0x20, (uint8_t)1) != 1)
            return false;
        const uint8_t port1 = (uint8_t)Wire.read();
        return (port1 & 0x04) == 0;
    }

    // Called from the firmware's sleep observers; the GT911 otherwise keeps
    // scanning through sleep.
    void sleepTouch(void)
    {
        if (touchOk)
            touch.sleep();
    }
    void wakeTouch(void)
    {
        if (touchOk)
            touch.wakeup();
    }

    // Frontlight: 4 discrete levels.
    uint8_t getBrightness(void) const { return T5_BACKLIGHT_LEVELS[blLevel]; }

    void setBrightness(uint8_t b)
    {
        uint8_t lvl = 0;
        for (uint8_t i = 0; i < 4; i++)
            if (b >= T5_BACKLIGHT_LEVELS[i])
                lvl = i;
        setBacklightLevel(lvl);
    }

    void setBacklightLevel(uint8_t lvl)
    {
        if (lvl > 3)
            lvl = 3;
        blLevel = lvl;
        // analogWrite, not ledcWrite: claiming the pin with ledcAttach makes
        // core 3.x's periman reject the digitalWrite the sleep and shutdown
        // paths use on this pin.
        analogWrite(T5_BL_EN, T5_BACKLIGHT_LEVELS[lvl]);
#if defined(T5_S3_EPAPER_PRO)
        t5BacklightSetDuty(T5_BACKLIGHT_LEVELS[lvl]);
#endif
    }

    uint8_t getBacklightLevel(void) const { return blLevel; }

  private:
    void initTouch(void)
    {
        // The GT911's I2C address is latched at reset by the INT level, so
        // setPins() must precede begin(), which owns the reset. 0x5D is the
        // address the vendor latches on this board.
        touch.setPins(T5_TOUCH_RST, T5_TOUCH_INT);
        touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_L, T5_I2C_SDA, T5_I2C_SCL);
        if (!touchOk) {
            ILOG_CRIT("GT911 not found at 0x%02X - the UI has no input", GT911_SLAVE_ADDRESS_L);
        }
        if (touchOk) {
            // The round front button is the GT911's capacitive home key;
            // route it to the view through the driver base.
            touch.setHomeButtonCallback([](void *) { EINKDriverBase::notifyHomeButton(); }, nullptr);
            // SensorLib's inline code uses the name but the pinned version
            // never defines it.
#ifndef LOW_LEVEL_QUERY
#define LOW_LEVEL_QUERY 0x03
#endif
            touch.setInterruptMode(LOW_LEVEL_QUERY);
        }
    }

    void initBacklight(void)
    {
        // analogWrite attaches LEDC on first use; nothing to claim up front.
        pinMode(T5_BL_EN, OUTPUT);
        setBacklightLevel(0);
    }

    TouchDrvGT911 touch;
    bool touchOk = false;
    bool touchEnabled = true;
    uint8_t blLevel = 0;
};

// DisplayDriverFactory's CUSTOM_EINK branch instantiates EINKDriver<EINKConfig>.
using EINKConfig = T5EpaperBoard;
