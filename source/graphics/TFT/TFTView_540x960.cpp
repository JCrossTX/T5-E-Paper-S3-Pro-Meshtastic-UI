#if HAS_TFT // VIEW_540x960

#include "graphics/view/TFT/TFTView_540x960.h"
#include "UiIndex.h"
#include "assets/poweroff.h"
#include "graphics/common/LoRaPresets.h" // upstream's bandwidth strings
#include "graphics/common/SdCard.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "graphics/driver/EInkRefreshPolicy.h"
#include "lora_freq.h" // generated: RF tables + the radio's frequency math
#include "mesh/HardwareRNG.h"
#include "ui.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime> // message bubble timestamps

#if defined(T5_S3_EPAPER_PRO)
// Defined by the variant: true only when a power-off would take effect.
bool t5CanPowerOff();
#endif

TFTView_540x960 *TFTView_540x960::gui = nullptr;

TFTView_540x960 *TFTView_540x960::instance(void)
{
    if (!gui)
        gui = new TFTView_540x960(nullptr, DisplayDriverFactory::create(540, 960));
    return gui;
}

TFTView_540x960 *TFTView_540x960::instance(const DisplayDriverConfig &cfg)
{
    if (!gui)
        gui = new TFTView_540x960(&cfg, DisplayDriverFactory::create(cfg));
    return gui;
}

