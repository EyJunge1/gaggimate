#include "BleServerTransport.h"
#include "NimBLEBondStore.h"
#include <Preferences.h>

void BleServerTransport::init(const String &deviceName) {
    migrateNimBLEBondsOnce(LOG_TAG);

    NimBLEDevice::init(deviceName.c_str());
    ESP_LOGI(LOG_TAG, "%s", NimBLEDevice::getVersion());
    NimBLEDevice::setPower(9);
    NimBLEDevice::setMTU(256); // headroom for batched frames

    // Just Works bonding + LE Secure Connections (no IO -> no MITM); keys persist in NVS across reboots.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(this, false);
    // Restart advertising ourselves in onDisconnect; the automatic restart would bypass the directed/paired mode.
    _server->advertiseOnDisconnect(false);

    NimBLEService *service = _server->createService(gm_proto::SERVICE_UUID);
    _rxChar = service->createCharacteristic(gm_proto::RX_CHAR_UUID,
                                            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    _rxChar->setCallbacks(this);
    _txChar = service->createCharacteristic(gm_proto::TX_CHAR_UUID,
                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
    _txChar->setCallbacks(this);
    // INFO stays readable without encryption so legacy/pre-pairing readers work.
    _infoChar = service->createCharacteristic(gm_proto::INFO_CHAR_UUID, NIMBLE_PROPERTY::READ);
    _infoChar->setValue(std::string(_info.c_str()));

    // OTA DFU shares the same server (separate service/UUIDs).
    _otaDfu.configure_OTA(_server);
    _otaDfu.start_OTA();

    _deviceName = deviceName;
    _advertising = NimBLEDevice::getAdvertising();
    _advertising->enableScanResponse(true);
    // First boot pairs openly; once a display has bonded, only it may connect.
    loadPairedPeer();
    if (_havePairedPeer) {
        NimBLEDevice::whiteListAdd(_pairedPeer);
        enableWhitelist();
        pruneForeignBonds(_pairedPeer);
        ESP_LOGI(LOG_TAG, "Paired to display %s", _pairedPeer.toString().c_str());
    } else if (NimBLEDevice::getNumBonds() > 0) {
        // Migration from multi-bond builds: allow all bonded displays and adopt the first one that encrypts.
        for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) {
            NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
            NimBLEDevice::whiteListAdd(addr);
            ESP_LOGW(LOG_TAG, "Legacy bond %s allowed until one display is adopted", addr.toString().c_str());
        }
        enableWhitelist();
    }
    applyAdvertisingData();
    startAdv();
    ESP_LOGI(LOG_TAG, "BLE server started, advertising %s",
             _havePairedPeer ? "(directed to paired display)" : (_whitelistOnly ? "(whitelist only)" : "(open, pairing mode)"));
}

void BleServerTransport::startAdv() {
    if (_advertising == nullptr || _advertising->isAdvertising())
        return;
    bool started;
    if (_havePairedPeer) {
        // Low-duty directed adverts are LL-dropped by every radio except the paired display's -- invisible to other scanners.
        _advertising->setConnectableMode(BLE_GAP_CONN_MODE_DIR);
        started = _advertising->start(0, &_pairedPeer);
    } else {
        _advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
        started = _advertising->start();
    }
    if (!started)
        ESP_LOGE(LOG_TAG, "Advertising start failed (%s)", _havePairedPeer ? "directed" : "open");
}

void BleServerTransport::applyAdvertisingData() {
    // Primary adv packet (31B): flags + service UUID + lock-owner mfg data; owner must be primary, displays scan passively.
    std::vector<uint8_t> mfg = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    if (_havePairedPeer)
        memcpy(&mfg[2], _pairedPeer.getVal(), 6);
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setCompleteServices(NimBLEUUID(gm_proto::SERVICE_UUID));
    advData.setManufacturerData(mfg);
    if (!_advertising->setAdvertisementData(advData))
        ESP_LOGE(LOG_TAG, "Failed to set advertisement data");
    NimBLEAdvertisementData scanResp;
    scanResp.setName(std::string(_deviceName.c_str()));
    if (!_advertising->setScanResponseData(scanResp))
        ESP_LOGE(LOG_TAG, "Failed to set scan response data");
}

void BleServerTransport::enableWhitelist() {
    _whitelistOnly = true;
    _advertising->setScanFilter(true, true);
}

void BleServerTransport::adoptPeer(const NimBLEAddress &address) {
    if (_havePairedPeer) {
        if (_pairedPeer == address)
            return; // our display, nothing to do
        // Whitelisting should make this impossible; refuse the interloper.
        ESP_LOGW(LOG_TAG, "Rejecting bond from foreign display %s", address.toString().c_str());
        NimBLEDevice::deleteBond(address);
        disconnect();
        return;
    }
    savePairedPeer(address);
    pruneForeignBonds(address);
    // Reduce the (possibly legacy multi-bond) whitelist to this display; safe while connected, advertising is stopped.
    while (NimBLEDevice::getWhiteListCount() > 0)
        NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
    NimBLEDevice::whiteListAdd(address);
    enableWhitelist();
    applyAdvertisingData(); // broadcast the new lock owner from the next adv start
    ESP_LOGI(LOG_TAG, "Bonded to display %s, advertising is now whitelist-only", address.toString().c_str());
}

void BleServerTransport::pruneForeignBonds(const NimBLEAddress &keep) {
    std::vector<NimBLEAddress> foreign;
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) {
        NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
        if (addr != keep)
            foreign.push_back(addr);
    }
    for (auto &addr : foreign) {
        ESP_LOGW(LOG_TAG, "Removing stale bond %s", addr.toString().c_str());
        NimBLEDevice::deleteBond(addr);
    }
}

