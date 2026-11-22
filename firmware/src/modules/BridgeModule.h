#pragma once
#ifdef HAS_UART_BRIDGE

#include "MeshModule.h"
#include "concurrency/OSThread.h"
#include <Arduino.h>

/**
 * Puente UART entre dos placas Meshtastic con presets distintos.
 *
 * Qué cruza (solo tráfico de terceros, nunca el de las propias placas puente):
 *   - TEXT_MESSAGE_APP en broadcast (mensajes de canal).
 *   - NODEINFO_APP en broadcast; en unicast solo si el destino vive al otro lado
 *     o si es la respuesta a una petición que cruzó el puente.
 *   - TRACEROUTE_APP unicast con la misma regla.
 *   - ROUTING_APP (ACK/NAK) solo como respuesta a algo que cruzó el puente.
 *
 * Qué NO cruza: nada que esta placa no pueda descifrar con una clave de canal
 * (canales ajenos, DMs PKI), telemetría, posiciones, ni paquetes propios.
 *
 * Los canales se emparejan por NOMBRE, no por índice:
 *   - primario <-> primario (aunque se llamen distinto en cada red)
 *   - secundario <-> secundario con el mismo nombre exacto
 */
class BridgeModule : public MeshModule, private concurrency::OSThread
{
  public:
    BridgeModule();

  protected:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    bool wantPacket(const meshtastic_MeshPacket *p) override { return true; }
    int32_t runOnce() override;

  private:
    // ---- UART ----
    static constexpr uint32_t BRIDGE_BAUD = 460800;
    static constexpr size_t UART_RX_FIFO = 2048; // el valor por defecto (32 B) se desborda
    static constexpr uint32_t RX_TIMEOUT_MS = 50;

    // ---- Cabecera propia delante del protobuf ----
    // [ver(1)] [flags(1)] [nodeNum de la placa emisora (4, BE)] [nombre canal (12)]
    static constexpr uint8_t PROTO_VERSION = 1;
    static constexpr uint8_t FLAG_PRIMARY = 0x01;
    static constexpr size_t CH_NAME_LEN = 12; // meshtastic_ChannelSettings.name
    static constexpr size_t HDR_LEN = 2 + 4 + CH_NAME_LEN;

    // ---- Trama: [0xAA 0x55][LEN_H LEN_L][cabecera + protobuf][CRC_H CRC_L] ----
    static constexpr size_t MAX_FRAME_SIZE = 512; // payload + CRC

    // ---- Caché anti-eco {from, id} ----
    struct PacketSignature {
        uint32_t from;
        uint32_t id;
    };
    static constexpr size_t SEEN_CACHE_SIZE = 128;
    PacketSignature seenCache[SEEN_CACHE_SIZE];
    size_t seenIdx = 0;

    // ---- Nodos conocidos al otro lado del puente ----
    static constexpr size_t REMOTE_TABLE_SIZE = 64;
    uint32_t remoteNodes[REMOTE_TABLE_SIZE];
    size_t remoteIdx = 0;

    // ---- Estado del receptor UART ----
    uint8_t rxBuffer[MAX_FRAME_SIZE];
    size_t rxPos = 0;
    uint16_t expectedLen = 0;
    uint8_t rxState = 0; // 0:0xAA 1:0x55 2:LenH 3:LenL 4:Payload+CRC
    uint32_t lastRxMs = 0;

    bool isDuplicate(uint32_t from, uint32_t id) const;
    void recordPacket(uint32_t from, uint32_t id);
    bool isRemote(uint32_t node) const;
    void markRemote(uint32_t node);

    bool shouldBridge(const meshtastic_MeshPacket &mp) const;
    bool unicastShouldCross(const meshtastic_MeshPacket &mp) const;

    bool writeHeader(uint8_t chIndex, uint8_t *hdr) const;
    int resolveLocalChannel(const uint8_t *hdr) const;

    void sendOverUart(const meshtastic_MeshPacket &mp);
    void processFrame(const uint8_t *buf, uint16_t len);
    static uint16_t crc16(const uint8_t *data, size_t len);
};

extern BridgeModule *bridgeModule;

#endif // HAS_UART_BRIDGE