TFTView_540x960::TFTView_540x960(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : MeshtasticView(cfg, driver, new ViewController)
{
}

void TFTView_540x960::init(IClientBase *client)
{
    ILOG_DEBUG("TFTView_540x960 init...");

    // Display, LVGL and the controller; screen construction is the view's job.
    MeshtasticView::init(client);

    // objects.* is null until this runs; it must precede ui_events_init().
    ui_init_boot();

    ui_events_init();
    navReset(objects.home_button, objects.home_panel, nullptr);

    // The controller requests config only once the boot screen is done; this
    // port has no boot-logo hold, so the config stream starts immediately.
    state = MeshtasticView::eBootScreenDone;

    ILOG_INFO("TFTView_540x960: settings surface up - %u regions, %u presets, %u languages",
              (unsigned)kRegionCount, (unsigned)kPresetCount, (unsigned)kLanguageCount);
}

void TFTView_540x960::task_handler(void)
{
    // The base runs the lvgl timer, pumps the controller, stamps curtime and
    // sends the heartbeat.
    MeshtasticView::task_handler();

    if (state != eRunning && state != eConfigComplete && state != eMessagesRestored) {
        // Drop any home press that arrived before the UI was ready.
        (void)EINKDriverBase::takeHomeButton();
        return;
    }

    // The capacitive HOME key: straight home from anywhere, including an open
    // modal or text overlay - the hardware escape route on a touch-only UI.
    if (EINKDriverBase::takeHomeButton()) {
        // The GT911 home key does not go through an indev, so count the press
        // as activity for the screen timeout.
        lv_display_trigger_activity(NULL);
        if (modalOpen) {
            lv_obj_add_flag(objects.pairing_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.editor_entry_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.editor_action_panel, LV_OBJ_FLAG_HIDDEN);
            EINKDriverBase::enableUnlimitedFast(false);
            textTarget = TEXT_NONE;
            modalOpen = false;
        }
        refreshHome();
        navReset(objects.home_button, objects.home_panel, nullptr);
    }

    // Periodic work, split by how fast the driven value changes: 1 s for the
    // home clock row (only while showing), 60 s for last-heard ageing.
    if (curtime != lastTick1) {
        lastTick1 = curtime;
        if (!lv_obj_has_flag(objects.home_panel, LV_OBJ_FLAG_HIDDEN))
            refreshHome();
    }

    if (curtime - lastTick60 >= 60) {
        lastTick60 = curtime;
        updateAllLastHeard();
        pollSDCard();
    }
}

// ---------------------------------------------------------------------------
// SD card. The slot is on the board's shared SPI bus, so this is polled rather
// than watched: there is no card-detect line wired on the T5 S3 Pro.
// ---------------------------------------------------------------------------
void TFTView_540x960::pollSDCard(void)
{
#if defined(HAS_SDCARD) || defined(HAS_SD_MMC)
    if (!sdCard)
        return;
    if (!sdCard->init()) {
        sdUsedMB = sdTotalMB = 0;
        return;
    }
    const uint64_t used = sdCard->usedBytes();
    const uint64_t total = used + sdCard->freeBytes();
    sdUsedMB = (uint32_t)(used / (1024ULL * 1024ULL));
    sdTotalMB = (uint32_t)(total / (1024ULL * 1024ULL));
#else
    // No card slot compiled in: the home row says "no card" and means it.
    sdUsedMB = sdTotalMB = 0;
#endif
}

// ---------------------------------------------------------------------------
// change gating - one visible update is one full-panel refresh (~1 s), so
// compare the rendered string and repaint only on difference.
// ---------------------------------------------------------------------------
bool TFTView_540x960::setLabelIfChanged(lv_obj_t *label, char *cache, size_t cacheLen, const char *text)
{
    if (!label || !text)
        return false;
    if (strncmp(cache, text, cacheLen) == 0)
        return false;
    strncpy(cache, text, cacheLen - 1);
    cache[cacheLen - 1] = '\0';
    lv_label_set_text(label, text);
    return true;
}

// ---------------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------------

// Incoming-message banner: takes the wordmark band only, so no data row is
// obscured. Cleared by a 5 s one-shot timer or the next navigation,
// whichever comes first; the logo returns on the same partial update.
static lv_timer_t *notifyTimer = nullptr;

static void notifyTimerCb(lv_timer_t *t)
{
    (void)t;
    notifyTimer = nullptr; // one-shot: LVGL deletes the timer after this returns
    if (objects.notify_panel)
        lv_obj_add_flag(objects.notify_panel, LV_OBJ_FLAG_HIDDEN);
}

static void hideNotifyBanner(void)
{
    if (notifyTimer) {
        lv_timer_delete(notifyTimer);
        notifyTimer = nullptr;
    }
    if (objects.notify_panel)
        lv_obj_add_flag(objects.notify_panel, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_540x960::ui_set_active(lv_obj_t *button, lv_obj_t *panel, const char *context)
{
    // Any navigation dismisses the banner (whichever-first spec).
    hideNotifyBanner();
    static lv_obj_t *shown = nullptr;
    if (shown && shown != panel)
        lv_obj_add_flag(shown, LV_OBJ_FLAG_HIDDEN);

    // Unlimited-fast exists for keyboard echo. Every navigation runs through
    // here, so this one clear covers all exits from the messages panel;
    // entering it re-enables the flag right after this call.
    if (panel != objects.messages_panel)
        EINKDriverBase::enableUnlimitedFast(false);

    lv_obj_t *navButtons[6] = {objects.home_button,     objects.nodes_button, objects.groups_button,
                               objects.messages_button, objects.map_button,   objects.settings_button};
    for (uint8_t i = 0; i < 6; i++) {
        // 1-bit panel: the active tab inverts. Never colour.
        if (navButtons[i] == button)
            lv_obj_add_state(navButtons[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(navButtons[i], LV_STATE_CHECKED);
    }

    if (panel) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        shown = panel;
    }

    if (context) {
        lv_obj_remove_flag(objects.context_panel, LV_OBJ_FLAG_HIDDEN);
        setLabelIfChanged(objects.context_label, cacheContext, sizeof(cacheContext), context);
    } else {
        lv_obj_add_flag(objects.context_panel, LV_OBJ_FLAG_HIDDEN);
        cacheContext[0] = '\0';
    }
}

void TFTView_540x960::navPush(lv_obj_t *button, lv_obj_t *panel, const char *context)
{
    if (navDepth >= NAV_STACK_MAX) {
        // Overflow drops the OLDEST entry.
        memmove(&navStack[0], &navStack[1], (NAV_STACK_MAX - 1) * sizeof(NavEntry));
        navDepth--;
    }
    ui_set_active(button, panel, context);
    NavEntry &e = navStack[navDepth++];
    e.button = button;
    e.panel = panel;
    e.hasContext = (context != nullptr);
    if (context) {
        strncpy(e.context, context, sizeof(e.context) - 1);
        e.context[sizeof(e.context) - 1] = '\0';
    } else {
        e.context[0] = '\0';
    }
    updateBackButton();
}

void TFTView_540x960::navPop(void)
{
    if (navDepth <= 1)
        return; // nav-rail root: Back is inert here
    // Back out of the channel editor discards its staged copy (same as
    // CANCEL); only the two-tap rekey arm needs an explicit clear.
    if (navStack[navDepth - 1].panel == objects.channel_edit_panel)
        chanRekeyArmed = false;
    navDepth--;
    const NavEntry &e = navStack[navDepth - 1];
    ui_set_active(e.button, e.panel, e.hasContext ? e.context : nullptr);
    updateBackButton();

    // Returning to the setup gate from the region picker: re-render, since
    // the picker only stages pendingRegionIdx.
    if (e.panel == objects.setup_panel)
        refreshSetup();
}

void TFTView_540x960::navReset(lv_obj_t *button, lv_obj_t *panel, const char *context)
{
    ILOG_DEBUG("nav: reset -> %s", context ? context : "HOME");
    navDepth = 0;
    navPush(button, panel, context);
}

void TFTView_540x960::updateBackButton(void)
{
    // The back button renders identically at all times and is simply inert
    // when Back has no meaning; inertness is enforced in the handlers
    // (navPop refuses at a nav root, ui_event_Back while a modal owns
    // dismissal), never via a DISABLED visual state.
    lv_obj_remove_state(objects.back_button, LV_STATE_DISABLED);
    lv_obj_remove_state(objects.back_label, LV_STATE_DISABLED);
    if (objects.back_chevron)
        lv_obj_set_style_line_color(objects.back_chevron, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
}

// ---------------------------------------------------------------------------
// Settings surface: one screen, five radio option pickers. Every control
// edits PENDING state only; the single publisher is saveAndReboot().
// Backing out of an option screen keeps the choice pending.
// ---------------------------------------------------------------------------
void TFTView_540x960::showSettings(void)
{
    refreshSettings();
    navReset(objects.settings_button, objects.settings_panel, "SETTINGS");
}

void TFTView_540x960::refreshSettings(void)
{
    char buf[96];

    // Text rows show the pending strings directly.
    lv_textarea_set_text(objects.settings_long_name, db.user.long_name);
    lv_textarea_set_text(objects.settings_short_name, db.user.short_name);
    lv_textarea_set_text(objects.settings_wifi_ssid, db.config.network.wifi_ssid);
    lv_textarea_set_text(objects.settings_wifi_psk, db.config.network.wifi_psk);

    snprintf(buf, sizeof(buf), "REGION: %s",
             pendingRegionIdx >= 0 ? kRegionNames[pendingRegionIdx] : "UNSET");
    lv_label_set_text(objects.settings_region_label, buf);

    // CUSTOM = use_preset=false: modem parameters no preset describes.
    snprintf(buf, sizeof(buf), "MODEM PRESET: %s",
             pendingPresetIdx >= 0 ? kPresetNames[pendingPresetIdx] : "CUSTOM");
    lv_label_set_text(objects.settings_preset_label, buf);

    if (pendingTimeoutIdx >= 0)
        snprintf(buf, sizeof(buf), "SCREEN TIMEOUT: %s", kTimeoutNames[pendingTimeoutIdx]);
    else // whatever the radio holds, shown as-is - a phone may have set it
        snprintf(buf, sizeof(buf), "SCREEN TIMEOUT: %uS", (unsigned)db.config.display.screen_on_secs);
    lv_label_set_text(objects.settings_timeout_label, buf);

    if (pendingBrightnessIdx >= 0)
        snprintf(buf, sizeof(buf), "BRIGHTNESS: %s", kBrightnessNames[pendingBrightnessIdx]);
    else
        snprintf(buf, sizeof(buf), "BRIGHTNESS: %u", (unsigned)db.uiConfig.screen_brightness);
    lv_label_set_text(objects.settings_brightness_label, buf);

    if (pendingLanguageIdx >= 0)
        snprintf(buf, sizeof(buf), "LANGUAGE: %s", kLanguageNames[pendingLanguageIdx]);
    else
        snprintf(buf, sizeof(buf), "LANGUAGE: %u", (unsigned)db.uiConfig.language);
    lv_label_set_text(objects.settings_language_label, buf);

    // SAVE stays dithered until the config stream has seeded the shadow;
    // publishing an all-zero shadow would zero the radio config.
    if (configSeeded) {
        lv_obj_remove_state(objects.settings_save_button, LV_STATE_DISABLED);
        lv_obj_remove_state(objects.settings_save_label, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(objects.settings_save_button, LV_STATE_DISABLED);
        lv_obj_add_state(objects.settings_save_label, LV_STATE_DISABLED);
    }
}

void TFTView_540x960::openOption(uint8_t kind)
{
    lv_obj_t *panel = optPanel(kind);
    if (!panel)
        return;
    refreshOptionChecks(kind);
    static const char *titles[OPT_COUNT] = {"SET REGION", "SET MODEM PRESET", "SET SCREEN TIMEOUT",
                                            "SET BRIGHTNESS", "SET LANGUAGE"};
    navPush(objects.settings_button, panel, titles[kind]);
}

void TFTView_540x960::refreshOptionChecks(uint8_t kind)
{
    // Radio semantics on the whole list: exactly the pending row is CHECKED.
    lv_obj_t *list = optList(kind);
    if (!list)
        return;
    int16_t sel;
    switch (kind) {
    case OPT_REGION: sel = pendingRegionIdx; break;
    case OPT_PRESET: sel = pendingPresetIdx; break;
    case OPT_TIMEOUT: sel = pendingTimeoutIdx; break;
    case OPT_BRIGHTNESS: sel = pendingBrightnessIdx; break;
    case OPT_LANGUAGE: sel = pendingLanguageIdx; break;
    default: return;
    }
    const uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(list, (int32_t)i);
        if ((int16_t)i == sel)
            lv_obj_add_state(b, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(b, LV_STATE_CHECKED);
    }
    // Bring the selection into view, so a 4-page region list opens on the
    // user's own region rather than at the top.
    if (sel >= 0) {
        lv_obj_t *b = lv_obj_get_child(list, sel);
        if (b)
            lv_obj_scroll_to_view(b, LV_ANIM_OFF);
    }
}

void TFTView_540x960::ui_event_OptRow(lv_event_t *e)
{
    if (!gui)
        return;
    const uint16_t packed = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    const uint8_t kind = (uint8_t)(packed >> 8);
    const int16_t idx = (int16_t)(packed & 0xFF);

    // Range-check before storing: no index may read outside its table.
    static const uint8_t kOptCounts[OPT_COUNT] = {kRegionCount, kPresetCount, kTimeoutCount,
                                                  kBrightnessCount, kLanguageCount};
    if (kind >= OPT_COUNT || idx < 0 || idx >= (int16_t)kOptCounts[kind]) {
        ILOG_ERROR("opt row out of range: kind %u idx %d", (unsigned)kind, (int)idx);
        return;
    }

    // Each pick marks only its own group dirty; SAVE publishes just those.
    switch (kind) {
    case OPT_REGION: gui->pendingRegionIdx = idx; gui->dirtyLora = true; break;
    case OPT_PRESET: gui->pendingPresetIdx = idx; gui->dirtyLora = true; break;
    case OPT_TIMEOUT: gui->pendingTimeoutIdx = idx; gui->dirtyDisplay = true;
        gui->dirtyUiConfig = true; break; // timeout lives in both
    case OPT_BRIGHTNESS: gui->pendingBrightnessIdx = idx; gui->dirtyUiConfig = true; break;
    case OPT_LANGUAGE: gui->pendingLanguageIdx = idx; gui->dirtyUiConfig = true; break;
    default: return;
    }
    // Re-assert radio state: the previous selection still has to be cleared.
    gui->refreshOptionChecks(kind);
    gui->refreshSettings();
}

void TFTView_540x960::ui_event_SettingsRow(lv_event_t *e)
{
    if (!gui)
        return;
    switch ((uint8_t)(uintptr_t)lv_event_get_user_data(e)) {
    case SROW_REGION: gui->openOption(OPT_REGION); return;
    case SROW_PRESET: gui->openOption(OPT_PRESET); return;
    case SROW_TIMEOUT: gui->openOption(OPT_TIMEOUT); return;
    case SROW_BRIGHTNESS: gui->openOption(OPT_BRIGHTNESS); return;
    case SROW_LANGUAGE: gui->openOption(OPT_LANGUAGE); return;

    case SROW_BLUETOOTH:
        // Immediate action, not a staged edit.
        gui->showPairing();
        return;

    case SROW_REBOOT:
        // Immediate action, not a staged edit.
        gui->controller->requestReboot(5, gui->ownNode);
        gui->messageAlert("REBOOTING", true);
        return;

    case SROW_SHUTDOWN:
        if (!gui->canShutdown)
            return;
#if defined(T5_S3_EPAPER_PRO)
        // Power-off is BATFET_DIS on the BQ25896, which has no authority
        // while VBUS holds the system up - check before acting.
        if (!t5CanPowerOff()) {
            gui->messageAlert("UNPLUG USB TO POWER OFF", true);
            return;
        }
#endif
        // Splash first and synchronously: e-paper keeps the last driven frame,
        // and the panel write must not race the shutdown timer.
        gui->notifyShutdown();
        gui->controller->requestShutdown(5, gui->ownNode);
        return;

    case SROW_SAVE:
        gui->saveAndReboot();
        return;

    default:
        return;
    }
}

void TFTView_540x960::ui_event_SettingsText(lv_event_t *e)
{
    if (!gui)
        return;
    const TextTarget t = (TextTarget)(uintptr_t)lv_event_get_user_data(e);
    switch (t) {
    case TEXT_OWNER_LONG:
        gui->openTextEntry(TEXT_OWNER_LONG, "USER NAME (LONG)", gui->db.user.long_name);
        return;
    case TEXT_OWNER_SHORT:
        gui->openTextEntry(TEXT_OWNER_SHORT, "USER NAME (SHORT)", gui->db.user.short_name);
        return;
    case TEXT_WIFI_SSID:
        gui->openTextEntry(TEXT_WIFI_SSID, "WIFI SSID", gui->db.config.network.wifi_ssid);
        return;
    case TEXT_WIFI_PSK:
        gui->openTextEntry(TEXT_WIFI_PSK, "WIFI PASSWORD", gui->db.config.network.wifi_psk);
        return;
    default:
        return;
    }
}

void TFTView_540x960::ui_event_ScrollArrow(lv_event_t *e)
{
    // user_data is the scroll container; direction comes from the button's own
    // y position - up arrows sit at the top of the viewport.
    lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!container || !btn)
        return;
    const bool up = lv_obj_get_y(btn) < 100;
    // One viewport minus one row, so context carries across the page turn.
    const int32_t step = lv_obj_get_height(container) - 62;
    lv_obj_scroll_by_bounded(container, 0, up ? step : -step, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// BLUETOOTH pairing. PacketAPI never forwards the RANDOM_PIN passkey to
// device-ui, so the button owns the code instead: first press generates six
// digits from the hardware CSPRNG, stores them as FIXED_PIN and enables
// Bluetooth; every later press shows the stored code.
// ---------------------------------------------------------------------------
void TFTView_540x960::showPairing(void)
{
    char buf[16];
    // Bluetooth is off at every boot, so a live enabled flag means this button
    // was already pressed since the last reset - show the same code rather
    // than rekeying a phone mid-pairing.
    const bool armed = db.config.bluetooth.enabled &&
                       db.config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN &&
                       db.config.bluetooth.fixed_pin >= 100000 && db.config.bluetooth.fixed_pin <= 999999 &&
                       db.config.bluetooth.fixed_pin != 123456; // never the firmware default

    if (!armed) {
        uint32_t r = 0;
        if (!HardwareRNG::fill(reinterpret_cast<uint8_t *>(&r), sizeof(r), true)) {
            messageAlert("NO ENTROPY - TRY AGAIN", true);
            return;
        }
        db.config.bluetooth.enabled = true;
        db.config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
        db.config.bluetooth.fixed_pin = 100000 + (r % 900000);

        meshtastic_AdminMessage m;
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        m.set_config.which_payload_variant = meshtastic_Config_bluetooth_tag;
        m.set_config.payload_variant.bluetooth = db.config.bluetooth;
        controller->sendAdminMessage(m, ownNode);
    }

    snprintf(buf, sizeof(buf), "%06u", (unsigned)db.config.bluetooth.fixed_pin);
    lv_textarea_set_text(objects.pairing_code, buf);
    // BLE starts the moment the set_config lands, so pairing is live as soon
    // as this overlay appears; Bluetooth is never restored at boot.
    lv_label_set_text(objects.pairing_hint, "PAIR YOUR PHONE NOW USING THIS CODE. "
                                            "BLUETOOTH TURNS OFF AT THE NEXT RESET.");
    lv_obj_remove_flag(objects.pairing_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(objects.pairing_panel);
    modalOpen = true;
    updateBackButton();
}

void TFTView_540x960::ui_event_PairingClose(lv_event_t *)
{
    if (!gui)
        return;
    lv_obj_add_flag(objects.pairing_panel, LV_OBJ_FLAG_HIDDEN);
    gui->modalOpen = false;
    gui->updateBackButton();
}

// ---------------------------------------------------------------------------
// SAVE SETTINGS AND REBOOT - the one and only publisher
// ---------------------------------------------------------------------------
void TFTView_540x960::saveAndReboot(void)
{
    // Refuse to publish an unseeded shadow: until configCompleted() the db is
    // all zeros, and publishing it would zero the lora and network config.
    // The settings screen is reachable before the stream completes, so the
    // guard lives here.
    if (!configSeeded) {
        messageAlert("CONFIG NOT LOADED YET - TRY AGAIN", true);
        return;
    }
    // One batch at a time: a re-tap mid-batch would reset the ack counter
    // and double-send every group.
    if (savePending)
        return;

    meshtastic_AdminMessage m;
    pendingSaveAcks = 0;
    savePending = false;

    // Owner. set_owner carries the whole User; id and hw_model are the radio's
    // to fill in, but a stale id would overwrite the real one.
    if (dirtyOwner) {
        snprintf(db.user.id, sizeof(db.user.id), "!%08x", (unsigned)ownNode);
        db.user.has_is_unmessagable = true;
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        m.set_owner = db.user;
        sendAdminTracked(m);
    }

    // LoRa: double-guarded on dirtyLora AND has_lora - a zeroed lora payload
    // carries tx_enabled=false and hop_limit=0.
    if (dirtyLora && db.config.has_lora) {
        if (pendingRegionIdx >= 0)
            db.config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)kRegionValues[pendingRegionIdx];
        // Only when a preset was actually chosen: a radio running custom modem
        // parameters carries use_preset=false, so the preset is never written
        // implicitly.
        if (pendingPresetIdx >= 0) {
            db.config.lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)kPresetValues[pendingPresetIdx];
            db.config.lora.use_preset = true;
        }
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        m.set_config.which_payload_variant = meshtastic_Config_lora_tag;
        m.set_config.payload_variant.lora = db.config.lora;
        sendAdminTracked(m);
    }

    // Network: only if the user touched the credentials - republishing an
    // untouched shadow wipes stored ones. wifi_enabled is deliberately NOT
    // derived here; the home screen's WiFi icon is the on/off control.
    if (dirtyNetwork) {
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        m.set_config.which_payload_variant = meshtastic_Config_network_tag;
        m.set_config.payload_variant.network = db.config.network;
        sendAdminTracked(m);
    }

    // Display: the screensaver timeout, in seconds.
    if (dirtyDisplay) {
        if (pendingTimeoutIdx >= 0)
            db.config.display.screen_on_secs = kTimeoutSecs[pendingTimeoutIdx];
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        m.set_config.which_payload_variant = meshtastic_Config_display_tag;
        m.set_config.payload_variant.display = db.config.display;
        sendAdminTracked(m);
    }

    // Brightness, timeout and language persist as DeviceUIConfig on the radio
    // (store_ui_config); brightness also takes effect immediately.
    if (dirtyUiConfig) {
        if (pendingBrightnessIdx >= 0)
            db.uiConfig.screen_brightness = kBrightnessLevels[pendingBrightnessIdx];
        if (pendingTimeoutIdx >= 0)
            db.uiConfig.screen_timeout = (uint16_t)kTimeoutSecs[pendingTimeoutIdx];
        if (pendingLanguageIdx >= 0)
            db.uiConfig.language = (meshtastic_Language)kLanguageValues[pendingLanguageIdx];
        if (displaydriver) {
            displaydriver->setBrightness(db.uiConfig.screen_brightness);
            displaydriver->setScreenTimeout(db.uiConfig.screen_timeout);
        }
        memset(&m, 0, sizeof(m));
        m.which_payload_variant = meshtastic_AdminMessage_store_ui_config_tag;
        m.store_ui_config = db.uiConfig;
        sendAdminTracked(m);
    }

    // Reboot only once the radio has acknowledged every message in the batch
    // - onSaveAck() sends it. An empty batch (nothing dirty) waits for nothing.
    if (pendingSaveAcks == 0) {
        controller->requestReboot(5, ownNode);
        messageAlert("SAVED - REBOOTING", true);
        return;
    }
    savePending = true;
    messageAlert("SAVING...", true);
}

bool TFTView_540x960::sendAdminTracked(meshtastic_AdminMessage &m)
{
    // The request id becomes the packet id; the radio's ACK carries it back
    // in decoded.request_id. EVERY terminal event counts the ack down: found
    // (normal), timeout (60 s sweep, lost ack), removed (external cleanup).
    const uint32_t id = requests.addRequest(
        ownNode, ResponseHandler::RemoteConfigRequest, nullptr,
        [](const ResponseHandler::Request &, ResponseHandler::EventType ev, int32_t) {
            if (gui && (ev == ResponseHandler::found || ev == ResponseHandler::timeout ||
                        ev == ResponseHandler::removed))
                gui->onSaveAck();
        });
    const bool ok = controller->sendAdminMessage(m, ownNode, id);
    if (ok)
        pendingSaveAcks++;
    else
        requests.removeRequest(id);
    return ok;
}

void TFTView_540x960::onSaveAck(void)
{
    if (!savePending && pendingSaveAcks == 0)
        return;
    if (pendingSaveAcks && --pendingSaveAcks == 0 && savePending) {
        savePending = false;
        messageAlert("SAVED - REBOOTING", true);
        controller->requestReboot(5, ownNode);
    }
}

// ---------------------------------------------------------------------------
// transient alert - a strip over the context bar, cleared by the next nav
// ---------------------------------------------------------------------------
void TFTView_540x960::messageAlert(const char *text, bool show)
{
    if (show && text) {
        lv_obj_remove_flag(objects.context_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(objects.context_label, text);
    }
    // Invalidate the gate either way, so the next real context write is not
    // swallowed as "unchanged".
    cacheContext[0] = '\0';
}

// ---------------------------------------------------------------------------
// identity and metadata
// ---------------------------------------------------------------------------
void TFTView_540x960::setMyInfo(uint32_t nodeNum)
{
    MeshtasticView::setMyInfo(nodeNum);
    ownNode = nodeNum;
}

void TFTView_540x960::setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi,
                                        bool has_eth, bool can_shutdown)
{
    MeshtasticView::setDeviceMetaData(hw_model, version, has_bluetooth, has_wifi, has_eth, can_shutdown);
    hasBluetooth = has_bluetooth;
    hasWifi = has_wifi;
    hasEthernet = has_eth;
    canShutdown = can_shutdown;
    if (version) {
        strncpy(firmwareVersion, version, sizeof(firmwareVersion) - 1);
        firmwareVersion[sizeof(firmwareVersion) - 1] = '\0';
    }
}

bool TFTView_540x960::setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig)
{
    db.uiConfig = uiconfig;
    db.silent = !uiconfig.alert_enabled;

    // Push the stored values into the driver: initBacklight() leaves the
    // frontlight at 0 and the driver never learns the timeout otherwise.
    if (displaydriver) {
        displaydriver->setBrightness(db.uiConfig.screen_brightness);
        // 0 means the radio has never stored one - keep the driver's own
        // default rather than reading it as "no timeout".
        if (db.uiConfig.screen_timeout)
            displaydriver->setScreenTimeout(db.uiConfig.screen_timeout);
    }
    return true;
}

void TFTView_540x960::updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status)
{
    db.connectionStatus = status;
    refreshHome();
}

void TFTView_540x960::updateTime(uint32_t time)
{
    curtime = (time_t)time;
    char buf[12];
    const uint32_t secs = time % 86400;
    if (db.config.display.use_12h_clock) {
        uint32_t h = (secs / 3600) % 24;
        const char *ap = h < 12 ? "am" : "pm";
        h = h % 12;
        if (h == 0)
            h = 12;
        snprintf(buf, sizeof(buf), "%u:%02u%s", (unsigned)h, (unsigned)((secs / 60) % 60), ap);
    } else {
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)((secs / 3600) % 24), (unsigned)((secs / 60) % 60));
    }
    setLabelIfChanged(objects.clock_label, cacheClock, sizeof(cacheClock), buf);
}

void TFTView_540x960::configCompleted(void)
{
    MeshtasticView::configCompleted();
    // The shadow now mirrors the radio. Until this point SAVE must not publish:
    // an unseeded db would overwrite the real config with zeros.
    configSeeded = true;
    refreshChannels();
    refreshHome();

    // A fresh device cannot transmit and has no identity keypair until a region
    // is chosen, so it opens on the setup gate rather than on a home screen
    // whose every row would read "not set".
    if (needsSetup()) {
        showSetup();
        return;
    }
    // The settings screen mirrors the shadow db, which is complete exactly now.
    // Seed the pending indexes from what the radio reported, so the pickers
    // open on the device's real state rather than on defaults.
    for (uint8_t i = 0; i < kRegionCount; i++)
        if (kRegionValues[i] == (int32_t)db.config.lora.region) {
            pendingRegionIdx = i;
            break;
        }
    // Only when the radio is actually running a preset: use_preset=false
    // means hand-set modem parameters no table entry describes.
    if (db.config.lora.use_preset) {
        for (uint8_t i = 0; i < kPresetCount; i++)
            if (kPresetValues[i] == (int32_t)db.config.lora.modem_preset) {
                pendingPresetIdx = i;
                break;
            }
    }
    for (uint8_t i = 0; i < kTimeoutCount; i++)
        if (kTimeoutSecs[i] == db.config.display.screen_on_secs) {
            pendingTimeoutIdx = i;
            break;
        }
    // Exact match only: re-mapping an unknown level would let SAVE write the
    // re-mapping back.
    for (uint8_t i = 0; i < kBrightnessCount; i++)
        if (kBrightnessLevels[i] == db.uiConfig.screen_brightness) {
            pendingBrightnessIdx = i;
            break;
        }
    for (uint8_t i = 0; i < kLanguageCount; i++)
        if (kLanguageValues[i] == (int32_t)db.uiConfig.language) {
            pendingLanguageIdx = i;
            break;
        }
    refreshSettings();
}

// ---------------------------------------------------------------------------
// shadow fill. Every group is stored, including the ones upstream discards,
// because the editors read each field's current value back out of the shadow.
// ---------------------------------------------------------------------------
void TFTView_540x960::updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg)
{
    db.config.device = cfg;
    db.config.has_device = true;
    db.has_config = true;
}
void TFTView_540x960::updatePositionConfig(const meshtastic_Config_PositionConfig &cfg)
{
    db.config.position = cfg;
    db.config.has_position = true;
    refreshHome();
}
void TFTView_540x960::updatePowerConfig(const meshtastic_Config_PowerConfig &cfg)
{
    db.config.power = cfg;
    db.config.has_power = true;
}
void TFTView_540x960::updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg)
{
    db.config.network = cfg;
    db.config.has_network = true;
    refreshHome();
}
void TFTView_540x960::updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg)
{
    db.config.display = cfg;
    db.config.has_display = true;
    // displaymode must stay COLOR in this build: main.cpp skips BaseUI on the
    // strength of it, so any other value boots a device with no UI at all.
    if (cfg.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        db.config.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
        controller->sendConfig(meshtastic_Config_DisplayConfig{db.config.display}, ownNode);
    }
}
void TFTView_540x960::updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg)
{
    db.config.lora = cfg;
    db.config.has_lora = true;
    refreshHome();
}
void TFTView_540x960::updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id)
{
    (void)id;
    db.config.bluetooth = cfg;
    db.config.has_bluetooth = true;
}
void TFTView_540x960::updateSecurityConfig(const meshtastic_Config_SecurityConfig &cfg)
{
    db.config.security = cfg;
    db.config.has_security = true;
    refreshHome();
}
void TFTView_540x960::updateSessionKeyConfig(const meshtastic_Config_SessionkeyConfig &cfg)
{
    // SessionkeyConfig carries no data; LocalConfig has no member for it.
    (void)cfg;
}

