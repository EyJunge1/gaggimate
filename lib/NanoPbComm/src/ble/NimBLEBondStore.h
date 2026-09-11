#ifndef NANOPBCOMM_NIMBLE_BOND_STORE_H
#define NANOPBCOMM_NIMBLE_BOND_STORE_H

#include <Arduino.h>
#include <NimBLEBondMigration.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <cstring>
#include <esp_log.h>

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

// Must run before NimBLEDevice::init(). A successful conversion reboots so the
// stack's first load of nimble_bond is already 2.x (official helper flow + #740:
// do not init() in the same boot that rewrote the store). Failed migration leaves
// pairing identity in gmble/peer; the user may need to re-pair for encryption keys.
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
    // Returns true for an empty or already-2.x store; the helper logs converted counts.
    const bool ok = NimBLEBondMigration::migrateBondStoreToCurrent();
    if (!ok) {
        ESP_LOGW(logTag, "NimBLE bond store migration failed; existing pairing may need re-pair");
        prefs.end();
        return;
    }
    prefs.putBool(GM_BLE_NVS_BOND2X_KEY, true);
    prefs.end();
    ESP_LOGI(logTag, "NimBLE bond store ready for 2.x, restarting");
    ESP.restart();
}

#endif // NANOPBCOMM_NIMBLE_BOND_STORE_H
