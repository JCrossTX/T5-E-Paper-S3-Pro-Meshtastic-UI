#pragma once

/**
 * TFTView_540x960 - device-ui view for the LilyGO T5 E-Paper S3 Pro.
 * 540x960 portrait e-paper.
 *
 * A protocol client like every device-ui view: state arrives through the
 * update*() callbacks, writes leave as AdminMessages, and `db` is the shadow
 * copy the editors touch. Differences from the upstream views: the complete
 * configuration surface is exposed on-device (table-driven from the generated
 * ConfigFields.h), and navigation keeps a back stack rather than the flat
 * panel switch.
 *
 * Singleton with static callbacks, because lvgl is C.
 */

#include "ConfigFields.h"
#include "graphics/common/MeshtasticView.h"
#include "meshtastic/clientonly.pb.h"
#include <stdint.h>

class TFTView_540x960 : public MeshtasticView
{
  public:
    void init(IClientBase *client) override;
    bool setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig) override;
    void task_handler(void) override;

    // identity
    void setMyInfo(uint32_t nodeNum) override;
    void setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                           bool can_shutdown) override;

    // node list
    void addNode(uint32_t nodeNum, uint8_t channel, const char *userShort, const char *userLong, uint32_t lastHeard,
                 eRole role, bool hasKey, bool unmessagable) override;
    void updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg) override;
    void addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg) override;
    void removeNode(uint32_t nodeNum) override;
    void updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats,
                        uint32_t precision) override;
    void updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chUtil, float airUtil) override;
    void updateEnvironmentMetrics(uint32_t nodeNum, const meshtastic_EnvironmentMetrics &metrics) override;
    void updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr) override;
    void updateHopsAway(uint32_t nodeNum, uint8_t hopsAway) override;
    void updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status) override;

    // config in (shadow fill); every group is stored because the editors read
    // back out of the shadow.
    void updateChannelConfig(const meshtastic_Channel &ch) override;
    void updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg) override;
    void updatePositionConfig(const meshtastic_Config_PositionConfig &cfg) override;
    void updatePowerConfig(const meshtastic_Config_PowerConfig &cfg) override;
    void updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg) override;
    void updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg) override;
    void updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg) override;
    void updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id = 0) override;
    void updateSecurityConfig(const meshtastic_Config_SecurityConfig &cfg) override;
    void updateSessionKeyConfig(const meshtastic_Config_SessionkeyConfig &cfg) override;

    void updateMQTTModule(const meshtastic_ModuleConfig_MQTTConfig &cfg) override;
    void updateSerialModule(const meshtastic_ModuleConfig_SerialConfig &cfg) override;
    void updateExtNotificationModule(const meshtastic_ModuleConfig_ExternalNotificationConfig &cfg) override;
    void updateStoreForwardModule(const meshtastic_ModuleConfig_StoreForwardConfig &cfg) override;
    void updateRangeTestModule(const meshtastic_ModuleConfig_RangeTestConfig &cfg) override;
    void updateTelemetryModule(const meshtastic_ModuleConfig_TelemetryConfig &cfg) override;
    void updateCannedMessageModule(const meshtastic_ModuleConfig_CannedMessageConfig &cfg) override;
    void updateAudioModule(const meshtastic_ModuleConfig_AudioConfig &cfg) override;
    void updateRemoteHardwareModule(const meshtastic_ModuleConfig_RemoteHardwareConfig &cfg) override;
    void updateNeighborInfoModule(const meshtastic_ModuleConfig_NeighborInfoConfig &cfg) override;
    void updateAmbientLightingModule(const meshtastic_ModuleConfig_AmbientLightingConfig &cfg) override;
    void updateDetectionSensorModule(const meshtastic_ModuleConfig_DetectionSensorConfig &cfg) override;
    void updatePaxCounterModule(const meshtastic_ModuleConfig_PaxcounterConfig &cfg) override;

    // Groups upstream device-ui has no callbacks for; the dispatch is added
    // by this port's device-ui patches.
    void updateStatusMessageModule(const meshtastic_ModuleConfig_StatusMessageConfig &cfg) override;
    void updateTrafficManagementModule(const meshtastic_ModuleConfig_TrafficManagementConfig &cfg) override;
    void updateTakModule(const meshtastic_ModuleConfig_TAKConfig &cfg) override;
    void updateMeshBeaconModule(const meshtastic_ModuleConfig_MeshBeaconConfig &cfg) override;

    void updateTime(uint32_t time) override;
    void configCompleted(void) override;

    // messaging
    void newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime,
                    bool restore = false) override;
    void restoreMessage(const LogMessage &msg) override;
    void notifyMessagesRestored(void) override;
    void showMessagePopup(const char *from) override;

    void handleResponse(uint32_t from, uint32_t id, const meshtastic_Routing &routing,
                        const meshtastic_MeshPacket &p) override;

    void notifyResync(bool show) override;
    void notifyReboot(bool show) override;
    void notifyShutdown(void) override;
    void blankScreen(bool enable) override;

  private:
    friend class ViewFactory;
    static TFTView_540x960 *instance(void);
    static TFTView_540x960 *instance(const DisplayDriverConfig &cfg);
    TFTView_540x960();
    TFTView_540x960(const DisplayDriverConfig *cfg, DisplayDriver *driver);

    // Navigation: panels are pushed on entry and popped by Back. Modals are
    // not pushed - each is dismissed only by its own Cancel, and Back is
    // disabled while one is open.
    static constexpr uint8_t NAV_STACK_MAX = 8;

    struct NavEntry {
        lv_obj_t *button;
        lv_obj_t *panel;
        char context[48]; // owned copy; hasContext false = no context bar
        bool hasContext;
    };

    void ui_set_active(lv_obj_t *button, lv_obj_t *panel, const char *context);
    void navPush(lv_obj_t *button, lv_obj_t *panel, const char *context);
    void navPop(void);
    void navReset(lv_obj_t *button, lv_obj_t *panel, const char *context);
    void updateBackButton(void);

    NavEntry navStack[NAV_STACK_MAX];
    uint8_t navDepth = 0;
    bool modalOpen = false;

    // Settings surface: one screen, five radio option pickers. Everything
    // edits pending state; SAVE SETTINGS AND REBOOT publishes the batch.
    void showSettings(void);
    void refreshSettings(void);
    void openOption(uint8_t kind);          // OPT_* from UiIndex.h
    void refreshOptionChecks(uint8_t kind); // CHECKED = the pending row
    void saveAndReboot(void);
    // SAVE waits for its acks: each admin message carries a request id, the
    // local radio acks each one, and the reboot goes out only after the last
    // ack (or the response handler's timeout sweep).
    bool sendAdminTracked(meshtastic_AdminMessage &m);
    void onSaveAck(void);
    uint8_t pendingSaveAcks = 0;
    bool savePending = false;
    void showPairing(void);
    // Transient text in the context bar. show=false only clears the gate.
    void messageAlert(const char *text, bool show);

    // Pending edits, staged; -1 = untouched, so SAVE publishes only the
    // groups the user actually changed and never overwrites values the
    // option tables cannot express.
    bool configSeeded = false;  // set by configCompleted()
    bool dirtyOwner = false;    // long/short name edited
    bool dirtyLora = false;     // region or modem preset chosen
    bool dirtyNetwork = false;  // SSID or password edited
    bool dirtyDisplay = false;  // screen timeout chosen
    bool dirtyUiConfig = false; // brightness or language chosen

    int16_t pendingRegionIdx = -1;
    int16_t pendingPresetIdx = -1;
    int16_t pendingTimeoutIdx = -1;
    int16_t pendingBrightnessIdx = -1;
    int16_t pendingLanguageIdx = -1;
    char pendingText[64] = {0};
    bool rebootPending = false;  // a published change asked for a restart
    bool entryIsNumeric = false; // the entry panel is hosting the keypad

    // Node panel child order. Fixed: every update path indexes into it.
    enum NodeChild {
        node_img_idx = 0,
        node_btn_idx,
        node_lbl_idx, // long name
        node_lbs_idx, // short name
        node_bat_idx,
        node_lh_idx,  // last heard
        node_sig_idx, // signal or hops
        node_pos_idx, // position, holds lat in user_data
        node_dst_idx, // distance, holds lon in user_data
        node_child_count
    };

    lv_obj_t *newNodePanel(uint32_t nodeNum, uint8_t ch);
    void setNodeImage(uint32_t nodeNum, eRole role, bool unmessagable, lv_obj_t *img);
    void sortNodeByLastHeard(lv_obj_t *panel, uint32_t lastHeard);
    void purgeOldestNode(uint32_t keep);
    void updateNodesStatus(void);
    void updateAllLastHeard(void);
    void updateDistance(uint32_t nodeNum, int32_t lat, int32_t lon);
    void showNodeActions(uint32_t nodeNum);
    void hideNodeActions(void);
    void applyNodeAction(uint8_t action);

    void refreshChannels(void);
    void openChannelEditor(uint8_t chIndex);

    // Conversations keep the newest MAX_MSGS_PER_CHAT messages.
    static constexpr uint32_t MAX_MSGS_PER_CHAT = 200;
    void trimConversation(lv_obj_t *container);

    void showChannelEditor(uint8_t chIndex);
    void refreshChannelEditor(void);
    void publishChannel(uint8_t chIndex);

    // Channel edits are staged here; APPLY commits and publishes, CANCEL and
    // Back discard, and the next open re-copies from db.
    meshtastic_Channel chanScratch = meshtastic_Channel_init_default;
    bool chanRekeyArmed = false; // ENCRYPTION needs a second tap to regenerate
    void clearNodeList(void);

    // First-run gate: region must exist before the radio will transmit.
    bool needsSetup(void) const;
    void showSetup(void);
    void refreshSetup(void);

    // The entry panel is shared; the target says whose string it holds. The
    // settings strings commit into pending state, never straight to the radio.
    enum TextTarget : uint8_t {
        TEXT_NONE,
        TEXT_OWNER_LONG,
        TEXT_OWNER_SHORT,
        TEXT_CHANNEL_NAME,
        TEXT_WIFI_SSID,
        TEXT_WIFI_PSK,
    };
    void openTextEntry(TextTarget target, const char *title, const char *current);
    bool commitTextEntry(void);
    void closeTextOverlay(void);

    TextTarget textTarget = TEXT_NONE;

    lv_obj_t *newMessageContainer(uint32_t from, uint32_t to, uint8_t ch);
    lv_obj_t *messageContainerFor(uint32_t from, uint32_t to, uint8_t ch);
    lv_obj_t *addMessage(lv_obj_t *container, const char *text, bool outgoing);
    void showMessages(uint32_t nodeNumOrChannel, bool isChannel);
    void addChat(uint32_t from, uint32_t to, uint8_t ch);
    void updateActiveChats(void);
    void handleAddMessage(const char *msg);

    // Delivery state: broadcasts have nobody to ack; a direct message's
    // routing reply recolors its bubble border.
    void markMessageStatus(uint32_t requestId, bool ack, bool err);
    // requestId -> bubble label. Each entry's widget carries an
    // LV_EVENT_DELETE handler that erases the entry, so a late ack can never
    // write into freed memory and the map cannot outgrow the capped widgets.
    std::unordered_map<uint32_t, lv_obj_t *> pendingAcks;

    void addOrUpdateMap(uint32_t nodeNum, int32_t lat, int32_t lon);
    void removeFromMap(uint32_t nodeNum);
    void redrawMap(void);
    void mapFitAll(void);

    void refreshHome(void);
    // One line, or two. Every row owns both labels; a one-line row hides the
    // second and re-centres the first.
    void setRow(uint8_t row, const char *l1, const char *l2);
    void setRowDim(uint8_t row, bool dim);
    void layoutKeyboard(lv_obj_t *kb, lv_obj_t *ta, bool withActions);
    const char *loraBandwidthText(void);
    void pollSDCard(void);

    // Split by how fast the driven value actually changes - see task_handler.
    time_t lastTick1 = 0;
    time_t lastTick60 = 0;

    // Change gating: every visible update costs a panel refresh, so compare
    // the rendered string and repaint only on difference.
    bool setLabelIfChanged(lv_obj_t *label, char *cache, size_t cacheLen, const char *text);

    char cacheClock[12] = {0};
    char cacheBattery[8] = {0};
    char cacheContext[48] = {0};
    char cacheHome[11][80] = {};
    char cacheHome2[11][80] = {};

    // Live state not carried by the base.
    uint32_t selectedNode = 0;
    uint8_t selectedChannel = 0;
    bool hasPosition = false;
    int32_t myLatitude = 0, myLongitude = 0;
    int32_t lastRssi = 0;
    float lastSnr = 0.0f;
    float chUtil = 0.0f, airUtil = 0.0f;
    uint32_t batteryLevel = 0;
    float batteryVoltage = 0.0f;
    uint32_t sdUsedMB = 0, sdTotalMB = 0;
    char firmwareVersion[24] = {0};
    bool hasBluetooth = false, hasWifi = false, hasEthernet = false, canShutdown = false;

    // Map viewport: metres per pixel plus a pan offset from own position.
    float mapMetresPerPx = 0.0f; // 0 = auto-fit
    float mapPanE = 0.0f, mapPanN = 0.0f;
    bool mapFollowOwn = true;
    std::unordered_map<uint32_t, lv_obj_t *> mapMarkers;

    // lvgl callbacks (static: lvgl is C)
    void ui_events_init(void);

    static void ui_event_Back(lv_event_t *e);
    static void ui_event_HomeButton(lv_event_t *e);
    static void ui_event_NodesButton(lv_event_t *e);
    static void ui_event_GroupsButton(lv_event_t *e);
    static void ui_event_MessagesButton(lv_event_t *e);
    static void ui_event_MapButton(lv_event_t *e);
    static void ui_event_SettingsButton(lv_event_t *e);

    static void ui_event_SettingsRow(lv_event_t *e);  // user_data = row id below
    static void ui_event_SettingsText(lv_event_t *e); // user_data = TextTarget
    static void ui_event_OptRow(lv_event_t *e);       // user_data = (kind<<8)|index
    static void ui_event_ScrollArrow(lv_event_t *e);  // user_data = scroll container
    static void ui_event_PairingClose(lv_event_t *e);
    static void ui_event_EditorOk(lv_event_t *e);
    static void ui_event_EditorCancel(lv_event_t *e);

    // Settings row ids for ui_event_SettingsRow's user_data.
    enum SettingsRow : uint8_t {
        SROW_REGION = 0,
        SROW_PRESET,
        SROW_TIMEOUT,
        SROW_BRIGHTNESS,
        SROW_LANGUAGE,
        SROW_REBOOT,
        SROW_SHUTDOWN,
        SROW_BLUETOOTH,
        SROW_SAVE,
    };

    static void ui_event_HomeRow(lv_event_t *e);       // user_data = row index
    static void ui_event_NodeButton(lv_event_t *e);    // user_data = nodeNum
    static void ui_event_NodeAction(lv_event_t *e);    // user_data = action index
    static void ui_event_ChannelButton(lv_event_t *e); // user_data = channel index
    static void ui_event_ChatButton(lv_event_t *e);    // user_data = nodeNum or channel
    static void ui_event_MessageSend(lv_event_t *e);
    static void ui_event_MapControl(lv_event_t *e);    // user_data = control id

    static void ui_event_ChannelEditRow(lv_event_t *e);   // user_data = row index
    static void ui_event_AckWidgetDeleted(lv_event_t *e); // user_data = requestId
    static void ui_event_ChannelApply(lv_event_t *e);     // commit chanScratch
    static void ui_event_ChannelCancel(lv_event_t *e);    // discard chanScratch
    static void ui_event_SetupRegion(lv_event_t *e);
    static void ui_event_SetupName(lv_event_t *e);
    static void ui_event_SetupDone(lv_event_t *e);

    static TFTView_540x960 *gui;

    // Shadow config db. Mirrors upstream's layout so the generated accessors
    // bind straight to db.config and db.module_config.
    struct meshtastic_DeviceProfile_ext : meshtastic_DeviceProfile {
        meshtastic_User user;
        meshtastic_Channel channel[c_max_channels];
        meshtastic_DeviceUIConfig uiConfig;
    };
    struct meshtastic_DeviceProfile_full : meshtastic_DeviceProfile_ext {
        bool silent;
        meshtastic_DeviceConnectionStatus connectionStatus;
    };
    meshtastic_DeviceProfile_full db{};
};