void TFTView_540x960::updateMQTTModule(const meshtastic_ModuleConfig_MQTTConfig &cfg)
{
    db.module_config.mqtt = cfg;
    db.module_config.has_mqtt = true;
    db.has_module_config = true;
    refreshHome();
}
void TFTView_540x960::updateSerialModule(const meshtastic_ModuleConfig_SerialConfig &cfg)
{
    db.module_config.serial = cfg;
    db.module_config.has_serial = true;
}
void TFTView_540x960::updateExtNotificationModule(const meshtastic_ModuleConfig_ExternalNotificationConfig &cfg)
{
    db.module_config.external_notification = cfg;
    db.module_config.has_external_notification = true;
}
void TFTView_540x960::updateStoreForwardModule(const meshtastic_ModuleConfig_StoreForwardConfig &cfg)
{
    db.module_config.store_forward = cfg;
    db.module_config.has_store_forward = true;
}
void TFTView_540x960::updateRangeTestModule(const meshtastic_ModuleConfig_RangeTestConfig &cfg)
{
    db.module_config.range_test = cfg;
    db.module_config.has_range_test = true;
}
void TFTView_540x960::updateTelemetryModule(const meshtastic_ModuleConfig_TelemetryConfig &cfg)
{
    db.module_config.telemetry = cfg;
    db.module_config.has_telemetry = true;
}
void TFTView_540x960::updateCannedMessageModule(const meshtastic_ModuleConfig_CannedMessageConfig &cfg)
{
    db.module_config.canned_message = cfg;
    db.module_config.has_canned_message = true;
}
void TFTView_540x960::updateAudioModule(const meshtastic_ModuleConfig_AudioConfig &cfg)
{
    db.module_config.audio = cfg;
    db.module_config.has_audio = true;
}
void TFTView_540x960::updateRemoteHardwareModule(const meshtastic_ModuleConfig_RemoteHardwareConfig &cfg)
{
    db.module_config.remote_hardware = cfg;
    db.module_config.has_remote_hardware = true;
}
void TFTView_540x960::updateNeighborInfoModule(const meshtastic_ModuleConfig_NeighborInfoConfig &cfg)
{
    db.module_config.neighbor_info = cfg;
    db.module_config.has_neighbor_info = true;
}
void TFTView_540x960::updateAmbientLightingModule(const meshtastic_ModuleConfig_AmbientLightingConfig &cfg)
{
    db.module_config.ambient_lighting = cfg;
    db.module_config.has_ambient_lighting = true;
}
void TFTView_540x960::updateDetectionSensorModule(const meshtastic_ModuleConfig_DetectionSensorConfig &cfg)
{
    db.module_config.detection_sensor = cfg;
    db.module_config.has_detection_sensor = true;
}
void TFTView_540x960::updatePaxCounterModule(const meshtastic_ModuleConfig_PaxcounterConfig &cfg)
{
    db.module_config.paxcounter = cfg;
    db.module_config.has_paxcounter = true;
}
void TFTView_540x960::updateStatusMessageModule(const meshtastic_ModuleConfig_StatusMessageConfig &cfg)
{
    db.module_config.statusmessage = cfg;
    db.module_config.has_statusmessage = true;
}
void TFTView_540x960::updateTrafficManagementModule(const meshtastic_ModuleConfig_TrafficManagementConfig &cfg)
{
    db.module_config.traffic_management = cfg;
    db.module_config.has_traffic_management = true;
}
void TFTView_540x960::updateTakModule(const meshtastic_ModuleConfig_TAKConfig &cfg)
{
    db.module_config.tak = cfg;
    db.module_config.has_tak = true;
}
void TFTView_540x960::updateMeshBeaconModule(const meshtastic_ModuleConfig_MeshBeaconConfig &cfg)
{
    db.module_config.mesh_beacon = cfg;
    db.module_config.has_mesh_beacon = true;
}

