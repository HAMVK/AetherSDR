#include "RadioModel.h"
#include "core/CommandParser.h"
#include <QDebug>
#include <QRegularExpression>

namespace AetherSDR {

RadioModel::RadioModel(QObject* parent)
    : QObject(parent)
{
    connect(&m_connection, &RadioConnection::statusReceived,
            this, &RadioModel::onStatusReceived);
    connect(&m_connection, &RadioConnection::connected,
            this, &RadioModel::onConnected);
    connect(&m_connection, &RadioConnection::disconnected,
            this, &RadioModel::onDisconnected);
    connect(&m_connection, &RadioConnection::errorOccurred,
            this, &RadioModel::onConnectionError);
    connect(&m_connection, &RadioConnection::versionReceived,
            this, &RadioModel::onVersionReceived);
}

bool RadioModel::isConnected() const
{
    return m_connection.isConnected();
}

SliceModel* RadioModel::slice(int id) const
{
    for (auto* s : m_slices)
        if (s->sliceId() == id) return s;
    return nullptr;
}

// ─── Actions ──────────────────────────────────────────────────────────────────

void RadioModel::connectToRadio(const RadioInfo& info)
{
    m_name  = info.name;
    m_model = info.model;
    m_connection.connectToRadio(info);
}

void RadioModel::disconnectFromRadio()
{
    m_connection.disconnectFromRadio();
}

void RadioModel::setTransmit(bool tx)
{
    m_connection.sendCommand(QString("xmit %1").arg(tx ? 1 : 0));
}

// ─── Connection slots ─────────────────────────────────────────────────────────

void RadioModel::onConnected()
{
    qDebug() << "RadioModel: connected";
    emit connectionStateChanged(true);

    // Register as a GUI client — required before the radio will allow
    // panadapter / slice creation. Must be sent before "slice list".
    m_connection.sendCommand("client gui");

    // Identify this client to the radio (cosmetic; error is non-fatal).
    m_connection.sendCommand("client program AetherSDR");

    // Start the VITA-49 UDP listener.
    // For SmartSDR v1.4 LAN: bind port 4991 and send a UDP registration packet
    // to the radio so it learns our address. "client set udpport" is not supported
    // on v1.4.0.0 (returns error 50001000).
    m_panResized = false;
    if (!m_panStream.isRunning())
        m_panStream.start(&m_connection);

    // Request the current slice list.
    // SmartConnect mode: body = "0 1 2 3" — request each slice's state.
    // Standalone mode:   body = ""        — no slices exist, create one.
    m_connection.sendCommand("slice list",
        [this](int code, const QString& body) {
            if (code != 0) {
                qWarning() << "RadioModel: slice list failed, code" << Qt::hex << code;
                return;
            }
            const QStringList ids = body.trimmed().split(' ', Qt::SkipEmptyParts);
            qDebug() << "RadioModel: slice list ->" << (ids.isEmpty() ? "(empty)" : body);

            if (ids.isEmpty()) {
                // Standalone mode: no slices — create panadapter + slice.
                createDefaultSlice();
            } else {
                // SmartConnect mode: request current state for each existing slice.
                for (const QString& idStr : ids) {
                    bool ok = false;
                    const int id = idStr.toInt(&ok);
                    if (ok) m_connection.sendCommand(QString("slice get %1").arg(id));
                }
            }
        });

    // Flush any slice commands that queued up before we connected.
    for (auto* s : m_slices) {
        for (const QString& cmd : s->drainPendingCommands())
            m_connection.sendCommand(cmd);
    }
}

void RadioModel::onDisconnected()
{
    qDebug() << "RadioModel: disconnected";
    m_panStream.stop();
    m_panId.clear();
    m_panResized = false;
    emit connectionStateChanged(false);
}

void RadioModel::onConnectionError(const QString& msg)
{
    qWarning() << "RadioModel: connection error:" << msg;
    emit connectionError(msg);
    emit connectionStateChanged(false);
}

void RadioModel::onVersionReceived(const QString& v)
{
    m_version = v;
    emit infoChanged();
}

// ─── Status dispatch ──────────────────────────────────────────────────────────
//
// Object strings look like:
//   "radio"           → global radio properties
//   "slice 0"         → slice receiver
//   "panadapter 0"    → panadapter (spectrum)
//   "meter 1"         → meter reading
//   "removed=True"    → object was removed

void RadioModel::onStatusReceived(const QString& object,
                                  const QMap<QString, QString>& kvs)
{
    if (object == "radio") {
        handleRadioStatus(kvs);
        return;
    }

    static const QRegularExpression sliceRe(R"(^slice\s+(\d+)$)");
    const auto sliceMatch = sliceRe.match(object);
    if (sliceMatch.hasMatch()) {
        const bool removed = kvs.value("in_use") == "0";
        handleSliceStatus(sliceMatch.captured(1).toInt(), kvs, removed);
        return;
    }

    if (object.startsWith("meter")) {
        handleMeterStatus(kvs);
        return;
    }

    // "display pan 0x40000000 center=14.1 bandwidth=0.2 ..."
    static const QRegularExpression panRe(R"(^display pan\s+(0x[0-9A-Fa-f]+)$)");
    if (object.startsWith("display pan")) {
        const auto m = panRe.match(object);
        if (m.hasMatch() && m_panId.isEmpty())
            m_panId = m.captured(1);
        handlePanadapterStatus(kvs);
        return;
    }

    // Interlock, ATU, EQ, WAN, transmit etc. are informational — ignore for now.
}

void RadioModel::handleRadioStatus(const QMap<QString, QString>& kvs)
{
    bool changed = false;
    if (kvs.contains("model")) { m_model = kvs["model"]; changed = true; }
    if (changed) emit infoChanged();
}

void RadioModel::handleSliceStatus(int id,
                                    const QMap<QString, QString>& kvs,
                                    bool removed)
{
    SliceModel* s = slice(id);

    if (removed) {
        if (s) {
            m_slices.removeOne(s);
            emit sliceRemoved(id);
            s->deleteLater();
        }
        return;
    }

    if (!s) {
        s = new SliceModel(id, this);
        // Forward slice commands to the radio
        connect(s, &SliceModel::commandReady, this, [this](const QString& cmd){
            m_connection.sendCommand(cmd);
        });
        m_slices.append(s);
        emit sliceAdded(s);
    }

    s->applyStatus(kvs);

    // Send any queued commands (e.g. if GUI changed freq before status arrived)
    if (m_connection.isConnected()) {
        for (const QString& cmd : s->drainPendingCommands())
            m_connection.sendCommand(cmd);
    }
}

void RadioModel::handleMeterStatus(const QMap<QString, QString>& kvs)
{
    // Meter format: "1.num=100 1.nam=FWDPWR 1.low=-150.0 1.hi=20.0 1.desc=Forward Power"
    // In practice the radio sends meter readings as "num" with a float value.
    if (kvs.contains("fwdpwr"))
        m_txPower = kvs["fwdpwr"].toFloat();
    if (kvs.contains("patemp"))
        m_paTemp = kvs["patemp"].toFloat();
    emit metersChanged();
}

void RadioModel::handlePanadapterStatus(const QMap<QString, QString>& kvs)
{
    bool freqChanged  = false;
    bool levelChanged = false;

    if (kvs.contains("center")) {
        m_panCenterMhz = kvs["center"].toDouble();
        freqChanged = true;
    }
    if (kvs.contains("bandwidth")) {
        m_panBandwidthMhz = kvs["bandwidth"].toDouble();
        freqChanged = true;
    }
    if (freqChanged)
        emit panadapterInfoChanged(m_panCenterMhz, m_panBandwidthMhz);

    if (kvs.contains("min_dbm") || kvs.contains("max_dbm")) {
        const float minDbm = kvs.value("min_dbm", "-130").toFloat();
        const float maxDbm = kvs.value("max_dbm", "-20").toFloat();
        emit panadapterLevelChanged(minDbm, maxDbm);
        levelChanged = true;
    }
    Q_UNUSED(levelChanged)

    // On the first full panadapter status we know the pan ID.  Send a resize
    // command to request a higher-resolution FFT (radio default is only 50 bins).
    if (!m_panResized && !m_panId.isEmpty() && kvs.contains("x_pixels")
        && m_connection.isConnected())
    {
        m_panResized = true;
        // Request 1024 bins at 25 fps — gives ~190 Hz/bin at 200 kHz bandwidth.
        const QString cmd = QString("display pan set %1 x_pixels=1024 y_pixels=384 fps=25")
                                .arg(m_panId);
        qDebug() << "RadioModel: requesting pan resize ->" << cmd;
        m_connection.sendCommand(cmd);
    }
}

// ─── Standalone mode: create panadapter + slice ───────────────────────────────
//
// SmartSDR API 1.4.0.0 standalone flow:
//   1. "panadapter create"
//      → R|0|pan=0x40000000         (KV response; key is "pan")
//   2. "slice create pan=0x40000000 freq=14.225000 antenna=ANT1 mode=USB"
//      → R|0|<slice_index>          (decimal, e.g. "0")
//   3. Radio emits S messages for the new panadapter and slice.
//
// Note: "display panafall create" (v2+ syntax) returns 0x50000016 on this firmware.

void RadioModel::createDefaultSlice(const QString& freqMhz,
                                     const QString& mode,
                                     const QString& antenna)
{
    qDebug() << "RadioModel: standalone mode — creating panadapter + slice"
             << freqMhz << mode << antenna;

    m_connection.sendCommand("panadapter create",
        [this, freqMhz, mode, antenna](int code, const QString& body) {
            if (code != 0) {
                qWarning() << "RadioModel: panadapter create failed, code" << Qt::hex << code
                           << "body:" << body;
                return;
            }

            qDebug() << "RadioModel: panadapter create response body:" << body;

            // Response body may be a bare hex ID ("0x40000000") or KV ("pan=0x40000000").
            // Parse KVs first; fall back to treating the whole body as the ID.
            QString panId;
            const QMap<QString, QString> kvs = CommandParser::parseKVs(body);
            if (kvs.contains("pan")) {
                panId = kvs["pan"];
            } else if (kvs.contains("id")) {
                panId = kvs["id"];
            } else {
                panId = body.trimmed();
            }

            qDebug() << "RadioModel: panadapter created, pan_id =" << panId;

            if (panId.isEmpty()) {
                qWarning() << "RadioModel: panadapter create returned empty pan_id";
                return;
            }

            const QString sliceCmd =
                QString("slice create pan=%1 freq=%2 antenna=%3 mode=%4")
                    .arg(panId, freqMhz, antenna, mode);

            m_connection.sendCommand(sliceCmd,
                [panId](int code2, const QString& body2) {
                    if (code2 != 0) {
                        qWarning() << "RadioModel: slice create failed, code"
                                   << Qt::hex << code2 << "body:" << body2;
                    } else {
                        qDebug() << "RadioModel: slice created, index =" << body2;
                        // Radio now emits S|slice N ... status messages;
                        // handleSliceStatus() picks them up automatically.
                    }
                });
        });
}

} // namespace AetherSDR
