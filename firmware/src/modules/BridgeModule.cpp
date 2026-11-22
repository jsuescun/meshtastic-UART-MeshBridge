#ifdef HAS_UART_BRIDGE

#include "BridgeModule.h"
#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "configuration.h"
#include <pb_decode.h>
#include <pb_encode.h>

BridgeModule *bridgeModule = nullptr;

BridgeModule::BridgeModule() : MeshModule("bridge"), concurrency::OSThread("BridgeModule")
{
    static_assert(CH_NAME_LEN == sizeof(meshtastic_ChannelSettings::name), "Tamaño de nombre de canal inesperado");
    static_assert(HDR_LEN + meshtastic_MeshPacket_size <= MAX_FRAME_SIZE - 2, "MAX_FRAME_SIZE demasiado pequeño");

    // Ver paquetes de canal dirigidos a terceros (traceroutes, NodeInfo unicast, ACKs).
    // encryptedOk se deja en false A PROPÓSITO: lo que no podamos descifrar no llega al módulo.
    isPromiscuous = true;

    memset(seenCache, 0, sizeof(seenCache));
    memset(remoteNodes, 0, sizeof(remoteNodes));

    Serial2.setTX(PIN_SERIAL2_TX);
    Serial2.setRX(PIN_SERIAL2_RX);
    Serial2.setFIFOSize(UART_RX_FIFO); // debe ir ANTES de begin()
    Serial2.begin(BRIDGE_BAUD);

    LOG_INFO("BridgeModule: UART1 TX=%d RX=%d a %u bps", PIN_SERIAL2_TX, PIN_SERIAL2_RX, (unsigned)BRIDGE_BAUD);
    setIntervalFromNow(10);
}

// ---------------------------------------------------------------------------
// Cachés
// ---------------------------------------------------------------------------

bool BridgeModule::isDuplicate(uint32_t from, uint32_t id) const
{
    if (id == 0)
        return false;
    for (const auto &s : seenCache)
        if (s.id == id && s.from == from)
            return true;
    return false;
}

void BridgeModule::recordPacket(uint32_t from, uint32_t id)
{
    if (id == 0 || isDuplicate(from, id))
        return;
    seenCache[seenIdx] = {from, id};
    seenIdx = (seenIdx + 1) % SEEN_CACHE_SIZE;
}

bool BridgeModule::isRemote(uint32_t node) const
{
    if (node == 0)
        return false;
    for (uint32_t n : remoteNodes)
        if (n == node)
            return true;
    return false;
}

void BridgeModule::markRemote(uint32_t node)
{
    if (node == 0 || node == NODENUM_BROADCAST || isRemote(node))
        return;
    remoteNodes[remoteIdx] = node;
    remoteIdx = (remoteIdx + 1) % REMOTE_TABLE_SIZE;
}

// ---------------------------------------------------------------------------
// Filtrado
// ---------------------------------------------------------------------------

bool BridgeModule::shouldBridge(const meshtastic_MeshPacket &mp) const
{
    const uint32_t me = nodeDB->getNodeNum();

    // Nunca reenviar lo originado por esta placa
    if (mp.from == 0 || mp.from == me)
        return false;

    // Solo lo descifrado con una clave de canal que esta placa conoce
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag || mp.pki_encrypted)
        return false;
    if (mp.hop_limit == 0)
        return false;
    if (isDuplicate(mp.from, mp.id))
        return false;

    const bool bcast = (mp.to == NODENUM_BROADCAST);
    if (!bcast && mp.to == me)
        return false; // dirigido a esta placa: se queda aquí

    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        return bcast; // solo mensajes de canal, nunca DMs

    case meshtastic_PortNum_NODEINFO_APP:
        return bcast || unicastShouldCross(mp);

    case meshtastic_PortNum_TRACEROUTE_APP:
        return !bcast && unicastShouldCross(mp);

    case meshtastic_PortNum_ROUTING_APP:
        // Solo ACK/NAK de algo que cruzó el puente
        return !bcast && mp.decoded.request_id != 0 && isDuplicate(mp.to, mp.decoded.request_id);

    default:
        return false; // telemetría, posición, etc.
    }
}

