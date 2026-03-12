#include "SliceModel.h"
#include <QDebug>

namespace AetherSDR {

SliceModel::SliceModel(int id, QObject* parent)
    : QObject(parent), m_id(id)
{}

// ─── Setters ──────────────────────────────────────────────────────────────────

void SliceModel::setFrequency(double mhz)
{
    if (qFuzzyCompare(m_frequency, mhz)) return;
    m_frequency = mhz;
    // SmartSDR command: "slice tune <id> <freq_in_MHz>"
    m_pendingCommands << QString("slice tune %1 %2").arg(m_id).arg(mhz, 0, 'f', 6);
    emit frequencyChanged(mhz);
}

void SliceModel::setMode(const QString& mode)
{
    if (m_mode == mode) return;
    m_mode = mode;
    m_pendingCommands << QString("slice set %1 mode=%2").arg(m_id).arg(mode);
    emit modeChanged(mode);
}

void SliceModel::setFilterWidth(int low, int high)
{
    m_filterLow  = low;
    m_filterHigh = high;
    m_pendingCommands << QString("slice set %1 filter_lo=%2 filter_hi=%3")
                             .arg(m_id).arg(low).arg(high);
    emit filterChanged(low, high);
}

void SliceModel::setAudioGain(float gain)
{
    m_audioGain = qBound(0.0f, gain, 100.0f);
    m_pendingCommands << QString("audio gain 0x%1 slice %2 %3")
                             .arg(0, 8, 16, QChar('0'))  // stream ID placeholder
                             .arg(m_id)
                             .arg(static_cast<int>(m_audioGain));
}

void SliceModel::setRfGain(float gain)
{
    m_rfGain = gain;
    m_pendingCommands << QString("slice set %1 rf_gain=%2").arg(m_id).arg(static_cast<int>(gain));
}

// ─── Status updates from radio ────────────────────────────────────────────────

void SliceModel::applyStatus(const QMap<QString, QString>& kvs)
{
    bool freqChanged   = false;
    bool modeChanged_  = false;
    bool filterChanged_= false;

    // The radio sends the frequency as "RF_frequency" in status messages.
    if (kvs.contains("RF_frequency")) {
        const double f = kvs["RF_frequency"].toDouble();
        if (!qFuzzyCompare(m_frequency, f)) {
            m_frequency = f;
            freqChanged = true;
        }
    }
    if (kvs.contains("mode")) {
        const QString m = kvs["mode"];
        if (m_mode != m) {
            m_mode = m;
            modeChanged_ = true;
        }
    }
    if (kvs.contains("filter_lo") || kvs.contains("filter_hi")) {
        m_filterLow  = kvs.value("filter_lo",  QString::number(m_filterLow)).toInt();
        m_filterHigh = kvs.value("filter_hi", QString::number(m_filterHigh)).toInt();
        filterChanged_ = true;
    }
    if (kvs.contains("active"))
        m_active = kvs["active"] == "1";
    if (kvs.contains("tx"))
        m_txSlice = kvs["tx"] == "1";
    if (kvs.contains("rf_gain"))
        m_rfGain = kvs["rf_gain"].toFloat();
    if (kvs.contains("audio_gain"))
        m_audioGain = kvs["audio_gain"].toFloat();

    if (freqChanged)    emit frequencyChanged(m_frequency);
    if (modeChanged_)   emit modeChanged(m_mode);
    if (filterChanged_) emit filterChanged(m_filterLow, m_filterHigh);
}

QStringList SliceModel::drainPendingCommands()
{
    QStringList cmds;
    cmds.swap(m_pendingCommands);
    return cmds;
}

} // namespace AetherSDR
