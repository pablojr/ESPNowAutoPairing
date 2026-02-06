#include "ESPNowAutoPairing.h"
#include <esp_wifi.h>

// Static member definitions
ESPNowAutoPairing* ESPNowAutoPairing::_instance = nullptr;
ESPNowAutoPairing::UserRecvCallback ESPNowAutoPairing::_userRecvCb = nullptr;

ESPNowAutoPairing::ESPNowAutoPairing(DeviceRole role) {
    _role = role;
    _pairingStatus = PAIRING_NONE;
    _pairingMode = false;
    _lastSendTime = 0;
    _instance = this;
    
    // Initialize MAC address
    for (int i = 0; i < 6; i++) {
        _pairedMacAddress[i] = 0xFF;
    }
}

void ESPNowAutoPairing::begin() {
    // EEPROM initialization
    EEPROM.begin(EEPROM_SIZE);
    loadPairingData();

    // WiFi settings
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Set to a fixed channel (same value on all devices)
    const uint8_t ESPNOW_CHANNEL = 1;
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);
    Serial.printf("WiFi channel set to: %u\n", ch);

    // ESP-NOW initialization
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed");
        return;
    }

    // Registering a callback function
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // Peer configuration
    memset(&_peerInfo, 0, sizeof(_peerInfo));
    
    if (_pairingStatus == PAIRING_PAIRED) {
        // If paired previously, use the stored MAC address
        memcpy(_peerInfo.peer_addr, _pairedMacAddress, 6);
        Serial.println("Using paired MAC address");
    } else {
        // If not paired, set for broadcast
        for (int i = 0; i < 6; ++i) {
            _peerInfo.peer_addr[i] = 0xFF;
        }
        Serial.println("No pairing data found. Ready for pairing.");
    }

    // Explicitly specify the channel
    _peerInfo.channel = 1;
    _peerInfo.encrypt = false;

    esp_err_t addStatus = esp_now_add_peer(&_peerInfo);
    if (addStatus == ESP_OK) {
        Serial.println("Peer added successfully");
    } else {
        Serial.println("Failed to add peer");
    }

    Serial.println("ESP-NOW Auto Pairing initialized");
}

void ESPNowAutoPairing::setUserRecvCallback(UserRecvCallback cb) {
    _userRecvCb = cb;
}

void ESPNowAutoPairing::startPairingMode() {
    _pairingMode = true;
    Serial.println("Entering pairing mode...");
}

void ESPNowAutoPairing::stopPairingMode() {
    _pairingMode = false;
    Serial.println("Exiting pairing mode...");
}

bool ESPNowAutoPairing::isPaired() {
    return (_pairingStatus == PAIRING_PAIRED);
}

uint8_t* ESPNowAutoPairing::getPairedMacAddress() {
    return _pairedMacAddress;
}

void ESPNowAutoPairing::clearPairingData() {
    // Clear pairing data
    for (int i = 0; i < 6; i++) {
        _pairedMacAddress[i] = 0xFF;
    }
    _pairingStatus = PAIRING_NONE;
    
    // Store pairing data in EEPROM
    savePairingData();
    
    Serial.println("Pairing data cleared");
}

void ESPNowAutoPairing::sendData(uint8_t* data, size_t len) {
    if (_pairingStatus == PAIRING_PAIRED) {
        esp_now_send(_peerInfo.peer_addr, data, len);
    } else {
        Serial.println("Not paired. Cannot send data.");
    }
}

