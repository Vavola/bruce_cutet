#include "esp_connection.h"
#include "core/display.h"
#include <WiFi.h>

// Initialize the static instance pointer
EspConnection *EspConnection::instance = nullptr;
std::vector<Option> peerOptions;
static constexpr size_t ESPNOW_RECV_QUEUE_LEN = 16;
static constexpr size_t ESPNOW_PEER_QUEUE_LEN = 8;
struct PeerMac {
    uint8_t mac[6];
};

EspConnection::EspConnection() {
    setInstance(this);
    if (!recvQueue) recvQueue = xQueueCreate(ESPNOW_RECV_QUEUE_LEN, sizeof(Message));
    if (!peerQueue) peerQueue = xQueueCreate(ESPNOW_PEER_QUEUE_LEN, sizeof(PeerMac));
}

EspConnection::~EspConnection() {
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();

    esp_now_deinit();
    if (recvQueue) {
        vQueueDelete(recvQueue);
        recvQueue = nullptr;
    }
    if (peerQueue) {
        vQueueDelete(peerQueue);
        peerQueue = nullptr;
    }
}

bool EspConnection::beginSend() {
    sendStatus = CONNECTING;

    if (!beginEspnow()) return false;

    sendPing();
    buildPeerOptions();

    loopOptions(peerOptions);

    peerOptions.clear();
    peerEntries.clear();

    if (!setupPeer(dstAddress)) {
        displayError("Failed to add peer");
        delay(1000);
        return false;
    }

    return true;
}

bool EspConnection::beginEspnow() {
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        displayError("Error initializing share");
        delay(1000);
        return false;
    }

    if (!setupPeer(broadcastAddress)) {
        displayError("Failed to add peer");
        delay(1000);
        return false;
    }

    esp_now_register_send_cb(onDataSentStatic);
    esp_now_register_recv_cb(onDataRecvStatic);

    return true;
}

EspConnection::Message EspConnection::createMessage(String text) {
    Message message;

    message.dataSize = text.length();
    message.totalBytes = text.length();
    message.bytesSent = text.length();
    message.done = true;

    strncpy(message.data, text.c_str(), ESP_DATA_SIZE);

    return message;
}

EspConnection::Message EspConnection::createFileMessage(File file) {
    Message message;
    String path = String(file.path());

    message.isFile = true;
    message.totalBytes = file.size();

    strncpy(message.filename, file.name(), ESP_FILENAME_SIZE);
    strncpy(message.filepath, path.substring(0, path.lastIndexOf("/")).c_str(), ESP_FILEPATH_SIZE);
    message.filename[ESP_FILENAME_SIZE - 1] = '\0';
    message.filepath[ESP_FILEPATH_SIZE - 1] = '\0';

    return message;
}

EspConnection::Message EspConnection::createPingMessage() {
    Message message;
    message.ping = true;

    return message;
}

EspConnection::Message EspConnection::createPongMessage() {
    Message message;
    message.pong = true;

    return message;
}

void EspConnection::sendPing() {
    if (peerQueue) xQueueReset(peerQueue);

    Message message = createPingMessage();

    esp_err_t response = esp_now_send(broadcastAddress, (uint8_t *)&message, sizeof(message));
    if (response != ESP_OK) { Serial.printf("Send ping response: %s\n", esp_err_to_name(response)); }

    delay(500);
}

void EspConnection::buildPeerOptions() {
    peerOptions.clear();
    peerEntries.clear();

    peerOptions.push_back({"Broadcast", [this]() { setDstAddress(broadcastAddress); }});

    if (!peerQueue) return;

    PeerMac pm = {};
    while (xQueueReceive(peerQueue, &pm, 0) == pdTRUE) {
        bool exists = false;
        for (const auto &e : peerEntries) {
            if (memcmp(e.mac, pm.mac, 6) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        PeerEntry entry = {};
        memcpy(entry.mac, pm.mac, 6);
        entry.label = macToString(pm.mac);
        peerEntries.push_back(entry);
    }

    for (size_t i = 0; i < peerEntries.size(); ++i) {
        peerOptions.push_back({peerEntries[i].label.c_str(), [this, i]() { setDstAddress(peerEntries[i].mac); }});
    }
}

void EspConnection::sendPong(const uint8_t *mac) {
    Message message = createPongMessage();

    if (!setupPeer(mac)) return;

    esp_err_t response = esp_now_send(mac, (uint8_t *)&message, sizeof(message));
    if (response != ESP_OK) { Serial.printf("Send pong response: %s\n", esp_err_to_name(response)); }
}

bool EspConnection::setupPeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peerInfo = {};

    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    return esp_now_add_peer(&peerInfo) == ESP_OK;
}

void EspConnection::printMessage(Message message) {
    delay(100);

    Serial.println("Message Details:");
    if (message.ping) {
        Serial.println("Ping: " + String(message.ping));
        Serial.println("");
        return;
    }
    if (message.pong) {
        Serial.println("Pong: " + String(message.pong));
        Serial.println("");
        return;
    }

    if (message.isFile) {
        Serial.println("Filename: " + String(message.filename));
        Serial.println("Filepath: " + String(message.filepath));
    }
    Serial.println("Data Size: " + String(message.dataSize));
    Serial.println("Total Bytes: " + String(message.totalBytes));
    Serial.println("Bytes Sent: " + String(message.bytesSent));
    Serial.println("Done: " + String(message.done));
    Serial.print("Data: ");

    // Append data to the result if dataSize is greater than 0
    if (message.dataSize > 0) {
        for (size_t i = 0; i < message.dataSize; ++i) {
            Serial.print((char)message.data[i]); // Assuming data contains valid characters
        }
    } else {
        Serial.println("No data");
    }

    Serial.println("");
}

String EspConnection::macToString(const uint8_t *mac) {
    char macStr[18];
    sprintf(macStr, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return macStr;
}

void EspConnection::appendPeerToList(const uint8_t *mac) {
    if (!peerQueue) return;
    PeerMac peer = {};
    memcpy(peer.mac, mac, 6);
    xQueueSend(peerQueue, &peer, 0);
}

void EspConnection::onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        sendStatus = SUCCESS;
        Serial.println("ESPNOW send success");
    } else {
        sendStatus = FAILED;
        Serial.println("ESPNOW send fail");
    }
}

void EspConnection::onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    Message recvMessage;

    // Use reinterpret_cast and copy assignment
    const Message *incomingMessage = reinterpret_cast<const Message *>(incomingData);
    recvMessage = *incomingMessage; // Use copy assignment

    printMessage(recvMessage);

    if (recvMessage.ping) return sendPong(mac);
    if (recvMessage.pong) return appendPeerToList(mac);

    if (recvQueue) xQueueSend(recvQueue, &recvMessage, 0);
}

void EspConnection::onDataSentStatic(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    if (instance) instance->onDataSent(info->src_addr, status);
}

void EspConnection::onDataRecvStatic(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
    if (instance) instance->onDataRecv(info->src_addr, incomingData, len);
}