bool BridgeModule::unicastShouldCross(const meshtastic_MeshPacket &mp) const
{
    // Respuesta: cruza solo si la petición original pasó por el puente
    if (mp.decoded.request_id != 0)
        return isDuplicate(mp.to, mp.decoded.request_id);
    // Petición: cruza solo si el destino vive al otro lado
    return isRemote(mp.to);
}

// ---------------------------------------------------------------------------
// Emparejamiento de canales por nombre
// ---------------------------------------------------------------------------

bool BridgeModule::writeHeader(uint8_t chIndex, uint8_t *hdr) const
{
    if (chIndex >= channels.getNumChannels())
        return false;

    const meshtastic_Channel &ch = channels.getByIndex(chIndex);
    const uint32_t me = nodeDB->getNodeNum();

    hdr[0] = PROTO_VERSION;
    hdr[1] = 0;
    hdr[2] = (uint8_t)(me >> 24);
    hdr[3] = (uint8_t)(me >> 16);
    hdr[4] = (uint8_t)(me >> 8);
    hdr[5] = (uint8_t)(me);
    memset(hdr + 6, 0, CH_NAME_LEN);

    if (ch.role == meshtastic_Channel_Role_PRIMARY) {
        hdr[1] |= FLAG_PRIMARY; // el primario se empareja con el primario, sin mirar el nombre
        return true;
    }
    if (ch.role == meshtastic_Channel_Role_SECONDARY && ch.settings.name[0] != '\0') {
        strncpy((char *)hdr + 6, ch.settings.name, CH_NAME_LEN);
        return true;
    }
    return false; // deshabilitado o secundario sin nombre (ambiguo)
}

