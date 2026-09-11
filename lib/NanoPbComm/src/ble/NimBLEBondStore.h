#ifndef NANOPBCOMM_NIMBLE_BOND_STORE_H
#define NANOPBCOMM_NIMBLE_BOND_STORE_H

#include <Arduino.h>
#include <NimBLEBondMigration.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <nvs.h>

// Shared NVS namespace with the paired-peer address. The bond2x flag records that
// the NimBLE 1.x NVS layout was converted so we do not re-run the helper every boot.
static constexpr const char *GM_BLE_NVS_NAMESPACE = "gmble";
static constexpr const char *GM_BLE_NVS_PEER_KEY = "peer";
static constexpr const char *GM_BLE_NVS_BOND2X_KEY = "bond2x";

// Native NimBLE wire order from getVal() (not the reversing uint8_t[6] ctor).
inline void packPeerAddress(const NimBLEAddress &address, uint8_t buf[7]) {
    memcpy(buf, address.getVal(), 6);
    buf[6] = address.getType();
}

inline NimBLEAddress unpackPeerAddress(const uint8_t buf[7]) {
    ble_addr_t addr;
    memcpy(addr.val, buf, 6);
    addr.type = buf[6];
    return NimBLEAddress(addr);
}

enum class NimBLEBondStoreLayout { Empty, Current, Legacy, Unknown, Unreadable };

// Size-check our_sec_*/peer_sec_* with the helper's own structs (host packing can differ).
// local_irk_* is not a trigger: 1.x often never wrote it.
inline NimBLEBondStoreLayout probeNimBLEBondStoreLayout() {
    nvs_handle_t handle = 0;
    const esp_err_t openErr = nvs_open("nimble_bond", NVS_READONLY, &handle);
    if (openErr == ESP_ERR_NVS_NOT_FOUND)
        return NimBLEBondStoreLayout::Empty;
    if (openErr != ESP_OK)
        return NimBLEBondStoreLayout::Unreadable;

    bool sawAny = false;
    bool sawLegacy = false;
    bool sawUnknown = false;
    char key[16];
    static constexpr const char *kPrefixes[] = {"our_sec", "peer_sec"};
    const uint16_t maxEntries = MYNEWT_VAL(BLE_STORE_MAX_BONDS);
    for (uint16_t i = 1; i <= maxEntries; ++i) {
        for (const char *prefix : kPrefixes) {
            const int written = snprintf(key, sizeof(key), "%s_%u", prefix, i);
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(key))
                continue;
            size_t blobSize = 0;
            const esp_err_t err = nvs_get_blob(handle, key, nullptr, &blobSize);
            if (err == ESP_ERR_NVS_NOT_FOUND)
                continue;
            if (err != ESP_OK) {
                sawUnknown = true;
                continue;
            }
            sawAny = true;
            if (blobSize == sizeof(NimBLEBondMigration::detail::BleStoreValueSecV1))
                sawLegacy = true;
            else if (blobSize != sizeof(NimBLEBondMigration::detail::BleStoreValueSecCurrent))
                sawUnknown = true;
        }
    }
    nvs_close(handle);

    if (sawLegacy)
        return NimBLEBondStoreLayout::Legacy;
    if (sawUnknown)
        return NimBLEBondStoreLayout::Unknown;
    if (sawAny)
        return NimBLEBondStoreLayout::Current;
    return NimBLEBondStoreLayout::Empty;
}

// Must run before NimBLEDevice::init(). Reboot only after a real 1.x→2.x rewrite so
// the stack's first load of nimble_bond is already 2.x (official helper: do not
// init() in the same boot that rewrote the store). Empty/already-2.x stores just
// set the flag — calling the helper would plant the 1.x default IRK.
// Rollback 2.x→1.x is not implemented: run migrateBondStoreToV1() or wipe
// nimble_bond before flashing 1.x, otherwise init() can crash.
inline void migrateNimBLEBondsOnce(const char *logTag) {
    Preferences prefs;
    if (!prefs.begin(GM_BLE_NVS_NAMESPACE, false)) {
        ESP_LOGW(logTag, "Could not open gmble NVS for bond migration");
        return;
    }
    if (prefs.getBool(GM_BLE_NVS_BOND2X_KEY, false)) {
        prefs.end();
        return;
    }

    const NimBLEBondStoreLayout layout = probeNimBLEBondStoreLayout();
    if (layout == NimBLEBondStoreLayout::Unreadable) {
        ESP_LOGW(logTag, "Could not read nimble_bond; will retry bond migration next boot");
        prefs.end();
        return;
    }
    if (layout == NimBLEBondStoreLayout::Empty || layout == NimBLEBondStoreLayout::Current) {
        prefs.putBool(GM_BLE_NVS_BOND2X_KEY, true);
        prefs.end();
        return;
    }
    if (layout == NimBLEBondStoreLayout::Unknown) {
        ESP_LOGW(logTag, "NimBLE bond store has unexpected record size; existing pairing may need re-pair");
        prefs.putBool(GM_BLE_NVS_BOND2X_KEY, true);
        prefs.end();
        return;
    }

    const bool ok = NimBLEBondMigration::migrateBondStoreToCurrent();
    if (!ok) {
        ESP_LOGW(logTag, "NimBLE bond store migration failed; existing pairing may need re-pair");
        prefs.end();
        return;
    }
    prefs.putBool(GM_BLE_NVS_BOND2X_KEY, true);
    prefs.end();
    ESP_LOGI(logTag, "NimBLE bond store converted from 1.x, restarting");
    ESP.restart();
}

#endif // NANOPBCOMM_NIMBLE_BOND_STORE_H