// ---------------------------------------------------------------------------
// notifications
// ---------------------------------------------------------------------------
void TFTView_540x960::showMessagePopup(const char *from)
{
    // The banner takes the wordmark band - the one strip with no data on it -
    // for five seconds or until the next navigation. The caller has already
    // applied the SILENT/mute gate.
    lv_label_set_text(objects.notify_line2, from ? from : "?");
    lv_obj_remove_flag(objects.notify_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(objects.notify_panel);
    if (notifyTimer)
        lv_timer_delete(notifyTimer);
    notifyTimer = lv_timer_create(notifyTimerCb, 5000, nullptr);
    lv_timer_set_repeat_count(notifyTimer, 1);
}

void TFTView_540x960::notifyResync(bool show)
{
    messageAlert(show ? "Resyncing" : nullptr, show);
}

void TFTView_540x960::notifyReboot(bool show)
{
    if (show)
        messageAlert("REBOOTING", true);
    rebootPending = false;
}

void TFTView_540x960::notifyShutdown(void)
{
    // Factory order: the screen is cleared and refreshed FIRST - scrub, then
    // a white flash-erase - and only then the power-off image is brought up
    // as the final frame the panel holds. An uncleaned GL16 pass would leave
    // the previous screen ghosted through the image's white field.
#if UI_HAS_POWEROFF_IMAGE
    lv_image_set_src(objects.power_off_image, &img_poweroff_screen);
    lv_obj_remove_flag(objects.power_off_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(objects.power_off_panel);
#else
    // No image supplied: the cleared field with the final alert text is what
    // the panel holds.
    messageAlert("SHUTTING DOWN", true);
#endif
    EINKDriverBase::requestShutdownSplash();
    // Synchronous, one flush: scrub -> white flash -> image. The driver makes
    // this the terminal update; the firmware's shutdown path then parks the
    // panel and deep sleeps.
    lv_refr_now(nullptr);
}

void TFTView_540x960::blankScreen(bool enable)
{
    // Screen timeout shows the power-off screen, exactly like shutdown, so
    // the device never idles holding a stale copy of the last screen. The
    // driver calls this BEFORE suspending refreshes and drives one clean
    // pass; on wake the panel is hidden again and the untouched screen
    // underneath repaints.
    if (enable) {
        // An open keyboard must not survive the idle period, and its FAST
        // mode must not leak into the wake repaint.
        EINKDriverBase::enableUnlimitedFast(false);
        if (objects.power_off_image)
            lv_image_set_src(objects.power_off_image, &img_poweroff_screen);
        if (objects.power_off_panel) {
            lv_obj_remove_flag(objects.power_off_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(objects.power_off_panel);
        }
    } else {
        if (objects.power_off_panel)
            lv_obj_add_flag(objects.power_off_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// lvgl callbacks
// ---------------------------------------------------------------------------
void TFTView_540x960::ui_events_init(void)
{
    lv_obj_add_event_cb(objects.back_button, ui_event_Back, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.home_button, ui_event_HomeButton, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.nodes_button, ui_event_NodesButton, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.groups_button, ui_event_GroupsButton, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.messages_button, ui_event_MessagesButton, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.map_button, ui_event_MapButton, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.settings_button, ui_event_SettingsButton, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(objects.editor_ok_button, ui_event_EditorOk, LV_EVENT_CLICKED, nullptr);
    // The editor keyboard's own checkmark is the commit and its close key the
    // discard - the on-screen OK/CANCEL bar is never shown.
    lv_obj_add_event_cb(objects.editor_keyboard, ui_event_EditorOk, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(objects.editor_keyboard, ui_event_EditorCancel, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(objects.editor_cancel_button, ui_event_EditorCancel, LV_EVENT_CLICKED, nullptr);

    // --- settings surface -------------------------------------------------
    // Value-picker rows carry their SettingsRow id; text rows carry their
    // TextTarget. All of them stage - saveAndReboot() is the only publisher.
    lv_obj_add_event_cb(objects.settings_region_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_REGION);
    lv_obj_add_event_cb(objects.settings_preset_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_PRESET);
    lv_obj_add_event_cb(objects.settings_timeout_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_TIMEOUT);
    lv_obj_add_event_cb(objects.settings_brightness_button, ui_event_SettingsRow, LV_EVENT_CLICKED,
                        (void *)SROW_BRIGHTNESS);
    lv_obj_add_event_cb(objects.settings_language_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_LANGUAGE);
    lv_obj_add_event_cb(objects.settings_reboot_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_REBOOT);
    lv_obj_add_event_cb(objects.settings_shutdown_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_SHUTDOWN);
    lv_obj_add_event_cb(objects.settings_bluetooth_button, ui_event_SettingsRow, LV_EVENT_CLICKED,
                        (void *)SROW_BLUETOOTH);
    lv_obj_add_event_cb(objects.settings_save_button, ui_event_SettingsRow, LV_EVENT_CLICKED, (void *)SROW_SAVE);
    lv_obj_add_event_cb(objects.pairing_close_button, ui_event_PairingClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(objects.settings_long_name, ui_event_SettingsText, LV_EVENT_CLICKED, (void *)TEXT_OWNER_LONG);
    lv_obj_add_event_cb(objects.settings_short_name, ui_event_SettingsText, LV_EVENT_CLICKED, (void *)TEXT_OWNER_SHORT);
    lv_obj_add_event_cb(objects.settings_wifi_ssid, ui_event_SettingsText, LV_EVENT_CLICKED, (void *)TEXT_WIFI_SSID);
    lv_obj_add_event_cb(objects.settings_wifi_psk, ui_event_SettingsText, LV_EVENT_CLICKED, (void *)TEXT_WIFI_PSK);

    // Option rows: one callback each, carrying (kind<<8)|row. Generated child
    // counts are verified against the table lengths instead of trusted; a
    // mismatched picker gets no callbacks - it renders, but cannot commit.
    static const uint8_t kOptCounts[OPT_COUNT] = {kRegionCount, kPresetCount, kTimeoutCount,
                                                  kBrightnessCount, kLanguageCount};
    for (uint8_t k = 0; k < OPT_COUNT; k++) {
        lv_obj_t *list = optList(k);
        if (!list)
            continue;
        const uint32_t n = lv_obj_get_child_count(list);
        if (n != kOptCounts[k]) {
            ILOG_ERROR("opt list %u: %u generated rows vs %u table entries - "
                       "generation drift, picker disabled",
                       (unsigned)k, (unsigned)n, (unsigned)kOptCounts[k]);
            continue;
        }
        for (uint32_t i = 0; i < n; i++)
            lv_obj_add_event_cb(lv_obj_get_child(list, (int32_t)i), ui_event_OptRow, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(((uint16_t)k << 8) | i));
    }

    // Floating page arrows: user_data is the container they move.
    struct ArrowWiring {
        lv_obj_t *up, *down, *container;
    };
    const ArrowWiring arrows[] = {
        {objects.settings_scroll_up_button, objects.settings_scroll_down_button, objects.settings_scroll},
        {objects.opt_region_scroll_up_button, objects.opt_region_scroll_down_button, objects.opt_region_list},
        {objects.opt_preset_scroll_up_button, objects.opt_preset_scroll_down_button, objects.opt_preset_list},
        {objects.opt_timeout_scroll_up_button, objects.opt_timeout_scroll_down_button, objects.opt_timeout_list},
        {objects.opt_brightness_scroll_up_button, objects.opt_brightness_scroll_down_button,
         objects.opt_brightness_list},
        {objects.opt_language_scroll_up_button, objects.opt_language_scroll_down_button, objects.opt_language_list},
    };
    for (const ArrowWiring &a : arrows) {
        if (a.up && a.container)
            lv_obj_add_event_cb(a.up, ui_event_ScrollArrow, LV_EVENT_CLICKED, a.container);
        if (a.down && a.container)
            lv_obj_add_event_cb(a.down, ui_event_ScrollArrow, LV_EVENT_CLICKED, a.container);
    }
    // Conversation arrows: all threads share one scroll host, so the wiring
    // is static; visibility is dynamic - shown only while the thread overflows.
    if (objects.messages_scroll_up_button)
        lv_obj_add_event_cb(objects.messages_scroll_up_button, ui_event_ScrollArrow, LV_EVENT_CLICKED,
                            objects.message_container);
    if (objects.messages_scroll_down_button)
        lv_obj_add_event_cb(objects.messages_scroll_down_button, ui_event_ScrollArrow, LV_EVENT_CLICKED,
                            objects.message_container);

    for (uint8_t r = 0; r < kHomeRowCount; r++) {
        lv_obj_t *b = homeRowButton(r);
        if (!b)
            continue;
        lv_obj_add_event_cb(b, ui_event_HomeRow, LV_EVENT_CLICKED, (void *)(uintptr_t)r);
        // Long press is how a row reaches its own toggle - the notification and
        // MQTT rows in particular are controls, not just readouts.
        lv_obj_add_event_cb(b, ui_event_HomeRow, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)r);
    }

    for (uint8_t c = 0; c < kChannelCount; c++) {
        lv_obj_t *b = channelButton(c);
        if (b)
            lv_obj_add_event_cb(b, ui_event_ChannelButton, LV_EVENT_CLICKED, (void *)(uintptr_t)c);
    }
    for (uint8_t a = 0; a < kNodeActionCount; a++) {
        lv_obj_t *b = nodeActionButton(a);
        if (b)
            lv_obj_add_event_cb(b, ui_event_NodeAction, LV_EVENT_CLICKED, (void *)(uintptr_t)a);
    }

    // LV_EVENT_READY is the keyboard's own send key.
    lv_obj_add_event_cb(objects.message_keyboard, ui_event_MessageSend, LV_EVENT_READY, nullptr);
    // Upstream's own limit for the message composer.
    lv_textarea_set_max_length(objects.message_input_area, 220);

    for (uint8_t r = 0; r < kChannelEditRows; r++) {
        lv_obj_t *b = channelEditButton(r);
        if (b)
            lv_obj_add_event_cb(b, ui_event_ChannelEditRow, LV_EVENT_CLICKED, (void *)(uintptr_t)r);
    }
    lv_obj_add_event_cb(objects.channel_apply_button, ui_event_ChannelApply, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.channel_cancel_button, ui_event_ChannelCancel, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(objects.setup_region_button, ui_event_SetupRegion, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.setup_name_button, ui_event_SetupName, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.setup_done_button, ui_event_SetupDone, LV_EVENT_CLICKED, nullptr);

    // A channel row: tap opens the conversation, long press opens its settings.
    for (uint8_t c = 0; c < kChannelCount; c++) {
        lv_obj_t *b = channelButton(c);
        if (b)
            lv_obj_add_event_cb(b, ui_event_ChannelButton, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)c);
    }

    lv_obj_t *mapControls[8] = {objects.map_gps_lock_button, objects.map_zoom_in_button, objects.map_zoom_out_button,
                                objects.map_up_button,       objects.map_left_button,    objects.map_home_button,
                                objects.map_right_button,    objects.map_down_button};
    for (uint8_t i = 0; i < 8; i++)
        lv_obj_add_event_cb(mapControls[i], ui_event_MapControl, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
}

void TFTView_540x960::ui_event_Back(lv_event_t *)
{
    if (!gui)
        return;
    // BACK cancels text entry, same as the keyboard's close key. Other modals
    // (pairing) own their own dismissal.
    if (gui->modalOpen) {
        if (objects.editor_entry_panel && !lv_obj_has_flag(objects.editor_entry_panel, LV_OBJ_FLAG_HIDDEN))
            ui_event_EditorCancel(nullptr);
        return;
    }
    gui->navPop();
}
void TFTView_540x960::ui_event_HomeButton(lv_event_t *)
{
    if (gui)
        gui->navReset(objects.home_button, objects.home_panel, nullptr);
}
void TFTView_540x960::ui_event_NodesButton(lv_event_t *)
{
    if (!gui)
        return;
    gui->navReset(objects.nodes_button, objects.nodes_panel, "NODES");
    gui->updateAllLastHeard();
}
void TFTView_540x960::ui_event_GroupsButton(lv_event_t *)
{
    if (!gui)
        return;
    gui->refreshChannels();
    gui->navReset(objects.groups_button, objects.groups_panel, "GROUP CHANNELS");
}
void TFTView_540x960::ui_event_MessagesButton(lv_event_t *)
{
    if (!gui)
        return;
    gui->navReset(objects.messages_button, objects.chats_panel, "CHATS");
    gui->unreadMessages = 0;
    gui->refreshHome();
}
void TFTView_540x960::ui_event_MapButton(lv_event_t *)
{
    if (!gui)
        return;
    gui->navReset(objects.map_button, objects.map_panel, nullptr);
    gui->redrawMap();
}
void TFTView_540x960::ui_event_SettingsButton(lv_event_t *)
{
    if (gui) {
        gui->refreshSettings();
        gui->navReset(objects.settings_button, objects.settings_panel, "SETTINGS");
    }
}
void TFTView_540x960::ui_event_EditorOk(lv_event_t *)
{
    if (!gui)
        return;
    gui->commitTextEntry(); // stages into db/pending - never publishes
    gui->closeTextOverlay();
}
void TFTView_540x960::ui_event_EditorCancel(lv_event_t *)
{
    if (!gui)
        return;
    gui->textTarget = TEXT_NONE; // discard
    gui->closeTextOverlay();
}

void TFTView_540x960::closeTextOverlay(void)
{
    lv_obj_add_flag(objects.editor_entry_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.editor_action_panel, LV_OBJ_FLAG_HIDDEN);
    // The fast-refresh window existed for keyboard echo; the panel goes back to
    // its normal rate the moment typing ends.
    EINKDriverBase::enableUnlimitedFast(false);
    modalOpen = false;
    updateBackButton();
    refreshSettings();
}

void TFTView_540x960::ui_event_HomeRow(lv_event_t *e)
{
    if (!gui)
        return;
    const uint8_t row = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    const bool longPress = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;

    switch (row) {
    case 0: // messages
        ui_event_MessagesButton(e);
        break;
    case 1: // nodes
        ui_event_NodesButton(e);
        break;
    case 3: // frequency -> region/preset live on the settings screen
        gui->showSettings();
        break;
    case 5: // notification: this row is itself the control
        if (longPress) {
            gui->db.silent = !gui->db.silent;
            gui->db.uiConfig.alert_enabled = !gui->db.silent;
            gui->controller->storeUIConfig(gui->db.uiConfig);
            gui->refreshHome();
        }
        break;
    case 6: // position
        ui_event_MapButton(e);
        break;
    case 7:
        // This row IS the WiFi switch: tap toggles, long press opens settings.
        // Applying a network change always reboots the radio (AdminModule),
        // so the alert says so.
        if (longPress) {
            gui->showSettings();
        } else {
            gui->db.config.network.wifi_enabled = !gui->db.config.network.wifi_enabled;
            meshtastic_AdminMessage m;
            memset(&m, 0, sizeof(m));
            m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            m.set_config.which_payload_variant = meshtastic_Config_network_tag;
            m.set_config.payload_variant.network = gui->db.config.network;
            gui->controller->sendAdminMessage(m, gui->ownNode);
            gui->refreshHome();
            gui->messageAlert(gui->db.config.network.wifi_enabled ? "WIFI ON - REBOOTING" : "WIFI OFF - REBOOTING",
                              true);
        }
        break;
    case 8:
        // MQTT starts disabled on a fresh device; long press toggles it.
        if (longPress) {
            gui->db.module_config.mqtt.enabled = !gui->db.module_config.mqtt.enabled;
            meshtastic_AdminMessage m;
            memset(&m, 0, sizeof(m));
            m.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
            m.set_module_config.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
            m.set_module_config.payload_variant.mqtt = gui->db.module_config.mqtt;
            gui->controller->sendAdminMessage(m, gui->ownNode);
            gui->refreshHome();
        }
        // No tap destination: the long-press enable is the whole control.
        break;
    case 10: // public key: a readout. Security detail is phone/desktop surface.
        break;
    default:
        break;
    }
}

void TFTView_540x960::ui_event_NodeButton(lv_event_t *e)
{
    if (!gui)
        return;
    const uint32_t num = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
        gui->showNodeActions(num);
    else
        gui->showMessages(num, false);
}

void TFTView_540x960::ui_event_NodeAction(lv_event_t *e)
{
    if (gui)
        gui->applyNodeAction((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

void TFTView_540x960::ui_event_ChannelButton(lv_event_t *e)
{
    if (!gui)
        return;
    const uint8_t c = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
        gui->showChannelEditor(c);
    else
        gui->openChannelEditor(c);
}

void TFTView_540x960::ui_event_ChatButton(lv_event_t *e)
{
    if (!gui)
        return;
    const uint32_t v = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (v & 0x80000000u)
        gui->showMessages(v & 0x7fffffffu, true);
    else
        gui->showMessages(v, false);
}

void TFTView_540x960::ui_event_MessageSend(lv_event_t *)
{
    if (!gui)
        return;
    gui->handleAddMessage(lv_textarea_get_text(objects.message_input_area));
    // Typing is over: leave fast mode and pay off the ghosting it accumulated.
    EINKDriverBase::enableUnlimitedFast(false);
    EINKDriverBase::requestNeatRefresh();
}

void TFTView_540x960::ui_event_MapControl(lv_event_t *e)
{
    if (!gui)
        return;
    // A pan step is a fixed fraction of the viewport, so one press moves the
    // same visible distance at every zoom level.
    const float step = gui->mapMetresPerPx * 120.0f;
    switch ((uint8_t)(uintptr_t)lv_event_get_user_data(e)) {
    case 0: // GPS lock: recentre on our own fix and resume following it
        gui->mapFollowOwn = true;
        gui->mapPanE = gui->mapPanN = 0.0f;
        break;
    case 1: // zoom in
        gui->mapMetresPerPx *= 0.5f;
        break;
    case 2: // zoom out
        gui->mapMetresPerPx *= 2.0f;
        break;
    case 3:
        gui->mapPanN += step;
        gui->mapFollowOwn = false;
        break;
    case 4:
        gui->mapPanE -= step;
        gui->mapFollowOwn = false;
        break;
    case 5: // home: fit every peer that has a position
        gui->mapFitAll();
        return;
    case 6:
        gui->mapPanE += step;
        gui->mapFollowOwn = false;
        break;
    case 7:
        gui->mapPanN -= step;
        gui->mapFollowOwn = false;
        break;
    default:
        return;
    }
    gui->redrawMap();
}

/**
 * Destination screens for TFTView_540x960: nodes, groups, messages, map.
 *
 * Everything here is driven by pushed state: device-ui is a protocol client
 * reaching the radio over PacketAPI's loopback. Peer data arrives through
 * addNode/updateMetrics/updatePosition and is cached in the widgets' own
 * user_data, config arrives through the update*Config callbacks into `db`,
 * and writes leave as AdminMessages.
 */

// Beyond a couple of hundred rows the flex layout pass costs more than the
// refresh does. Upstream caps the same way, for the same reason.
static constexpr uint32_t MAX_NUM_NODES_VIEW = 200;

// Metres per degree of latitude. Longitude is scaled by cos(lat) at the origin,
// which is exact over a mesh-sized area and avoids a full geodesic.
static constexpr float METRES_PER_DEG = 111320.0f;

static constexpr int32_t NODE_ROW_H = 96;

// ---------------------------------------------------------------------------
// node list
// ---------------------------------------------------------------------------

void TFTView_540x960::setNodeImage(uint32_t nodeNum, eRole role, bool unmessagable, lv_obj_t *img)
{
    (void)nodeNum;
    const lv_image_dsc_t *src;
    if (unmessagable) {
        src = &img_unmessagable_image;
    } else {
        switch (role) {
        case router:
        case router_client:
        case repeater:
        case router_late:
            src = &img_node_router_image;
            break;
        case sensor:
        case tracker:
            src = &img_node_sensor_image;
            break;
        case unknown:
            src = &img_user_question_image;
            break;
        default:
            src = &img_node_client_image;
            break;
        }
    }
    lv_image_set_src(img, src);
}

lv_obj_t *TFTView_540x960::newNodePanel(uint32_t nodeNum, uint8_t ch)
{
    // Child order is fixed - see TFTView_540x960::NodeChild. Every update path
    // indexes into it, so nothing may be inserted in the middle.
    lv_obj_t *p = lv_obj_create(objects.nodes_container);
    lv_obj_set_user_data(p, (void *)(uintptr_t)ch);
    lv_obj_set_size(p, LV_PCT(100), NODE_ROW_H);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    // [0] role icon
    lv_obj_t *img = lv_image_create(p);
    lv_obj_set_pos(img, 12, 24);
    lv_obj_set_size(img, 48, 48);

    // [1] the row-sized button that owns the click; transparent, no press
    // visual.
    lv_obj_t *btn = lv_button_create(p);
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_size(btn, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Every label gets an explicit face and pure black; 20pt name / 16pt
    // detail fits the 96px row.
    // [2] long name
    lv_obj_t *ln = lv_label_create(p);
    lv_obj_set_pos(ln, 72, 8);
    lv_obj_set_size(ln, 300, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ln, &ui_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ln, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_user_data(ln, (void *)(uintptr_t)nodeNum);

    // [3] short name
    lv_obj_t *sn = lv_label_create(p);
    lv_obj_set_pos(sn, 72, 42);
    lv_obj_set_size(sn, 180, LV_SIZE_CONTENT);
    lv_label_set_long_mode(sn, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(sn, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    // [4] battery
    lv_obj_t *bat = lv_label_create(p);
    lv_obj_set_pos(bat, -12, 42);
    lv_obj_set_size(bat, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(bat, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_text_align(bat, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(bat, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bat, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(bat, "");

    // [5] last heard
    lv_obj_t *lh = lv_label_create(p);
    lv_obj_set_pos(lh, -12, 8);
    lv_obj_set_size(lh, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(lh, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_text_align(lh, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lh, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lh, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(lh, "");

    // [6] signal, hops, or utilisation - whichever last arrived
    lv_obj_t *sig = lv_label_create(p);
    lv_obj_set_pos(sig, 260, 42);
    lv_obj_set_size(sig, 180, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(sig, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sig, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(sig, "");

    // [7] position - user_data carries latitude, so distances can be recomputed
    //     when our own fix moves without keeping a second table
    lv_obj_t *pos = lv_label_create(p);
    lv_obj_set_pos(pos, 72, 68);
    lv_obj_set_size(pos, 280, LV_SIZE_CONTENT);
    lv_label_set_long_mode(pos, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(pos, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(pos, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(pos, "");
    lv_obj_set_user_data(pos, 0);

    // [8] distance - user_data carries longitude
    lv_obj_t *dst = lv_label_create(p);
    lv_obj_set_pos(dst, -12, 68);
    lv_obj_set_size(dst, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(dst, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_text_font(dst, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(dst, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(dst, "");
    lv_obj_set_user_data(dst, 0);

    // Extended rows stay hidden until data actually arrives, so a fresh mesh
    // does not render three blank lines per peer.
    lv_obj_add_flag(bat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sig, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dst, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(btn, ui_event_NodeButton, LV_EVENT_CLICKED, (void *)(uintptr_t)nodeNum);
    lv_obj_add_event_cb(btn, ui_event_NodeButton, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)nodeNum);
    return p;
}

void TFTView_540x960::sortNodeByLastHeard(lv_obj_t *panel, uint32_t lastHeard)
{
    // Most recently heard first. Insertion over the container children, which is
    // cheap because a freshly heard peer normally belongs at or near the top.
    if (!lastHeard)
        return;
    lv_obj_t *cont = objects.nodes_container;
    const uint32_t n = lv_obj_get_child_count(cont);
    uint32_t target = 0;
    for (; target < n; target++) {
        lv_obj_t *other = lv_obj_get_child(cont, target);
        if (other == panel)
            continue;
        const uint32_t olh = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_obj_get_child(other, node_lh_idx));
        if (lastHeard >= olh)
            break;
    }
    lv_obj_move_to_index(panel, (int32_t)target);
}

void TFTView_540x960::purgeOldestNode(uint32_t keep)
{
    // Drop the least recently heard peer that is neither the one being inserted
    // nor ourselves. The ordering above already puts it last.
    lv_obj_t *cont = objects.nodes_container;
    for (int32_t i = (int32_t)lv_obj_get_child_count(cont) - 1; i >= 0; i--) {
        lv_obj_t *p = lv_obj_get_child(cont, i);
        const uint32_t num = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_obj_get_child(p, node_lbl_idx));
        if (num == keep || num == ownNode)
            continue;
        nodes.erase(num);
        removeFromMap(num);
        lv_obj_delete(p);
        if (nodeCount)
            nodeCount--;
        return;
    }
}

void TFTView_540x960::addNode(uint32_t nodeNum, uint8_t ch, const char *userShort, const char *userLong,
                              uint32_t lastHeard, eRole role, bool hasKey, bool unmessagable)
{
    ILOG_DEBUG("addNode(%u): num=0x%08x, lastseen=%u, name=%s(%s), role=%d", (unsigned)nodeCount, nodeNum,
               (unsigned)lastHeard, userLong ? userLong : "", userShort ? userShort : "", (int)role);

    while (nodeCount >= MAX_NUM_NODES_VIEW)
        purgeOldestNode(nodeNum);

    lv_obj_t *p = newNodePanel(nodeNum, ch);
    nodes[nodeNum] = p;
    nodeCount++;

    lv_obj_t *img = lv_obj_get_child(p, node_img_idx);
    setNodeImage(nodeNum, role, unmessagable, img);
    lv_obj_set_user_data(img, (void *)(uintptr_t)(unmessagable ? (uint32_t)eRole::unmessagable : (uint32_t)role));

    char buf[48];
    lv_obj_t *ln = lv_obj_get_child(p, node_lbl_idx);
    if (userLong && *userLong) {
        lv_label_set_text(ln, userLong);
    } else {
        snprintf(buf, sizeof(buf), "!%08x", (unsigned)nodeNum);
        lv_label_set_text(ln, buf);
    }

    // A short name made only of glyphs this font cannot draw is useless, so fall
    // back to the low half of the node number, as upstream does.
    lv_obj_t *sn = lv_obj_get_child(p, node_lbs_idx);
    const bool printable =
        userShort && lv_text_get_width(userShort, strlen(userShort), &ui_font_montserrat_20, 0) > 4;
    // The tick marks a peer whose public key we hold. LV_SYMBOL_OK, not
    // U+2713: Montserrat has no check-mark glyph.
    if (printable)
        snprintf(buf, sizeof(buf), "%s%s", userShort, hasKey ? "  " LV_SYMBOL_OK : "");
    else
        snprintf(buf, sizeof(buf), "%04x%s", (unsigned)(nodeNum & 0xffff), hasKey ? "  " LV_SYMBOL_OK : "");
    lv_label_set_text(sn, buf);
    lv_obj_set_user_data(sn, (void *)(uintptr_t)(hasKey ? 1u : 0u));

    // Upstream refreshes an open DM chat's label when the node's names
    // change: "<short>: <long>".
    auto ct = chats.find(nodeNum);
    if (ct != chats.end()) {
        char cbuf[64];
        snprintf(cbuf, sizeof(cbuf), "%s: %s", lv_label_get_text(sn), lv_label_get_text(ln));
        // child 0 is the row icon, child 1 the label - see addChat()
        lv_obj_t *clab = lv_obj_get_child(ct->second, 1);
        if (clab)
            lv_label_set_text(clab, cbuf);
    }

    lv_obj_t *lh = lv_obj_get_child(p, node_lh_idx);
    if (lastHeard) {
        // Clamp a value from a peer whose clock runs ahead of ours.
        lastHeard = std::min((uint32_t)curtime, lastHeard);
        if (lastHeardToString(lastHeard, buf))
            nodesOnline++;
        lv_label_set_text(lh, buf);
    }
    lv_obj_set_user_data(lh, (void *)(uintptr_t)lastHeard);

    sortNodeByLastHeard(p, lastHeard);
    updateNodesStatus();
}

void TFTView_540x960::updateNode(uint32_t nodeNum, uint8_t ch, const meshtastic_User &cfg)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    lv_obj_t *p = it->second;
    lv_obj_set_user_data(p, (void *)(uintptr_t)ch);

    const bool hasKey = cfg.public_key.size == 32;
    const bool unmessagable = cfg.has_is_unmessagable && cfg.is_unmessagable;
    lv_obj_t *img = lv_obj_get_child(p, node_img_idx);
    setNodeImage(nodeNum, (eRole)cfg.role, unmessagable, img);
    lv_obj_set_user_data(img, (void *)(uintptr_t)(unmessagable ? (uint32_t)eRole::unmessagable : (uint32_t)cfg.role));

    char buf[48];
    if (cfg.long_name[0])
        lv_label_set_text(lv_obj_get_child(p, node_lbl_idx), cfg.long_name);
    snprintf(buf, sizeof(buf), "%s%s", cfg.short_name[0] ? cfg.short_name : "----", hasKey ? "  " LV_SYMBOL_OK : "");
    lv_obj_t *sn = lv_obj_get_child(p, node_lbs_idx);
    lv_label_set_text(sn, buf);
    lv_obj_set_user_data(sn, (void *)(uintptr_t)(hasKey ? 1u : 0u));
}

void TFTView_540x960::addOrUpdateNode(uint32_t nodeNum, uint8_t ch, uint32_t lastHeard, const meshtastic_User &cfg)
{
    // Our own NodeInfo is the only place the radio reports our own User; the
    // settings screen needs it so SAVE round-trips the current names.
    if (nodeNum == ownNode) {
        db.user = cfg;
        refreshSettings();
    }

    if (nodes.find(nodeNum) == nodes.end()) {
        addNode(nodeNum, ch, cfg.short_name, cfg.long_name, lastHeard, (eRole)cfg.role, cfg.public_key.size == 32,
                cfg.has_is_unmessagable && cfg.is_unmessagable);
    } else {
        updateNode(nodeNum, ch, cfg);
        if (lastHeard)
            updateLastHeard(nodeNum);
    }
}

void TFTView_540x960::removeNode(uint32_t nodeNum)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    lv_obj_delete(it->second);
    nodes.erase(it);
    if (nodeCount)
        nodeCount--;
    removeFromMap(nodeNum);
    updateNodesStatus();
}

void TFTView_540x960::updateNodesStatus(void)
{
    // Runs from packet arrival; the context header belongs to whatever
    // screen is showing, so update only while the nodes panel is visible.
    if (lv_obj_has_flag(objects.nodes_panel, LV_OBJ_FLAG_HIDDEN))
        return;
    char buf[48];
    snprintf(buf, sizeof(buf), "%d of %d nodes online", (int)nodesOnline, (int)nodeCount); // upstream updateNodesStatus
    setLabelIfChanged(objects.context_label, cacheContext, sizeof(cacheContext), buf);
}

void TFTView_540x960::updateAllLastHeard(void)
{
    // Called once a minute: "5 min" only ages at minute granularity.
    char buf[24];
    uint32_t online = 0;
    for (auto &kv : nodes) {
        lv_obj_t *lh = lv_obj_get_child(kv.second, node_lh_idx);
        const uint32_t t = (uint32_t)(uintptr_t)lv_obj_get_user_data(lh);
        if (!t)
            continue;
        if (lastHeardToString(t, buf))
            online++;
        if (strcmp(lv_label_get_text(lh), buf) != 0)
            lv_label_set_text(lh, buf);
    }
    if (online != nodesOnline) {
        nodesOnline = online;
        updateNodesStatus();
    }
}

void TFTView_540x960::updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chU, float airU)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    char buf[48];

    if (nodeNum == ownNode) {
        chUtil = chU;
        airUtil = airU;
        batteryLevel = bat_level;
        batteryVoltage = voltage;

        // 101 is the firmware's "powered, no battery" sentinel, not 101 percent.
        if (bat_level == 101)
            snprintf(buf, sizeof(buf), "USB");
        else
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)std::min(bat_level, (uint32_t)100));
        setLabelIfChanged(objects.battery_percent_label, cacheBattery, sizeof(cacheBattery), buf);

        const lv_image_dsc_t *icon = &img_battery_full_image;
        if (bat_level == 101)
            icon = &img_battery_bolt_image;
        else if (bat_level < 10)
            icon = &img_battery_empty_warn_image;
        else if (bat_level < 40)
            icon = &img_battery_low_image;
        else if (bat_level < 80)
            icon = &img_battery_mid_image;
        lv_image_set_src(objects.battery_image, icon);

        snprintf(buf, sizeof(buf), "Util %.1f%%  Air %.1f%%", (double)chU, (double)airU);
        lv_obj_t *sig = lv_obj_get_child(it->second, node_sig_idx);
        lv_label_set_text(sig, buf);
        lv_obj_remove_flag(sig, LV_OBJ_FLAG_HIDDEN);
    }

    if (bat_level != 0 || voltage != 0.0f) {
        if (bat_level == 101)
            snprintf(buf, sizeof(buf), "USB %.2fV", (double)voltage);
        else
            snprintf(buf, sizeof(buf), "%u%% %.2fV", (unsigned)std::min(bat_level, (uint32_t)100), (double)voltage);
        lv_obj_t *bat = lv_obj_get_child(it->second, node_bat_idx);
        lv_label_set_text(bat, buf);
        lv_obj_remove_flag(bat, LV_OBJ_FLAG_HIDDEN);
    }
}

void TFTView_540x960::updateEnvironmentMetrics(uint32_t nodeNum, const meshtastic_EnvironmentMetrics &metrics)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    char buf[64];
    const bool metric = db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
    if (metric) {
        if ((int)metrics.relative_humidity > 0)
            snprintf(buf, sizeof(buf), "%2.1f°C %d%% %3.1fhPa", (double)metrics.temperature,
                     (int)metrics.relative_humidity, (double)metrics.barometric_pressure);
        else
            snprintf(buf, sizeof(buf), "%2.1f°C %3.1fhPa", (double)metrics.temperature,
                     (double)metrics.barometric_pressure);
    } else {
        const double f = metrics.temperature * 9.0 / 5.0 + 32.0;
        if ((int)metrics.relative_humidity > 0)
            snprintf(buf, sizeof(buf), "%2.1f°F %d%% %3.1finHg", f, (int)metrics.relative_humidity,
                     (double)metrics.barometric_pressure / 33.86);
        else
            snprintf(buf, sizeof(buf), "%2.1f°F %3.1finHg", f, (double)metrics.barometric_pressure / 33.86);
    }
    lv_obj_t *sig = lv_obj_get_child(it->second, node_sig_idx);
    lv_label_set_text(sig, buf);
    lv_obj_remove_flag(sig, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_540x960::updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr)
{
    // Only values that pass upstream's nonzero gate - packets truly received
    // over RF - reach the home signal row; loopback packets carry rssi 0.
    if (nodeNum != ownNode && (rssi != 0 || snr != 0.0f)) {
        lastRssi = rssi;
        lastSnr = snr;
    }

    auto it = nodes.find(nodeNum);
    if (it == nodes.end() || nodeNum == ownNode)
        return;

    char buf[32];
    snprintf(buf, sizeof(buf), "rssi: %d snr: %.1f", (int)rssi, (double)snr); // upstream updateSignalStrength
    lv_obj_t *sig = lv_obj_get_child(it->second, node_sig_idx);
    lv_label_set_text(sig, buf);
    lv_obj_remove_flag(sig, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_540x960::updateHopsAway(uint32_t nodeNum, uint8_t hopsAway)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "hops: %d", (int)hopsAway); // upstream updateHopsAway
    lv_obj_t *sig = lv_obj_get_child(it->second, node_sig_idx);
    lv_label_set_text(sig, buf);
    lv_obj_remove_flag(sig, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_540x960::updateDistance(uint32_t nodeNum, int32_t lat, int32_t lon)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end() || !hasPosition || (!lat && !lon))
        return;

    const float ownLat = myLatitude * 1e-7f;
    const float lonScale = cosf(ownLat * (float)M_PI / 180.0f) * METRES_PER_DEG;
    const float e = (lon * 1e-7f - myLongitude * 1e-7f) * lonScale;
    const float n = (lat * 1e-7f - ownLat) * METRES_PER_DEG;
    const float metres = sqrtf(e * e + n * n);

    char buf[24];
    if (db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC) {
        if (metres < 1000.0f)
            snprintf(buf, sizeof(buf), "%d m ", (int)metres); // upstream updateDistance
        else
            snprintf(buf, sizeof(buf), "%.1f km ", (double)metres / 1000.0); // upstream updateDistance
    } else {
        const float feet = metres * 3.28084f;
        if (feet < 5280.0f)
            snprintf(buf, sizeof(buf), "%d ft ", (int)feet); // upstream updateDistance
        else
            snprintf(buf, sizeof(buf), "%.1f mi ", (double)feet / 5280.0); // upstream updateDistance
    }
    lv_obj_t *dst = lv_obj_get_child(it->second, node_dst_idx);
    lv_label_set_text(dst, buf);
    lv_obj_remove_flag(dst, LV_OBJ_FLAG_HIDDEN);
}

// Timezone from the GPS fix; there is no menu option for it. US region gets
// the four CONUS zones plus Alaska/Hawaii by longitude band with standard US
// DST rules; other regions get the whole-hour solar offset, no DST. Written
// once, only while device.tz_def is empty - a user/phone-set value is never
// overwritten.
static const char *tzFromFix(int32_t lat_i, int32_t lon_i, int region)
{
    const float lat = lat_i * 1e-7f, lon = lon_i * 1e-7f;
    if (region == meshtastic_Config_LoRaConfig_RegionCode_US) {
        if (lat < 23.0f && lon < -154.0f)
            return "HST10"; // Hawaii, no DST
        if (lat > 51.0f && lon < -129.0f)
            return "AKST9AKDT,M3.2.0,M11.1.0"; // Alaska
        if (lon >= -85.0f)
            return "EST5EDT,M3.2.0,M11.1.0";
        if (lon >= -100.5f)
            return "CST6CDT,M3.2.0,M11.1.0";
        if (lon >= -114.0f)
            return "MST7MDT,M3.2.0,M11.1.0";
        return "PST8PDT,M3.2.0,M11.1.0";
    }
    // Solar approximation: UTC offset = round(lon / 15). POSIX offsets are
    // west-positive, so the sign flips into the string.
    static char buf[12];
    int off = (int)((lon + (lon >= 0 ? 7.5f : -7.5f)) / 15.0f);
    snprintf(buf, sizeof(buf), "UTC%+d", -off);
    return buf;
}

void TFTView_540x960::updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats,
                                     uint32_t precision)
{
    (void)precision;
    // Own fix + no timezone stored yet -> derive and publish it, once.
    if (nodeNum == ownNode && configSeeded && db.config.has_device && db.config.device.tzdef[0] == '\0' &&
        (lat != 0 || lon != 0)) {
        static bool tzSent = false;
        if (!tzSent) {
            tzSent = true;
            snprintf(db.config.device.tzdef, sizeof(db.config.device.tzdef), "%s",
                     tzFromFix(lat, lon, db.config.lora.region));
            meshtastic_AdminMessage m = meshtastic_AdminMessage_init_default;
            m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            m.set_config.which_payload_variant = meshtastic_Config_device_tag;
            m.set_config.payload_variant.device = db.config.device;
            controller->sendAdminMessage(m, ownNode);
            ILOG_INFO("timezone from fix: %s", db.config.device.tzdef);
        }
    }
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;

    lv_obj_t *pos = lv_obj_get_child(it->second, node_pos_idx);
    lv_obj_t *dst = lv_obj_get_child(it->second, node_dst_idx);
    lv_obj_set_user_data(pos, (void *)(intptr_t)lat);
    lv_obj_set_user_data(dst, (void *)(intptr_t)lon);

    int32_t altU = (abs(alt) < 10000) ? alt : 0;
    const bool metric = db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
    const char *units = metric ? "m" : "ft";
    if (!metric)
        altU = (int32_t)(altU * 3.28084f);

    // Node rows show decimal degrees plus MSL altitude, as upstream; DMS
    // belongs only to the own node's home row.
    char buf[96];
    if (lat != 0 || lon != 0) {
        snprintf(buf, sizeof(buf), "%.5f %.5f   %d%s MSL", lat * 1e-7, lon * 1e-7, (int)altU, units);
        lv_label_set_text(pos, buf);
        lv_obj_remove_flag(pos, LV_OBJ_FLAG_HIDDEN);
    }

    if (nodeNum == ownNode) {
        // Home row: upstream's own-position DMS format.
        int latSeconds = (int)lround(lat * 1e-7 * 3600);
        const int latDegrees = latSeconds / 3600;
        latSeconds = abs(latSeconds % 3600);
        const int latMinutes = latSeconds / 60;
        latSeconds %= 60;
        const char latLetter = (lat > 0) ? 'N' : 'S';

        int lonSeconds = (int)lround(lon * 1e-7 * 3600);
        const int lonDegrees = lonSeconds / 3600;
        lonSeconds = abs(lonSeconds % 3600);
        const int lonMinutes = lonSeconds / 60;
        lonSeconds %= 60;
        const char lonLetter = (lon > 0) ? 'E' : 'W';
        // Upstream renders this as one label with a line break; this design's
        // home rows are two labels, so the same two lines map onto setRow's
        // l1/l2 - lat + sats above, lon + altitude below.
        char l2buf[48];
        if (sats)
            snprintf(buf, sizeof(buf), "%c%02i° %2i'%02i\"   %u sats", latLetter, abs(latDegrees), latMinutes,
                     latSeconds, (unsigned)sats);
        else
            snprintf(buf, sizeof(buf), "%c%02i° %2i'%02i\"", latLetter, abs(latDegrees), latMinutes, latSeconds);
        snprintf(l2buf, sizeof(l2buf), "%c%02i° %2i'%02i\"   %d%s", lonLetter, abs(lonDegrees), lonMinutes, lonSeconds,
                 (int)altU, units);
        setRow(6, buf, l2buf);

        if (lat != 0 || lon != 0) {
            hasPosition = true;
            myLatitude = lat;
            myLongitude = lon;
            // Our own fix moved, so every peer distance is now stale.
            for (auto &kv : nodes) {
                if (kv.first == ownNode)
                    continue;
                const int32_t nlat = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(kv.second, node_pos_idx));
                const int32_t nlon = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(kv.second, node_dst_idx));
                if (nlat || nlon)
                    updateDistance(kv.first, nlat, nlon);
            }
            if (mapFollowOwn)
                mapMetresPerPx = 0.0f; // re-fit on the next draw
        }
    } else {
        updateDistance(nodeNum, lat, lon);
    }

    addOrUpdateMap(nodeNum, lat, lon);
}

// --- node action sheet -----------------------------------------------------

void TFTView_540x960::showNodeActions(uint32_t nodeNum)
{
    auto it = nodes.find(nodeNum);
    if (it == nodes.end())
        return;
    selectedNode = nodeNum;
    lv_label_set_text(objects.node_action_title, lv_label_get_text(lv_obj_get_child(it->second, node_lbl_idx)));

    // Key verification is only meaningful for a peer we already hold a key for.
    const bool hasKey = (uintptr_t)lv_obj_get_user_data(lv_obj_get_child(it->second, node_lbs_idx)) != 0;
    if (hasKey)
        lv_obj_remove_state(objects.node_action4_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(objects.node_action4_button, LV_STATE_DISABLED);

    lv_obj_remove_flag(objects.node_action_panel, LV_OBJ_FLAG_HIDDEN);
    modalOpen = true;
    updateBackButton();
}

void TFTView_540x960::hideNodeActions(void)
{
    lv_obj_add_flag(objects.node_action_panel, LV_OBJ_FLAG_HIDDEN);
    modalOpen = false;
    updateBackButton();
}

void TFTView_540x960::applyNodeAction(uint8_t action)
{
    const uint32_t n = selectedNode;
    if (!n) {
        hideNodeActions();
        return;
    }
    meshtastic_AdminMessage m;
    memset(&m, 0, sizeof(m));

    switch (action) {
    case 0: // Go to chat
        hideNodeActions();
        showMessages(n, false);
        return;
    case 1: // Favourite
        m.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
        m.set_favorite_node = n;
        break;
    case 2: // Mute - the firmware owns the flag, so this is a toggle, not a set
        m.which_payload_variant = meshtastic_AdminMessage_toggle_muted_node_tag;
        m.toggle_muted_node = n;
        break;
    case 3: // Trace route
        hideNodeActions();
        controller->traceRoute(n, 0, db.config.lora.hop_limit, 0);
        return;
    case 4: // Key verification - starts the out-of-band 4 digit exchange
        m.which_payload_variant = meshtastic_AdminMessage_key_verification_tag;
        m.key_verification.message_type = meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION;
        m.key_verification.remote_nodenum = n;
        break;
    case 5: // Ignore
        m.which_payload_variant = meshtastic_AdminMessage_set_ignored_node_tag;
        m.set_ignored_node = n;
        break;
    default:
        hideNodeActions();
        return;
    }
    controller->sendAdminMessage(m, ownNode);
    hideNodeActions();
}

// ---------------------------------------------------------------------------
// groups / channels
// ---------------------------------------------------------------------------

void TFTView_540x960::updateChannelConfig(const meshtastic_Channel &ch)
{
    // Channel.index is int8_t: -1 is the protocol's "match by name" sentinel,
    // not a slot, and would index out of bounds.
    if (ch.index < 0 || ch.index >= (int8_t)c_max_channels)
        return;
    db.channel[ch.index] = ch;
    refreshChannels();
}

void TFTView_540x960::refreshChannels(void)
{
    char buf[64];
    for (uint8_t i = 0; i < c_max_channels; i++) {
        const meshtastic_Channel &ch = db.channel[i];
        lv_obj_t *name = channelNameLabel(i);
        lv_obj_t *role = channelRoleLabel(i);
        lv_obj_t *icon = channelIcon(i);
        if (!name || !role)
            continue;

        // Formats match stock device-ui.
        if (ch.role == meshtastic_Channel_Role_DISABLED) {
            // A disabled slot shows only its number.
            snprintf(buf, sizeof(buf), "%u", (unsigned)i);
            lv_label_set_text(name, buf);
            lv_label_set_text(role, "");
            if (icon)
                lv_image_set_src(icon, &img_groups_unlock_image);
            continue;
        }

        // Primary is "Channel: <name-or-preset>", secondary "*<name-or-preset>".
        const char *nm =
            ch.settings.name[0] ? ch.settings.name : t5_preset_name(db.config.lora.modem_preset);
        if (ch.role == meshtastic_Channel_Role_PRIMARY)
            snprintf(buf, sizeof(buf), "Channel: %s", nm);
        else
            snprintf(buf, sizeof(buf), "*%s", nm);
        lv_label_set_text(name, buf);
        lv_label_set_text(role, "");

        // Unencrypted (all-zero PSK) shows the open lock, encrypted the
        // closed lock.
        bool unencrypted = (ch.settings.psk.size == 0);
        if (!unencrypted && ch.settings.psk.size <= 16) {
            static const uint8_t zeros[16] = {0};
            if (memcmp(ch.settings.psk.bytes, zeros, ch.settings.psk.size) == 0)
                unencrypted = true;
        }
        if (icon)
            lv_image_set_src(icon, unencrypted ? &img_groups_unlock_image : &img_groups_lock_image);
    }
}

void TFTView_540x960::openChannelEditor(uint8_t chIndex)
{
    // Tapping a channel opens its conversation; the channel's own settings live
    // in the config tree, reached from Settings.
    selectedChannel = chIndex;
    showMessages(chIndex, true);
}

// ---------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------

lv_obj_t *TFTView_540x960::newMessageContainer(uint32_t from, uint32_t to, uint8_t ch)
{
    // One container per conversation: a broadcast is keyed by channel index, a
    // direct message by the peer, whichever end of it we are.
    const uint32_t key = (to == UINT32_MAX) ? (uint32_t)ch : (from == ownNode ? to : from);
    auto it = messages.find(key);
    if (it != messages.end())
        return it->second;

    lv_obj_t *c = lv_obj_create(objects.message_container);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    messages[key] = c;
    return c;
}

lv_obj_t *TFTView_540x960::messageContainerFor(uint32_t from, uint32_t to, uint8_t ch)
{
    return newMessageContainer(from, to, ch);
}

// Conversation page arrows appear only while the thread overflows its
// viewport; re-checked whenever content or the active conversation changes.
static void updateMessagesScrollArrows(void)
{
    lv_obj_t *c = objects.message_container;
    if (!c || !objects.messages_scroll_up_button || !objects.messages_scroll_down_button)
        return;
    lv_obj_update_layout(c);
    const bool over = lv_obj_get_scroll_top(c) > 0 || lv_obj_get_scroll_bottom(c) > 0;
    if (over) {
        lv_obj_remove_flag(objects.messages_scroll_up_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.messages_scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(objects.messages_scroll_up_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.messages_scroll_down_button, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *TFTView_540x960::addMessage(lv_obj_t *container, const char *text, bool outgoing)
{
    if (!container || !text)
        return nullptr;

    // Upstream bubble, structure verbatim: one full-width transparent row,
    // one width-fitted bordered label. Text is 20 pt pure black to match the
    // entry field; 16 px side padding keeps bubbles off the screen edge.
    lv_obj_t *row = lv_obj_create(container);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(row, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(row, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(row, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(row, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lab = lv_label_create(row);
    // Upstream width fit: clamp(text width, cap) + 10 with a floor; caps
    // scale from upstream's 160/200-on-320 to 300/330-on-540.
    const int32_t w = (int32_t)lv_text_get_width(text, (uint32_t)strlen(text), &ui_font_montserrat_20, 0);
    const int32_t cap = outgoing ? 330 : 300;
    lv_obj_set_width(lab, std::max<int32_t>(std::min<int32_t>(w, cap) + 10, 60));
    lv_obj_set_height(lab, LV_SIZE_CONTENT);
    lv_obj_set_align(lab, outgoing ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lab, text);

    // NewMessageStyle / ChatMessageStyle: white incoming with 1 px border,
    // pale outgoing with 2 px, radius 8.
    lv_obj_set_style_bg_color(lab, lv_color_hex(outgoing ? 0xfbfce9 : 0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lab, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(lab, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(lab, outgoing ? 2 : 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(lab, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(lab, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lab, &ui_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);

    // LV_ANIM_OFF: a smooth scroll at a ~1 Hz refresh ceiling is a stutter.
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    updateMessagesScrollArrows();
    return lab;
}

/**
 * Drop whole messages off the front until the conversation is back at
 * MAX_MSGS_PER_CHAT. One child per message now - the upstream row structure -
 * so the trim is plain child arithmetic. Deleting a row deletes its bubble
 * label, whose LV_EVENT_DELETE handler clears any pendingAcks entry.
 */
void TFTView_540x960::trimConversation(lv_obj_t *container)
{
    if (!container)
        return;
    while (lv_obj_get_child_count(container) > MAX_MSGS_PER_CHAT)
        lv_obj_delete(lv_obj_get_child(container, 0));
}

/**
 * Clears a pendingAcks entry when the status line it points at goes away.
 * Registered on the widget itself so it fires however the deletion happens -
 * the trim above, lv_obj_clean(), or teardown.
 */
void TFTView_540x960::ui_event_AckWidgetDeleted(lv_event_t *e)
{
    if (!gui)
        return;
    gui->pendingAcks.erase((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

// Upstream timestamps: live messages carry "HH:MM\n" as the bubble's first
// line, restored ones "yy/mm/dd HH:MM\n"; an unset clock writes nothing.
static uint32_t msgStamp(char *out, uint32_t t, bool restored)
{
    if (t < 946684800UL) // pre-2000 epoch: the clock has never been set
        return 0;
    time_t local = (time_t)t;
    std::tm tmv{};
    localtime_r(&local, &tmv);
    return (uint32_t)strftime(out, 20, restored ? "%y/%m/%d %R\n" : "%R\n", &tmv);
}

void TFTView_540x960::newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime,
                                 bool restore)
{
    if (!msg)
        return;
    lv_obj_t *c = newMessageContainer(from, to, ch);
    const bool outgoing = (from == ownNode);

    // Upstream assembly: a group message opens with the sender's short name
    // (%04x when unknown) and a space; a DM and our own messages carry no
    // sender. Then the stamp line, then the text - nothing else.
    char buf[300];
    int pos = 0;
    if (!outgoing && to == UINT32_MAX) {
        auto it = nodes.find(from);
        if (it != nodes.end()) {
            char shortName[16];
            snprintf(shortName, sizeof(shortName), "%s",
                     lv_label_get_text(lv_obj_get_child(it->second, node_lbs_idx)));
            char *cut = strstr(shortName, "  "); // strip the key-checkmark suffix
            if (cut)
                *cut = '\0';
            pos = snprintf(buf, sizeof(buf), "%s", shortName);
        } else {
            pos = snprintf(buf, sizeof(buf), "%04x", (unsigned)(from & 0xffff));
        }
        buf[pos++] = ' ';
    }
    pos += (int)msgStamp(&buf[pos], msgtime, restore);
    snprintf(&buf[pos], sizeof(buf) - (size_t)pos, "%s", msg);

    addMessage(c, buf, outgoing);
    trimConversation(c);
    addChat(from, to, ch);

    if (!restore && !outgoing) {
        unreadMessages++;
        refreshHome();
        // Upstream's banner gate: alerts enabled AND the channel not muted.
        if (!db.silent && !db.channel[ch].settings.module_settings.is_muted) {
            auto it = nodes.find(from);
            showMessagePopup(it != nodes.end() ? lv_label_get_text(lv_obj_get_child(it->second, node_lbs_idx)) : "?");
        }
    }
}

void TFTView_540x960::restoreMessage(const LogMessage &msg)
{
    // LogMessage is a POD header plus a non-terminated payload, so the text has
    // to be copied out and terminated before it can be treated as a string.
    if (msg.trashFlag || msg._size == 0)
        return;
    char text[messagePayloadSize + 1];
    const uint16_t n = std::min<uint16_t>(msg._size, messagePayloadSize);
    memcpy(text, msg.bytes, n);
    text[n] = '\0';

    uint32_t t = (uint32_t)msg.time;
    newMessage(msg.from, msg.to, msg.ch, text, t, true);
}

void TFTView_540x960::notifyMessagesRestored(void)
{
    messagesRestored = true;
    state = eMessagesRestored;
    updateActiveChats();
}

void TFTView_540x960::addChat(uint32_t from, uint32_t to, uint8_t ch)
{
    const bool broadcast = (to == UINT32_MAX);
    const uint32_t key = broadcast ? (uint32_t)ch : (from == ownNode ? to : from);
    if (chats.find(key) != chats.end())
        return;

    lv_obj_t *b = lv_button_create(objects.chats_container);
    lv_obj_set_size(b, LV_PCT(100), 88);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *icon = lv_image_create(b);
    lv_image_set_src(icon, broadcast ? &img_groups_lock_image : &img_node_client_image);
    lv_obj_set_pos(icon, 12, 22);

    lv_obj_t *lab = lv_label_create(b);
    lv_obj_set_pos(lab, 72, 28);
    lv_obj_set_size(lab, 400, LV_SIZE_CONTENT);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
    // Explicit ink: the theme's button text is white, which would render
    // white-on-white on this forced-white row.
    lv_obj_set_style_text_color(lab, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lab, &ui_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Stock device-ui format: a broadcast chat is "<index>: <channel label>",
    // a DM "<short name>: <long name>", an unknown sender "!<nodenum>".
    char buf[64];
    if (broadcast) {
        lv_obj_t *chName = ch < c_max_channels ? channelNameLabel(ch) : nullptr;
        snprintf(buf, sizeof(buf), "%d: %s", (int)ch, chName ? lv_label_get_text(chName) : "");
    } else {
        auto it = nodes.find(key);
        if (it != nodes.end())
            snprintf(buf, sizeof(buf), "%s: %s", lv_label_get_text(lv_obj_get_child(it->second, node_lbs_idx)),
                     lv_label_get_text(lv_obj_get_child(it->second, node_lbl_idx)));
        else
            snprintf(buf, sizeof(buf), "!%08x", (unsigned)key);
    }
    lv_label_set_text(lab, buf);

    // The high bit distinguishes a channel key from a node number, so one
    // callback serves both kinds; node numbers never set it.
    lv_obj_add_event_cb(b, ui_event_ChatButton, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(broadcast ? (0x80000000u | ch) : key));
    chats[key] = b;
    updateActiveChats();
}

void TFTView_540x960::updateActiveChats(void)
{
    // Same guard as updateNodesStatus: addChat fires on the first message from
    // any peer, and the header is not its to overwrite unless the chats screen
    // owns it.
    if (lv_obj_has_flag(objects.chats_panel, LV_OBJ_FLAG_HIDDEN))
        return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d active chat(s)", (int)chats.size()); // upstream updateActiveChats
    setLabelIfChanged(objects.context_label, cacheContext, sizeof(cacheContext), buf);
}

void TFTView_540x960::showMessages(uint32_t nodeNumOrChannel, bool isChannel)
{
    const uint32_t key = nodeNumOrChannel;
    if (messages.find(key) == messages.end())
        newMessageContainer(isChannel ? ownNode : key, isChannel ? UINT32_MAX : ownNode, (uint8_t)key);

    // Exactly one conversation is visible at a time; the others stay parented
    // but hidden so their scroll position survives.
    for (auto &kv : messages) {
        if (kv.first == key)
            lv_obj_remove_flag(kv.second, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(kv.second, LV_OBJ_FLAG_HIDDEN);
    }
    // The active thread changed; the arrows follow its overflow state.
    updateMessagesScrollArrows();

    if (isChannel) {
        selectedChannel = (uint8_t)key;
        selectedNode = 0;
    } else {
        selectedNode = key;
    }

    char ctx[48];
    if (isChannel) {
        const meshtastic_Channel &c = db.channel[key < c_max_channels ? key : 0];
        snprintf(ctx, sizeof(ctx), "%s", c.settings.name[0] ? c.settings.name : "<DEFAULT>");
    } else {
        auto it = nodes.find(key);
        if (it != nodes.end())
            snprintf(ctx, sizeof(ctx), "%s", lv_label_get_text(lv_obj_get_child(it->second, node_lbl_idx)));
        else
            snprintf(ctx, sizeof(ctx), "!%08x", (unsigned)key);
    }
    navPush(objects.messages_button, objects.messages_panel, ctx);

    lv_keyboard_set_textarea(objects.message_keyboard, objects.message_input_area);
    // Re-run the anchor at open time: creation-time align_to ran before the
    // first layout pass.
    layoutKeyboard(objects.message_keyboard, objects.message_input_area, false);
    // Typing cannot be gated to 1 Hz or the keyboard is unusable, so the driver
    // is put into unlimited-fast mode for as long as the thread is open.
    EINKDriverBase::enableUnlimitedFast(true);
}

void TFTView_540x960::handleAddMessage(const char *msg)
{
    if (!msg || !*msg)
        return;

    const bool broadcast = (selectedNode == 0);
    const uint32_t to = broadcast ? UINT32_MAX : selectedNode;
    const uint8_t ch = broadcast ? selectedChannel : 0;
    uint32_t now = (uint32_t)curtime;

    // usePkc only when the peer's key is known. Without it the packet falls back
    // to channel encryption anyway, and the 12 bytes of PKC overhead would be
    // spent for nothing.
    bool usePkc = false;
    if (!broadcast) {
        auto it = nodes.find(selectedNode);
        if (it != nodes.end())
            usePkc = (uintptr_t)lv_obj_get_user_data(lv_obj_get_child(it->second, node_lbs_idx)) != 0;
    }

    // Register before sending: the routing reply can come back before the call
    // returns on a loopback transport.
    const uint32_t key = broadcast ? (uint32_t)ch : to;
    const uint32_t requestId = requests.addRequest(key, ResponseHandler::TextMessageRequest,
                                                   (void *)(uintptr_t)key);

    controller->sendTextMessage(to, ch, db.config.lora.hop_limit, now, requestId, usePkc, msg);
    // restore=false: a live send gets the live "HH:MM" stamp; the unread and
    // banner block is skipped anyway because the message is outgoing.
    newMessage(ownNode, to, ch, msg, now, false);

    // The bubble newMessage just appended is the label inside the last row of
    // the conversation; a routing reply later recolors its border.
    lv_obj_t *c = newMessageContainer(ownNode, to, ch);
    const uint32_t n = lv_obj_get_child_count(c);
    if (n) {
        lv_obj_t *bubble = lv_obj_get_child(lv_obj_get_child(c, (int32_t)n - 1), 0);
        if (bubble) {
            pendingAcks[requestId] = bubble;
            // The map holds a raw widget pointer and the trim can delete that
            // widget, so the widget owns the removal of its own entry.
            lv_obj_add_event_cb(bubble, ui_event_AckWidgetDeleted, LV_EVENT_DELETE, (void *)(uintptr_t)requestId);
        }
    }

    lv_textarea_set_text(objects.message_input_area, "");
}

// A broadcast has nobody to acknowledge it, so it is marked sent and left
// alone. A direct message carries a routing reply, and the difference between
// "gone" and "delivered" is the whole point of showing it.
void TFTView_540x960::markMessageStatus(uint32_t requestId, bool ack, bool err)
{
    auto it = pendingAcks.find(requestId);
    if (it == pendingAcks.end())
        return;
    lv_obj_t *bubble = it->second;
    pendingAcks.erase(it);
    if (!bubble)
        return;

    // Upstream carries delivery state in the bubble border: yellow heard,
    // blue-green acked, red failed; on 16-gray glass, distinct shades.
    const uint32_t colr = err ? 0xff5555 : (ack ? 0x05f6cb : 0xdbd251);
    lv_obj_set_style_border_color(bubble, lv_color_hex(colr), LV_PART_MAIN | LV_STATE_DEFAULT);
}

void TFTView_540x960::handleResponse(uint32_t from, uint32_t id, const meshtastic_Routing &routing,
                                     const meshtastic_MeshPacket &p)
{
    (void)from;
    (void)p;
    // SAVE-batch acks first: each admin message went out under its own request
    // id (sendAdminTracked) and the local radio acks it back on that id.
    if (requests.findRequest(id, ResponseHandler::RemoteConfigRequest).id) {
        requests.removeRequest(id, ResponseHandler::RemoteConfigRequest);
        onSaveAck();
        return;
    }

    ResponseHandler::Request req = requests.findRequest(id, ResponseHandler::TextMessageRequest);
    if (!req.id)
        return;

    switch (routing.error_reason) {
    case meshtastic_Routing_Error_NONE:
        // An implicit ack from a relaying node is not proof of delivery; only a
        // reply from the addressee is.
        markMessageStatus(id, true, false);
        requests.removeRequest(id, ResponseHandler::TextMessageRequest);
        break;
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
    case meshtastic_Routing_Error_NO_RESPONSE:
    case meshtastic_Routing_Error_NO_CHANNEL:
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
        markMessageStatus(id, false, true);
        requests.removeRequest(id, ResponseHandler::TextMessageRequest);
        break;
    default:
        break; // still in flight
    }
}

// ---------------------------------------------------------------------------
// map - relative markers, no tiles
// ---------------------------------------------------------------------------

void TFTView_540x960::addOrUpdateMap(uint32_t nodeNum, int32_t lat, int32_t lon)
{
    if (!lat && !lon)
        return;
    if (mapMarkers.find(nodeNum) == mapMarkers.end())
        mapMarkers[nodeNum] = nullptr; // plotted lazily by redrawMap
    if (!lv_obj_has_flag(objects.map_panel, LV_OBJ_FLAG_HIDDEN))
        redrawMap();
}

void TFTView_540x960::removeFromMap(uint32_t nodeNum)
{
    auto it = mapMarkers.find(nodeNum);
    if (it == mapMarkers.end())
        return;
    if (it->second)
        lv_obj_delete(it->second);
    mapMarkers.erase(it);
}

void TFTView_540x960::mapFitAll(void)
{
    mapMetresPerPx = 0.0f; // 0 means auto-fit; redrawMap resolves it
    mapPanE = mapPanN = 0.0f;
    mapFollowOwn = true;
    redrawMap();
}

void TFTView_540x960::redrawMap(void)
{
    lv_obj_t *canvas = objects.map_canvas;
    lv_obj_clean(canvas);
    for (auto &kv : mapMarkers)
        kv.second = nullptr;

    if (!hasPosition) {
        lv_obj_t *l = lv_label_create(canvas);
        lv_label_set_text(l, "No position fix");
        lv_obj_center(l);
        lv_label_set_text(objects.map_scale_label, "");
        lv_label_set_text(objects.map_location_label, "");
        return;
    }

    const float ownLat = myLatitude * 1e-7f;
    const float ownLon = myLongitude * 1e-7f;
    const float lonScale = cosf(ownLat * (float)M_PI / 180.0f) * METRES_PER_DEG;

    const int32_t cw = lv_obj_get_width(canvas);
    const int32_t chh = lv_obj_get_height(canvas);
    // Leave room for the marker labels and for the control cluster on the right.
    const int32_t usable = std::min(cw / 2 - 120, chh / 2 - 60);
    const int32_t cx = cw / 2 - 60, cy = chh / 2;

    // Resolve the scale first: with no explicit zoom, fit the furthest peer.
    float mpp = mapMetresPerPx;
    if (mpp <= 0.0f) {
        float maxR = 1.0f;
        for (auto &kv : mapMarkers) {
            auto nit = nodes.find(kv.first);
            if (nit == nodes.end())
                continue;
            const int32_t la = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(nit->second, node_pos_idx));
            const int32_t lo = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(nit->second, node_dst_idx));
            if (!la && !lo)
                continue;
            const float e = (lo * 1e-7f - ownLon) * lonScale;
            const float n = (la * 1e-7f - ownLat) * METRES_PER_DEG;
            maxR = std::max(maxR, std::max(fabsf(e), fabsf(n)));
        }
        mpp = (usable > 0) ? (maxR / (float)usable) : 1.0f;
        mapMetresPerPx = mpp;
    }
    if (mpp <= 0.0f)
        mpp = 1.0f;

    // own node: filled dot at the pan origin
    lv_obj_t *me = lv_obj_create(canvas);
    lv_obj_set_size(me, 18, 18);
    lv_obj_set_pos(me, cx - 9 - (int32_t)(mapPanE / mpp), cy - 9 + (int32_t)(mapPanN / mpp));
    lv_obj_set_style_radius(me, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(me, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(me, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(me, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (auto &kv : mapMarkers) {
        auto nit = nodes.find(kv.first);
        if (nit == nodes.end())
            continue;
        const int32_t la = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(nit->second, node_pos_idx));
        const int32_t lo = (int32_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(nit->second, node_dst_idx));
        if (!la && !lo)
            continue;

        const float e = (lo * 1e-7f - ownLon) * lonScale - mapPanE;
        const float n = (la * 1e-7f - ownLat) * METRES_PER_DEG - mapPanN;
        const int32_t px = cx + (int32_t)(e / mpp);
        const int32_t py = cy - (int32_t)(n / mpp);
        if (px < -40 || px > cw + 40 || py < -40 || py > chh + 40)
            continue; // off-viewport: do not spend widgets on it

        lv_obj_t *pin = lv_image_create(canvas);
        lv_image_set_src(pin, &img_node_location_pin24_image);
        lv_obj_set_pos(pin, px - 12, py - 24);
        kv.second = pin;

        lv_obj_t *lab = lv_label_create(canvas);
        lv_label_set_text(lab, lv_label_get_text(lv_obj_get_child(nit->second, node_lbs_idx)));
        lv_obj_set_pos(lab, px - 30, py + 4);
    }

    // Scale bar: the largest round distance whose bar still fits in 140px.
    static const int32_t nice[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    int32_t chosen = nice[0];
    for (int32_t d : nice) {
        if ((float)d / mpp <= 140.0f)
            chosen = d;
        else
            break;
    }
    char buf[32];
    if (db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC) {
        if (chosen < 1000)
            snprintf(buf, sizeof(buf), "%d m", (int)chosen);
        else
            snprintf(buf, sizeof(buf), "%d km", (int)(chosen / 1000));
    } else {
        const float mi = chosen * 3.28084f / 5280.0f;
        if (mi < 1.0f)
            snprintf(buf, sizeof(buf), "%d ft", (int)(chosen * 3.28084f));
        else
            snprintf(buf, sizeof(buf), "%.1f mi", (double)mi);
    }
    lv_label_set_text(objects.map_scale_label, buf);

    // The bar is drawn at the resolved length, so the label stays honest.
    lv_obj_t *bar = lv_obj_create(canvas);
    lv_obj_set_size(bar, std::max((int32_t)8, (int32_t)((float)chosen / mpp)), 4);
    lv_obj_set_pos(bar, 20, chh - 34);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    snprintf(buf, sizeof(buf), "%.4f %.4f", (double)ownLat, (double)ownLon);
    lv_label_set_text(objects.map_location_label, buf);
}

// ---------------------------------------------------------------------------
// home rows. Six carry one line, five carry two; the second label is hidden
// on one-line rows so a row's height and baseline match the render.
// ---------------------------------------------------------------------------

// Stock device-ui signalStrength2Percent, kept exactly - including upstream's
// float-SNR truncation through max<int32_t>. This board carries an SX1262,
// so the non-SX127x branch applies.
static int32_t signalStrength2Percent(int32_t rx_rssi, float rx_snr)
{
#if defined(USE_SX127x)
    int p_snr = ((std::max<int32_t>(rx_snr, -19.0f) + 19.0f) / 33.0f) * 100.0f; // range -19..14
    int p_rssi = ((std::max<int32_t>(rx_rssi, -145L) + 145) * 100) / 90;        // range -145..-55
#else
    int p_snr = ((std::max<int32_t>(rx_snr, -18.0f) + 18.0f) / 26.0f) * 100.0f; // range -18..8
    int p_rssi = ((std::max<int32_t>(rx_rssi, -125) + 125) * 100) / 100;        // range -125..-25
#endif
    return std::min<int32_t>((p_snr + p_rssi * 2) / 3, 100);
}

// Home row content is black, always: the state lives in the words ("WIFI
// OFF"), not in a shade. This helper only guarantees the row stays tappable
// (never LV_STATE_DISABLED, which swallows taps) and its content stays black.
void TFTView_540x960::setRowDim(uint8_t row, bool dim)
{
    (void)dim;
    const lv_color_t c = lv_color_hex(0x000000);
    if (homeRowButton(row))
        lv_obj_remove_state(homeRowButton(row), LV_STATE_DISABLED);
    if (homeRowLabel(row))
        lv_obj_set_style_text_color(homeRowLabel(row), c, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (homeRowLabel2(row))
        lv_obj_set_style_text_color(homeRowLabel2(row), c, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (homeRowIcon(row)) {
        lv_obj_set_style_image_recolor(homeRowIcon(row), c, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor_opa(homeRowIcon(row), LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

// One line, or two. The second label exists on every row; a one-line row
// hides it and re-centres the first.
void TFTView_540x960::setRow(uint8_t row, const char *l1, const char *l2)
{
    lv_obj_t *a = homeRowLabel(row);
    lv_obj_t *b = homeRowLabel2(row);
    if (!a)
        return;

    setLabelIfChanged(a, cacheHome[row], sizeof(cacheHome[row]), l1);

    if (!b)
        return;
    if (l2 && *l2) {
        setLabelIfChanged(b, cacheHome2[row], sizeof(cacheHome2[row]), l2);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        cacheHome2[row][0] = '\0';
    }
}

void TFTView_540x960::refreshHome(void)
{
    char l1[80], l2[80];

    // 1 messages
    if (unreadMessages == 0)
        snprintf(l1, sizeof(l1), "NO NEW MESSAGES");
    else
        snprintf(l1, sizeof(l1), unreadMessages == 1 ? "%u NEW MESSAGE" : "%u NEW MESSAGES",
                 (unsigned)unreadMessages);
    setRow(0, l1, nullptr);
    lv_image_set_src(homeRowIcon(0), unreadMessages ? &img_home_mail_unread_button_image
                                                    : &img_home_mail_button_image);

    // 2 nodes
    snprintf(l1, sizeof(l1), "%u OF %u NODES ONLINE", (unsigned)nodesOnline, (unsigned)nodeCount);
    setRow(1, l1, nullptr);

    // 3 uptime: wall-clock since boot, hours not wrapped at 24 (device-ui).
    const uint32_t up = (uint32_t)(lv_tick_get() / 1000);
    // Hours and minutes only: a seconds field would force one full panel
    // refresh per second.
    snprintf(l1, sizeof(l1), "UPTIME: %02u:%02u", (unsigned)(up / 3600),
             (unsigned)((up / 60) % 60));
    setRow(2, l1, nullptr);

    // 4 frequency: the centre frequency the radio operates on, never a
    // channel number. The radio keeps its computed value private, so the view
    // computes it with the same math over the same tables (lora_freq.h).
    if (db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        setRow(3, "REGION UNSET", nullptr);
    } else {
        // Primary channel's raw name; the empty->preset-name rule lives
        // inside t5_lora_freq_mhz.
        const char *primary = "";
        for (uint8_t i = 0; i < c_max_channels; i++)
            if (db.channel[i].role == meshtastic_Channel_Role_PRIMARY) {
                primary = db.channel[i].settings.name;
                break;
            }
        // Stock device-ui format: "LoRa %g MHz" over "[<bandwidth> kHz]";
        // the value comes from the math in lora_freq.h.
        float mhz;
        if (t5_lora_freq_mhz(db.config.lora.region, db.config.lora.modem_preset, db.config.lora.use_preset,
                             db.config.lora.bandwidth, db.config.lora.channel_num, db.config.lora.override_frequency,
                             db.config.lora.frequency_offset, primary, &mhz))
            snprintf(l1, sizeof(l1), "LoRa %g MHz", (double)mhz);
        else
            snprintf(l1, sizeof(l1), "LORA CH %u", (unsigned)db.config.lora.channel_num);
        if (db.config.lora.use_preset)
            snprintf(l2, sizeof(l2), "[%s kHz]", LoRaPresets::getBandwidthString(db.config.lora.modem_preset));
        else
            snprintf(l2, sizeof(l2), "[%d kHz]", (int)db.config.lora.bandwidth);
        setRow(3, l1, l2);
    }

    // 5 signal, stock device-ui format: "SNR: %.1f" / "RSSI: %d (%d%%)",
    // icon chosen by the percent (>80 full, >60 strong, >40 good, >20 fair,
    // >1 weak, else none).
    if (lastSnr == 0.0f && lastRssi == 0) {
        setRow(4, "NO SIGNAL", nullptr);
        lv_image_set_src(homeRowIcon(4), &img_home_no_signal_image);
    } else {
        const uint32_t pct = signalStrength2Percent(lastRssi, lastSnr);
        snprintf(l1, sizeof(l1), "SNR: %.1f", (double)lastSnr);
        snprintf(l2, sizeof(l2), "RSSI: %d   (%d%%)", (int)lastRssi, (int)pct);
        setRow(4, l1, l2);
        const lv_image_dsc_t *ic;
        if (pct > 80)
            ic = &img_home_signal_button_image;
        else if (pct > 60)
            ic = &img_home_strong_signal_image;
        else if (pct > 40)
            ic = &img_home_good_signal_image;
        else if (pct > 20)
            ic = &img_home_fair_signal_image;
        else if (pct > 1)
            ic = &img_home_weak_signal_image;
        else
            ic = &img_home_no_signal_image;
        lv_image_set_src(homeRowIcon(4), ic);
    }

    // 6 notification. No buzzer is fitted on this board, so there is no sound
    // option to advertise: the row is BANNER or SILENT, nothing else.
    setRow(5, db.silent ? "SILENT" : "BANNER", nullptr);
    lv_image_set_src(homeRowIcon(5), db.silent ? &img_groups_bell_slash_image : &img_groups_bell_image);

    // 7 position - written in full by updatePosition once a fix exists.
    if (!hasPosition) {
        setRow(6, db.config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED
                      ? "acquiring fix"
                      : "No GPS present",
               nullptr);
    }

    // 8 WiFi. This row is the on/off control, not a readout: a tap toggles
    // wifi_enabled. It stays live even when off - otherwise there would be
    // no phoneless way to switch WiFi back on.
    const bool wifiUsable = db.config.network.wifi_enabled && db.config.network.wifi_ssid[0];
    if (!db.config.network.wifi_enabled) {
        setRow(7, "WIFI OFF", nullptr);
        lv_image_set_src(homeRowIcon(7), &img_home_wlan_off_image);
    } else if (db.connectionStatus.has_wifi && db.connectionStatus.wifi.status.is_connected) {
        const uint32_t ip = db.connectionStatus.wifi.status.ip_address;
        snprintf(l1, sizeof(l1), "%u.%u.%u.%u", (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                 (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff));
        setRow(7, l1, nullptr);
        lv_image_set_src(homeRowIcon(7), &img_home_wlan_button_image);
    } else {
        setRow(7, db.config.network.wifi_ssid[0] ? "CONNECTING" : "NO SSID SET", nullptr);
        lv_image_set_src(homeRowIcon(7), &img_home_wlan_off_image);
    }
    // Dim the content, never the button: a DISABLED button swallows clicks.
    setRowDim(7, !wifiUsable);

    // 9 MQTT. The whole row dithers when MQTT is off, as in the render - but it
    // stays live, because a long press here is the only way to turn MQTT on
    // without a phone.
    if (db.module_config.mqtt.enabled)
        snprintf(l1, sizeof(l1), "%s", db.module_config.mqtt.root[0] ? db.module_config.mqtt.root : "msh");
    else
        snprintf(l1, sizeof(l1), "MQTT OFF"); // state in words, like the WiFi row
    setRow(8, l1, nullptr);
    setRowDim(8, !db.module_config.mqtt.enabled); // keeps the row tappable

    // 10 SD card. Reduced format: the Arduino SD object exposes
    // cardType/cardSize/usedBytes but not the FAT type.
    if (sdTotalMB) {
        snprintf(l1, sizeof(l1), "SD %u GB", (unsigned)((sdTotalMB + 512) / 1024));
        snprintf(l2, sizeof(l2), "Used: %.2f GB (%u%%)", (double)sdUsedMB / 1024.0,
                 (unsigned)(sdTotalMB ? (sdUsedMB * 100 / sdTotalMB) : 0));
        setRow(9, l1, l2);
    } else {
        setRow(9, "NO SD CARD", nullptr);
    }

    // 11 public key: 44 base64 characters split 22/22, never clipped or
    // ellipsised - a key must be readable exactly.
    if (db.config.security.public_key.size == 32) {
        const std::string b64 = pskToBase64(db.config.security.public_key.bytes,
                                            db.config.security.public_key.size);
        if (b64.size() > 22) {
            snprintf(l1, sizeof(l1), "%.22s", b64.c_str());
            snprintf(l2, sizeof(l2), "%s", b64.c_str() + 22);
            setRow(10, l1, l2);
        } else {
            setRow(10, b64.c_str(), nullptr);
        }
    } else {
        setRow(10, "NO PUBLIC KEY", nullptr);
    }
}

// Bandwidth for the frequency row's second line. use_preset means the modem
// preset picks the bandwidth, so the numeric field is not meaningful then.
const char *TFTView_540x960::loraBandwidthText(void)
{
    static char buf[16];
    if (db.config.lora.use_preset) {
        // Same table the preset picker is built from (ConfigFields.h), so the
        // home row and the settings row can never disagree about a name.
        const int64_t v = db.config.lora.modem_preset;
        for (uint8_t i = 0; i < kPresetCount; i++)
            if (kPresetValues[i] == (int32_t)v)
                return kPresetNames[i];
        return "preset";
    }
    snprintf(buf, sizeof(buf), "%u kHz", (unsigned)db.config.lora.bandwidth);
    return buf;
}

/**
 * Identity, channels, power/reset and first-run setup for TFTView_540x960 -
 * the parts of device configuration that do not live in LocalConfig /
 * LocalModuleConfig: owner (set_owner), the channel slots (set_channel),
 * power/reset actions, and the first-run region gate.
 */

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------
void TFTView_540x960::openTextEntry(TextTarget target, const char *title, const char *current)
{
    textTarget = target;
    entryIsNumeric = false;
    // The standard MUI limits. One shared editor, so the limit is set per
    // target at open; without it the commit would truncate at the proto
    // boundary, splitting multibyte UTF-8 mid-sequence.
    uint32_t maxLen;
    switch (target) {
    case TEXT_OWNER_SHORT:  maxLen = 4;  break;
    case TEXT_OWNER_LONG:   maxLen = 39; break;
    case TEXT_WIFI_SSID:    maxLen = 32; break;
    case TEXT_WIFI_PSK:     maxLen = 64; break;
    case TEXT_CHANNEL_NAME: maxLen = 11; break;
    default:                maxLen = 64; break;
    }
    lv_textarea_set_max_length(objects.editor_entry_text, maxLen);
    lv_label_set_text(objects.editor_entry_title, title);
    lv_label_set_text(objects.editor_entry_range, "");
    lv_textarea_set_text(objects.editor_entry_text, current ? current : "");
    lv_keyboard_set_mode(objects.editor_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(objects.editor_keyboard, objects.editor_entry_text);
    lv_obj_remove_flag(objects.editor_entry_panel, LV_OBJ_FLAG_HIDDEN);
    layoutKeyboard(objects.editor_keyboard, objects.editor_entry_text, false);
    // No action bar on text entry: the keyboard's checkmark commits and BACK
    // cancels; editor_action_panel stays hidden.
    EINKDriverBase::enableUnlimitedFast(true);
    modalOpen = true;
    updateBackButton();
}

bool TFTView_540x960::commitTextEntry(void)
{
    if (textTarget == TEXT_NONE)
        return false;
    const char *t = lv_textarea_get_text(objects.editor_entry_text);
    if (!t)
        t = "";

    switch (textTarget) {
    case TEXT_OWNER_LONG:
        // Staged: the settings batch publishes on SAVE, never a text commit.
        strncpy(db.user.long_name, t, sizeof(db.user.long_name) - 1);
        db.user.long_name[sizeof(db.user.long_name) - 1] = '\0';
        dirtyOwner = true; // SAVE publishes only touched groups
        refreshSettings();
        break;
    case TEXT_OWNER_SHORT:
        strncpy(db.user.short_name, t, sizeof(db.user.short_name) - 1);
        db.user.short_name[sizeof(db.user.short_name) - 1] = '\0';
        dirtyOwner = true; // SAVE publishes only touched groups
        refreshSettings();
        break;
    case TEXT_WIFI_SSID:
        strncpy(db.config.network.wifi_ssid, t, sizeof(db.config.network.wifi_ssid) - 1);
        db.config.network.wifi_ssid[sizeof(db.config.network.wifi_ssid) - 1] = '\0';
        dirtyNetwork = true; // SAVE publishes only touched groups
        refreshSettings();
        break;
    case TEXT_WIFI_PSK:
        strncpy(db.config.network.wifi_psk, t, sizeof(db.config.network.wifi_psk) - 1);
        db.config.network.wifi_psk[sizeof(db.config.network.wifi_psk) - 1] = '\0';
        dirtyNetwork = true; // SAVE publishes only touched groups
        refreshSettings();
        break;
    case TEXT_CHANNEL_NAME:
        // Staged like every other channel row; the editor's APPLY publishes.
        strncpy(chanScratch.settings.name, t, sizeof(chanScratch.settings.name) - 1);
        chanScratch.settings.name[sizeof(chanScratch.settings.name) - 1] = '\0';
        refreshChannelEditor();
        break;
    default:
        break;
    }
    textTarget = TEXT_NONE;
    return true;
}

// ---------------------------------------------------------------------------
// channels
// ---------------------------------------------------------------------------
/**
 * PSK material for a channel slot: 0 clears, 1 selects the public default,
 * 16/32 draw fresh AES-128/256 key material from the hardware CSPRNG.
 */
static bool cfgChannelPsk(meshtastic_ChannelSettings &st, uint8_t size)
{
    if (size == 0 || size == 1) {
        memset(st.psk.bytes, 0, sizeof(st.psk.bytes));
        if (size == 1)
            st.psk.bytes[0] = 1;
        st.psk.size = size;
        return true;
    }
    if (size != 16 && size != 32)
        return false;
    if (!HardwareRNG::fill(st.psk.bytes, size, true))
        return false;
    st.psk.size = size;
    return true;
}

void TFTView_540x960::showChannelEditor(uint8_t chIndex)
{
    if (chIndex >= c_max_channels)
        return;
    selectedChannel = chIndex;
    // Take a working copy: every row edits THIS, not db.channel, so nothing
    // reaches the radio until APPLY.
    chanScratch = db.channel[chIndex];
    chanRekeyArmed = false;
    refreshChannelEditor();
    char ctx[32];
    snprintf(ctx, sizeof(ctx), "CHANNEL %u", (unsigned)chIndex);
    navPush(objects.groups_button, objects.channel_edit_panel, ctx);
}

void TFTView_540x960::refreshChannelEditor(void)
{
    // Renders the STAGED copy - what APPLY would send, not what the radio holds.
    const meshtastic_Channel &ch = chanScratch;
    const meshtastic_ChannelSettings &st = ch.settings;
    char buf[72];

    snprintf(buf, sizeof(buf), "CHANNEL %u", (unsigned)selectedChannel);
    lv_label_set_text(objects.channel_edit_title, buf);

    snprintf(buf, sizeof(buf), "NAME: %s", st.name[0] ? st.name : "<DEFAULT>");
    lv_label_set_text(channelEditLabel(0), buf);

    const char *role = "DISABLED";
    if (ch.role == meshtastic_Channel_Role_PRIMARY)
        role = "PRIMARY";
    else if (ch.role == meshtastic_Channel_Role_SECONDARY)
        role = "SECONDARY";
    snprintf(buf, sizeof(buf), "ROLE: %s", role);
    lv_label_set_text(channelEditLabel(1), buf);

    const char *enc;
    switch (st.psk.size) {
    case 0:
        enc = "EMPTY";
        break;
    case 1:
        enc = "8 BIT";
        break;
    case 16:
        enc = "128 BIT";
        break;
    case 32:
        enc = "256 BIT";
        break;
    default:
        enc = st.psk.size < 32 ? "128 BIT" : "256 BIT";
        break;
    }
    snprintf(buf, sizeof(buf), "ENCRYPTION: %s", enc);
    lv_label_set_text(channelEditLabel(2), buf);

    snprintf(buf, sizeof(buf), "UPLINK TO MQTT: %s", st.uplink_enabled ? "ON" : "OFF");
    lv_label_set_text(channelEditLabel(3), buf);
    snprintf(buf, sizeof(buf), "DOWNLINK FROM MQTT: %s", st.downlink_enabled ? "ON" : "OFF");
    lv_label_set_text(channelEditLabel(4), buf);

    // 0 means "send the exact position"; the firmware treats 32 the same way.
    // Anything between is the number of latitude/longitude bits kept.
    if (!st.has_module_settings || st.module_settings.position_precision == 0)
        snprintf(buf, sizeof(buf), "POSITION PRECISION: EXACT");
    else if (st.module_settings.position_precision >= 32)
        snprintf(buf, sizeof(buf), "POSITION PRECISION: EXACT");
    else
        snprintf(buf, sizeof(buf), "POSITION PRECISION: %u BITS",
                 (unsigned)st.module_settings.position_precision);
    lv_label_set_text(channelEditLabel(5), buf);

    snprintf(buf, sizeof(buf), "MUTED: %s",
             (st.has_module_settings && st.module_settings.is_muted) ? "YES" : "NO");
    lv_label_set_text(channelEditLabel(6), buf);

    // A disabled slot has nothing to configure but its role.
    const bool live = ch.role != meshtastic_Channel_Role_DISABLED;
    for (uint8_t r = 0; r < kChannelEditRows; r++) {
        if (r == 1)
            continue;
        if (live)
            lv_obj_remove_state(channelEditButton(r), LV_STATE_DISABLED);
        else
            lv_obj_add_state(channelEditButton(r), LV_STATE_DISABLED);
    }
}

// The only place entry-overlay geometry changes: keyboard at screen bottom,
// entry field anchored directly above it. When a context carries SAVE/CANCEL,
// keyboard and field move up by the button-band height and the buttons nest
// in the vacated strip underneath.
void TFTView_540x960::layoutKeyboard(lv_obj_t *kb, lv_obj_t *ta, bool withActions)
{
    static constexpr int32_t kActionBandH = 96;
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, withActions ? -kActionBandH : 0);
    lv_obj_update_layout(kb);

    // Every keyboard appearance passes through here: one full NEAT pass, so
    // the key grid never appears half-formed on glass.
    lv_obj_invalidate(lv_screen_active());
    EINKDriverBase::requestNeatRefresh();
    if (ta) {
        lv_obj_set_size(ta, lv_pct(98), 57);
        lv_obj_align_to(ta, kb, LV_ALIGN_OUT_TOP_MID, 0, -5);
    }
    if (objects.editor_action_panel) {
        if (withActions) {
            lv_obj_set_pos(objects.editor_action_panel, 0, 960 - kActionBandH);
            lv_obj_remove_flag(objects.editor_action_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(objects.editor_action_panel);
        } else {
            lv_obj_add_flag(objects.editor_action_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TFTView_540x960::publishChannel(uint8_t chIndex)
{
    if (chIndex >= c_max_channels)
        return;
    chanScratch.index = (int8_t)chIndex;
    chanScratch.has_settings = true;
    db.channel[chIndex] = chanScratch; // shadow tracks what we just sent
    controller->sendConfig(db.channel[chIndex], ownNode);
    refreshChannelEditor();
    refreshChannels();
}

/**
 * APPLY: send the staged channel, then leave the editor. Nothing leaves the
 * device until this button.
 */
void TFTView_540x960::ui_event_ChannelApply(lv_event_t *e)
{
    if (!gui)
        return;
    gui->chanRekeyArmed = false;
    gui->publishChannel(gui->selectedChannel);
    gui->navPop();
}

/**
 * CANCEL: throw the staged copy away; re-entering re-copies from db.channel.
 */
void TFTView_540x960::ui_event_ChannelCancel(lv_event_t *e)
{
    if (!gui)
        return;
    gui->chanRekeyArmed = false;
    gui->navPop();
}

void TFTView_540x960::ui_event_ChannelEditRow(lv_event_t *e)
{
    if (!gui)
        return;
    const uint8_t row = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    // Staged copy only. publishChannel() is reached from APPLY, nowhere else.
    meshtastic_Channel &ch = gui->chanScratch;
    meshtastic_ChannelSettings &st = ch.settings;

    switch (row) {
    case 0:
        gui->openTextEntry(TEXT_CHANNEL_NAME, "CHANNEL NAME", st.name);
        return;

    case 1: {
        // Cycle disabled -> secondary -> disabled. PRIMARY is not part of
        // the cycle: exactly one channel is primary.
        if (gui->selectedChannel == 0) {
            gui->messageAlert("Channel 0 is always primary", true);
            return;
        }
        ch.role = (ch.role == meshtastic_Channel_Role_DISABLED) ? meshtastic_Channel_Role_SECONDARY
                                                                : meshtastic_Channel_Role_DISABLED;
        if (ch.role == meshtastic_Channel_Role_SECONDARY && st.psk.size == 0)
            cfgChannelPsk(st, 1); // a new secondary defaults to the shared PSK
        gui->refreshChannelEditor();
        return;
    }

    case 2:
        // The two steps that draw fresh key material (DEFAULT->128, 128->256)
        // throw away the current key, so they need their own confirm; the
        // steps that destroy no secret go straight through.
        if ((st.psk.size == 1 || st.psk.size == 16) && !gui->chanRekeyArmed) {
            gui->chanRekeyArmed = true;
            gui->messageAlert("TAP AGAIN TO GENERATE A NEW KEY", true);
            return;
        }
        gui->chanRekeyArmed = false;
        // Four strengths, cycled in place.
        switch (st.psk.size) {
        case 0:
            cfgChannelPsk(st, 1);
            break;
        case 1:
            cfgChannelPsk(st, 16);
            break;
        case 16:
            cfgChannelPsk(st, 32);
            break;
        default:
            cfgChannelPsk(st, 0);
            break;
        }
        gui->refreshChannelEditor();
        return;

    case 3:
        st.uplink_enabled = !st.uplink_enabled;
        gui->refreshChannelEditor();
        return;

    case 4:
        st.downlink_enabled = !st.downlink_enabled;
        gui->refreshChannelEditor();
        return;

    case 5: {
        // The precisions the firmware and the phone apps actually offer.
        static const uint8_t kPrecision[] = {32, 16, 14, 13, 12, 11, 10};
        st.has_module_settings = true;
        uint8_t cur = st.module_settings.position_precision;
        uint8_t next = kPrecision[0];
        for (uint8_t i = 0; i < sizeof(kPrecision); i++) {
            if (kPrecision[i] == cur) {
                next = kPrecision[(i + 1) % sizeof(kPrecision)];
                break;
            }
        }
        st.module_settings.position_precision = next;
        gui->refreshChannelEditor();
        return;
    }

    case 6:
        st.has_module_settings = true;
        st.module_settings.is_muted = !st.module_settings.is_muted;
        gui->refreshChannelEditor();
        return;

    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// power and reset
// ---------------------------------------------------------------------------
void TFTView_540x960::clearNodeList(void)
{
    lv_obj_clean(objects.nodes_container);
    nodes.clear();
    mapMarkers.clear();
    nodeCount = 1;
    nodesOnline = 1;
    updateNodesStatus();
}

// ---------------------------------------------------------------------------
// first-run setup
// ---------------------------------------------------------------------------
bool TFTView_540x960::needsSetup(void) const
{
    // Region is the gate. Without it the radio will not transmit and no identity
    // keypair can be generated, so nothing else on the device is usable yet.
    return db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET;
}

void TFTView_540x960::showSetup(void)
{
    refreshSetup();
    navReset(nullptr, objects.setup_panel, nullptr);
}

void TFTView_540x960::refreshSetup(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "REGION: %s",
             gui && gui->pendingRegionIdx >= 0 ? kRegionNames[gui->pendingRegionIdx] : "UNSET");
    lv_label_set_text(objects.setup_region_label, buf);

    snprintf(buf, sizeof(buf), "NAME: %s", db.user.long_name[0] ? db.user.long_name : "<NOT SET>");
    lv_label_set_text(objects.setup_name_label, buf);

    // Gate DONE on the STAGED choice, not the shadow db: the region picker
    // only stages pendingRegionIdx. DONE is what publishes the region, so it
    // enables as soon as one is picked.
    if (gui && gui->pendingRegionIdx >= 0)
        lv_obj_remove_state(objects.setup_done_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(objects.setup_done_button, LV_STATE_DISABLED);
}

void TFTView_540x960::ui_event_SetupRegion(lv_event_t *)
{
    if (gui)
        gui->openOption(OPT_REGION);
}

void TFTView_540x960::ui_event_SetupName(lv_event_t *)
{
    if (gui)
        gui->openTextEntry(TEXT_OWNER_LONG, "LONG NAME", gui->db.user.long_name);
}

void TFTView_540x960::ui_event_SetupDone(lv_event_t *)
{
    if (!gui || gui->pendingRegionIdx < 0)
        return; // nothing chosen yet; the button is dithered in this state

    // DONE is the publisher for the setup gate: no identity keypair can be
    // minted while the region is UNSET, so the region must be on disk before
    // the reboot that generates the keys. Once persisted, needsSetup() is
    // false on every later boot and this screen never shows again.
    gui->db.config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)kRegionValues[gui->pendingRegionIdx];
    gui->db.config.has_lora = true;

    meshtastic_AdminMessage m;
    memset(&m, 0, sizeof(m));
    m.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    m.set_config.which_payload_variant = meshtastic_Config_lora_tag;
    m.set_config.payload_variant.lora = gui->db.config.lora;
    gui->controller->sendAdminMessage(m, gui->ownNode);

    // Owner name too, if one was typed - it is staged in the same shadow and
    // there is no other commit point in this flow.
    if (gui->db.user.long_name[0]) {
        snprintf(gui->db.user.id, sizeof(gui->db.user.id), "!%08x", (unsigned)gui->ownNode);
        gui->db.user.has_is_unmessagable = true;
        gui->controller->sendConfig(gui->db.user, gui->ownNode);
    }

    // The keypair is minted by NodeDB during boot, so setup genuinely ends with
    // a restart rather than merely suggesting one.
    gui->controller->requestReboot(5, gui->ownNode);
    gui->messageAlert("REBOOTING TO GENERATE KEYS", true);
}

#endif
