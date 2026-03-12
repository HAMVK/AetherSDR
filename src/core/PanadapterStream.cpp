#include "PanadapterStream.h"
#include "RadioConnection.h"

#include <QNetworkDatagram>
#include <QHostAddress>
#include <QtEndian>
#include <QDebug>

namespace AetherSDR {

// ─── VITA-49 header layout (28 bytes, big-endian) ─────────────────────────────
// Word 0 (bytes  0- 3): Packet header (type, flags, packet count, size in words)
// Word 1 (bytes  4- 7): Stream ID
// Word 2 (bytes  8-11): Class ID OUI / info type (upper)
// Word 3 (bytes 12-15): Class ID (lower)
// Word 4 (bytes 16-19): Integer timestamp (seconds, UTC)
// Word 5 (bytes 20-23): Fractional timestamp (upper)
// Word 6 (bytes 24-27): Fractional timestamp (lower)
// Byte 28+            : Payload — int16_t FFT bins, big-endian
//
// Stream ID patterns (FLEX radio):
//   0x4xxxxxxx — panadapter FFT frames
//   0x42xxxxxx — waterfall pixel rows
//   0x0001xxxx — audio (remote audio)
//
// UDP delivery (SmartSDR v1.x LAN):
//   "client set udpport" is NOT supported on firmware v1.4.0.0 (returns 50001000).
//   Instead: bind port 4991 (the well-known VITA-49 receive port for local clients)
//   and send a one-byte UDP registration packet to the radio at port 4992 so the
//   radio learns our IP:port from the datagram source address.

PanadapterStream::PanadapterStream(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead,
            this, &PanadapterStream::onDatagramReady);
}

bool PanadapterStream::isRunning() const
{
    return m_socket.state() == QAbstractSocket::BoundState;
}

bool PanadapterStream::start(RadioConnection* conn)
{
    if (isRunning()) return true;

    // SmartSDR v1.x LAN clients receive VITA-49 data on port 4991.
    // Try the well-known port first; fall back to OS-assigned if already in use.
    const quint16 preferredPort = 4991;
    bool bound = m_socket.bind(QHostAddress::AnyIPv4, preferredPort,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        qDebug() << "PanadapterStream: port 4991 busy, trying OS-assigned port";
        bound = m_socket.bind(QHostAddress::AnyIPv4, 0);
    }
    if (!bound) {
        qWarning() << "PanadapterStream: failed to bind UDP socket:"
                   << m_socket.errorString();
        return false;
    }

    m_localPort = m_socket.localPort();
    qDebug() << "PanadapterStream: bound to UDP port" << m_localPort;

    // Send a one-byte UDP registration packet to the radio.
    // The radio (firmware v1.4) learns our IP:port from the source address of this
    // datagram and directs its VITA-49 stream to us.
    // "client set udpport" returns error 50001000 on v1.4 and is not used.
    const QHostAddress radioAddr = conn->radioAddress();
    if (!radioAddr.isNull()) {
        const QByteArray regPacket(4, '\0');   // minimal payload
        qint64 sent = m_socket.writeDatagram(regPacket, radioAddr, 4992);
        qDebug() << "PanadapterStream: sent UDP registration to"
                 << radioAddr.toString() << ":4992, bytes sent =" << sent;
    } else {
        qWarning() << "PanadapterStream: radio address unknown; UDP stream may not arrive";
    }

    return true;
}

void PanadapterStream::stop()
{
    m_socket.close();
    m_localPort = 0;
}

// ─── Datagram reception ───────────────────────────────────────────────────────

void PanadapterStream::onDatagramReady()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket.receiveDatagram();
        if (!dg.isNull())
            processDatagram(dg.data());
    }
}

void PanadapterStream::processDatagram(const QByteArray& data)
{
    if (data.size() < VITA49_HEADER_BYTES + 2) {
        qDebug() << "PanadapterStream: short datagram, size =" << data.size();
        return;
    }

    const auto* raw = reinterpret_cast<const uchar*>(data.constData());

    // Read stream ID (word 1, bytes 4-7, big-endian).
    const quint32 streamId = qFromBigEndian<quint32>(raw + 4);

    qDebug() << "PanadapterStream: datagram" << data.size() << "bytes, streamId ="
             << QString("0x%1").arg(streamId, 8, 16, QChar('0'));

    // Accept panadapter frames (stream ID 0x40000000–0x40FFFFFF).
    // Skip waterfall (0x42000000), audio (0x0001xxxx), and others.
    if ((streamId & 0xFF000000u) != 0x40000000u) return;

    // Payload: signed 16-bit FFT bins, big-endian.
    const int payloadBytes = data.size() - VITA49_HEADER_BYTES;
    const int binCount     = payloadBytes / 2;
    if (binCount <= 0) return;

    const uchar* payload = raw + VITA49_HEADER_BYTES;

    QVector<float> bins(binCount);
    for (int i = 0; i < binCount; ++i) {
        const qint16 sample = qFromBigEndian<qint16>(payload + i * 2);
        // Scale: raw value / 128.0 gives dBm (FLEX VITA-49 convention).
        bins[i] = static_cast<float>(sample) / 128.0f;
    }

    emit spectrumReady(bins);
}

} // namespace AetherSDR