void ESPNowAutoPairing::loadPairingData() {
    EEPROM.readBytes(0, _pairedMacAddress, 6);
    _pairingStatus = EEPROM.read(6);
    
    Serial.print("Loaded MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", _pairedMacAddress[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    Serial.printf("Pairing status: %d\n", _pairingStatus);
}

void ESPNowAutoPairing::savePairingData() {
    EEPROM.writeBytes(0, _pairedMacAddress, 6);
    EEPROM.write(6, _pairingStatus);
    EEPROM.commit();
    Serial.println("Pairing data saved to EEPROM");
}

void ESPNowAutoPairing::addPeer(const uint8_t* mac_addr) {
    // Delete existing peer (target MAC)
    esp_now_del_peer(mac_addr);
    // New peer configuration
    memset(&_peerInfo, 0, sizeof(_peerInfo));
    memcpy(_peerInfo.peer_addr, mac_addr, 6);
    _peerInfo.channel = ESPNOW_CHANNEL;          // Use a fixed channel
    _peerInfo.encrypt = false;

    esp_err_t st = esp_now_add_peer(&_peerInfo);
    if (st != ESP_OK) {
        Serial.printf("Failed to add peer (err=%d)\n", (int)st);
    } else {
        Serial.println("New peer added successfully");
    }
}

void ESPNowAutoPairing::deletePeer(const uint8_t* mac_addr) {
    esp_now_del_peer(mac_addr);
}

void ESPNowAutoPairing::OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.print("Last Packet Sent to: ");
    Serial.println(macStr);
    Serial.print("Last Packet Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

void ESPNowAutoPairing::OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int data_len) {
    if (_instance == nullptr) return;

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.printf("Last Packet Recv from: %s\n", macStr);

    if (data_len >= sizeof(pairing_message_t)) {
        pairing_message_t msg;
        memcpy(&msg, incomingData, sizeof(msg));

        if (_instance->_pairingMode) {
            if (_instance->_role == SLAVE && msg.type == PAIR_REQUEST) {
                Serial.println("Received PAIR_REQUEST");
                // When PAIR_REQUEST is received, return PAIR_RESPONSE
                pairing_message_t responseMsg;
                responseMsg.type = PAIR_RESPONSE;
                
                //Get your MAC address
                uint8_t myMac[6];
                WiFi.macAddress(myMac);
                memcpy(responseMsg.mac, myMac, 6);
                // Add a destination peer (channel matching)
                _instance->addPeer(mac_addr);
                esp_err_t s = esp_now_send(mac_addr, (uint8_t *) &responseMsg, sizeof(responseMsg));
                Serial.println("Sent PAIR_RESPONSE");
                if (s != ESP_OK) {
                    Serial.printf("esp_now_send(PAIR_RESPONSE) err=%d\n", (int)s);
                }
                
            } else if (_instance->_role == MASTER && msg.type == PAIR_RESPONSE) {
                Serial.println("Received PAIR_RESPONSE");
                // If PAIR_RESPONSE is received, save the MAC address and return PAIR_CONFIRM.
                memcpy(_instance->_pairedMacAddress, msg.mac, 6);
                _instance->_pairingStatus = PAIRING_PAIRED;
                _instance->savePairingData();
                
                Serial.println("Paired MAC address saved to EEPROM.");
                
                pairing_message_t confirmMsg;
                confirmMsg.type = PAIR_CONFIRM;
                
                // Get your MAC address
                uint8_t myMac[6];
                WiFi.macAddress(myMac);
                memcpy(confirmMsg.mac, myMac, 6);
                // Add a destination peer before sending
                _instance->addPeer(_instance->_pairedMacAddress);
                esp_err_t s2 = esp_now_send(mac_addr, (uint8_t *) &confirmMsg, sizeof(confirmMsg));
                Serial.println("Sent PAIR_CONFIRM");
                if (s2 != ESP_OK) {
                    Serial.printf("esp_now_send(PAIR_CONFIRM) err=%d\n", (int)s2);
                }
                _instance->stopPairingMode();
                
            } else if (_instance->_role == SLAVE && msg.type == PAIR_CONFIRM) {
                Serial.println("Received PAIR_CONFIRM");
                // Once PAIR_CONFIRM is received, save the MAC address and pairing is complete.
                memcpy(_instance->_pairedMacAddress, msg.mac, 6);
                _instance->_pairingStatus = PAIRING_PAIRED;
                _instance->savePairingData();
                
                Serial.println("Paired MAC address saved to EEPROM.");
                _instance->stopPairingMode();
                _instance->addPeer(_instance->_pairedMacAddress);
            }
        }
    }

    // After internal library processing, if there is a user callback
    // it will be forwarded
    if (_userRecvCb) {
        _userRecvCb(mac_addr, incomingData, data_len);
    }
}