void BleServerTransport::loadPairedPeer() {
    Preferences prefs;
    if (!prefs.begin(GM_BLE_NVS_NAMESPACE, true))
        return;
    uint8_t buf[7];
    if (prefs.getBytes(GM_BLE_NVS_PEER_KEY, buf, sizeof(buf)) == sizeof(buf)) {
        _pairedPeer = unpackPeerAddress(buf);
        _havePairedPeer = true;
    }
    prefs.end();
}

void BleServerTransport::savePairedPeer(const NimBLEAddress &address) {
    Preferences prefs;
    if (!prefs.begin(GM_BLE_NVS_NAMESPACE, false))
        return;
    uint8_t buf[7];
    packPeerAddress(address, buf);
    prefs.putBytes(GM_BLE_NVS_PEER_KEY, buf, sizeof(buf));
    prefs.end();
    _pairedPeer = address;
    _havePairedPeer = true;
}

void BleServerTransport::clearBonds() {
    bool wasAdvertising = _advertising && _advertising->isAdvertising();
    if (wasAdvertising)
        _advertising->stop();
    while (NimBLEDevice::getWhiteListCount() > 0)
        NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
    NimBLEDevice::deleteAllBonds();
    Preferences prefs;
    if (prefs.begin(GM_BLE_NVS_NAMESPACE, false)) {
        prefs.remove(GM_BLE_NVS_PEER_KEY);
        prefs.end();
    }
    _havePairedPeer = false;
    _whitelistOnly = false;
    if (_advertising) {
        _advertising->setScanFilter(false, false);
        applyAdvertisingData(); // owner field back to zeros (open for pairing)
    }
    ESP_LOGW(LOG_TAG, "Bonds cleared, open for pairing");
    disconnect(); // drop the current peer (if any) so the next link re-pairs
    if (wasAdvertising)
        startAdv();
}

void BleServerTransport::startAdvertising() { startAdv(); }

void BleServerTransport::setInfo(const String &info) {
    _info = info;
    if (_infoChar)
        _infoChar->setValue(std::string(info.c_str()));
}

bool BleServerTransport::send(const uint8_t *data, size_t length) {
    if (!_connected || _txChar == nullptr)
        return false;
    return _txChar->notify(data, length);
}

bool BleServerTransport::isConnected() const { return _connected; }

void BleServerTransport::onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) {
    _connected = true;
    // Deliberately no startSecurity() here: the display is the sole initiator (dual initiation raced via EALREADY).
    _connHandle = connInfo.getConnHandle();
    server->stopAdvertising();
    ESP_LOGI(LOG_TAG, "Client connected");
    emitConnection(true);
}

void BleServerTransport::onAuthenticationComplete(NimBLEConnInfo &connInfo) {
    if (!connInfo.isEncrypted()) {
        // Comms characteristics require encryption anyway; drop peers that cannot pair rather than keep a half-usable link.
        ESP_LOGW(LOG_TAG, "Pairing/encryption failed, dropping connection");
        _server->disconnect(connInfo.getConnHandle());
        return;
    }
    if (connInfo.isBonded())
        adoptPeer(connInfo.getIdAddress());
}

void BleServerTransport::onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) {
    (void)server;
    (void)connInfo;
    _connected = false;
    _connHandle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(LOG_TAG, "Client disconnected, reason=%d", reason);
    emitConnection(false);
    startAdv();
}

void BleServerTransport::disconnect() {
    if (_connected && _server && _connHandle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(LOG_TAG, "Forcing client disconnect (conn=%u)", _connHandle);
        _server->disconnect(_connHandle);
    }
}

void BleServerTransport::onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) {
    (void)connInfo;
    if (characteristic != _rxChar)
        return;
    NimBLEAttValue value = characteristic->getValue();
    if (value.length() > 0)
        emitData(value.data(), value.length());
}

void BleServerTransport::onSubscribe(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo, uint16_t subValue) {
    (void)pCharacteristic;
    (void)connInfo;
    (void)subValue;
    emitConnection(true);
}