int BridgeModule::resolveLocalChannel(const uint8_t *hdr) const
{
    const bool wantPrimary = hdr[1] & FLAG_PRIMARY;
    char name[CH_NAME_LEN + 1] = {0};
    memcpy(name, hdr + 6, CH_NAME_LEN);

    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        const meshtastic_Channel &ch = channels.getByIndex(i);
        if (wantPrimary) {
            if (ch.role == meshtastic_Channel_Role_PRIMARY)
                return i;
        } else if (ch.role == meshtastic_Channel_Role_SECONDARY && strncmp(ch.settings.name, name, CH_NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// RF -> UART
// ---------------------------------------------------------------------------

ProcessMessage BridgeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (shouldBridge(mp))
        sendOverUart(mp);

    // El firmware local sigue procesando el paquete con normalidad
    return ProcessMessage::CONTINUE;
}

void BridgeModule::sendOverUart(const meshtastic_MeshPacket &mp)
{
    uint8_t payload[MAX_FRAME_SIZE - 2];

    if (!writeHeader(mp.channel, payload)) {
        LOG_WARN("BridgeModule: canal local %u no puenteable, descartado", (unsigned)mp.channel);
        return;
    }

    meshtastic_MeshPacket out = mp;
    if (out.hop_limit > 0)
        out.hop_limit--; // el puente cuenta como un salto

    pb_ostream_t stream = pb_ostream_from_buffer(payload + HDR_LEN, sizeof(payload) - HDR_LEN);
    if (!pb_encode(&stream, meshtastic_MeshPacket_fields, &out)) {
        LOG_ERROR("BridgeModule: error codificando MeshPacket");
        return;
    }

    const uint16_t len = HDR_LEN + stream.bytes_written;
    const uint16_t crc = crc16(payload, len);

    recordPacket(out.from, out.id);

    const uint8_t head[4] = {0xAA, 0x55, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF)};
    const uint8_t tail[2] = {(uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)};
    Serial2.write(head, sizeof(head));
    Serial2.write(payload, len);
    Serial2.write(tail, sizeof(tail));
    // Sin flush(): no bloquear el hilo del router
}

// ---------------------------------------------------------------------------
// UART -> RF
// ---------------------------------------------------------------------------

int32_t BridgeModule::runOnce()
{
    if (rxState != 0 && (millis() - lastRxMs) > RX_TIMEOUT_MS) {
        LOG_WARN("BridgeModule: trama UART incompleta, resincronizando");
        rxState = 0;
    }

    while (Serial2.available()) {
        const uint8_t b = Serial2.read();
        lastRxMs = millis();

        switch (rxState) {
        case 0:
            if (b == 0xAA)
                rxState = 1;
            break;
        case 1:
            rxState = (b == 0x55) ? 2 : (b == 0xAA ? 1 : 0);
            break;
        case 2:
            expectedLen = (uint16_t)b << 8;
            rxState = 3;
            break;
        case 3:
            expectedLen |= b;
            if (expectedLen > HDR_LEN && expectedLen <= MAX_FRAME_SIZE - 2) {
                rxPos = 0;
                rxState = 4;
            } else {
                rxState = 0;
            }
            break;
        case 4:
            rxBuffer[rxPos++] = b;
            if (rxPos >= (size_t)expectedLen + 2) {
                const uint16_t rxCrc = ((uint16_t)rxBuffer[expectedLen] << 8) | rxBuffer[expectedLen + 1];
                if (rxCrc == crc16(rxBuffer, expectedLen))
                    processFrame(rxBuffer, expectedLen);
                else
                    LOG_WARN("BridgeModule: trama UART descartada por CRC");
                rxState = 0;
            }
            break;
        }
    }
    return 10;
}

void BridgeModule::processFrame(const uint8_t *buf, uint16_t len)
{
    if (buf[0] != PROTO_VERSION) {
        LOG_WARN("BridgeModule: versión de protocolo %u desconocida", buf[0]);
        return;
    }

    const uint32_t peer = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | buf[5];
    markRemote(peer);

    const int ch = resolveLocalChannel(buf);
    if (ch < 0) {
        char name[CH_NAME_LEN + 1] = {0};
        memcpy(name, buf + 6, CH_NAME_LEN);
        LOG_WARN("BridgeModule: el canal '%s' no existe en esta placa, paquete descartado",
                 (buf[1] & FLAG_PRIMARY) ? "(primario)" : name);
        return;
    }

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p) {
        LOG_ERROR("BridgeModule: sin memoria en packetPool");
        return;
    }

    pb_istream_t stream = pb_istream_from_buffer(buf + HDR_LEN, len - HDR_LEN);
    if (!pb_decode(&stream, meshtastic_MeshPacket_fields, p) || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->from == nodeDB->getNodeNum() || isDuplicate(p->from, p->id)) {
        packetPool.release(p);
        return;
    }

    recordPacket(p->from, p->id);
    markRemote(p->from);

    // Adaptar el paquete a esta red
    p->channel = (uint8_t)ch; // índice LOCAL resuelto por nombre
    p->next_hop = 0;          // sin preferencia de siguiente salto: inundar en esta red
    p->relay_node = 0;
    p->pki_encrypted = false;
    p->public_key.size = 0;
    p->rx_snr = 0;
    p->rx_rssi = 0;
    p->rx_time = 0;
    if (p->to == NODENUM_BROADCAST)
        p->want_ack = false; // el ACK implícito ya lo dio la placa del otro lado

    // Inyectar preservando el 'from' original: se retransmite por RF en esta red
    // y, si es broadcast, se entrega también localmente (NodeDB / USB en el Maestro)
    if (router)
        router->sendLocal(p);
    else
        packetPool.release(p);
}

uint16_t BridgeModule::crc16(const uint8_t *data, size_t len)
{
    // CRC16-CCITT (poly 0x1021, init 0xFFFF)
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

#endif // HAS_UART_BRIDGE
