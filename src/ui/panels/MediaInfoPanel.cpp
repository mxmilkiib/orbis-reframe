#include "MediaInfoPanel.h"

#include <InsvMediaInfo.h>

#include <QDateTime>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>

#include <cmath>

namespace {

QString bitRate(int64_t bps)
{
    if (bps <= 0)
        return {};
    if (bps >= 1'000'000)
        return QStringLiteral("%1 Mb/s").arg(bps / 1e6, 0, 'f', bps >= 10'000'000 ? 0 : 1);
    return QStringLiteral("%1 kb/s").arg(bps / 1000);
}

QString fps(double f)
{
    // 29.97 / 59.94 / 119.88 keep their decimals; whole rates don't get ".00".
    const double rounded = std::round(f);
    if (std::abs(f - rounded) < 0.005)
        return QString::number(int(rounded));
    return QString::number(f, 'f', 2);
}

QString duration(int64_t ms)
{
    const int64_t s = ms / 1000;
    const QString mmss = QStringLiteral("%1:%2.%3")
                             .arg(s / 60)
                             .arg(s % 60, 2, 10, QLatin1Char('0'))
                             .arg(ms % 1000, 3, 10, QLatin1Char('0'));
    return s >= 3600 ? QStringLiteral("%1:%2").arg(s / 3600).arg(mmss.rightJustified(9, u'0'))
                     : mmss;
}

QString upperCodec(const std::string &codec, const std::string &profile)
{
    QString s = QString::fromStdString(codec).toUpper();
    if (!profile.empty())
        s += QLatin1Char(' ') + QString::fromStdString(profile);
    return s;
}

} // namespace

MediaInfoPanel::MediaInfoPanel(QWidget *parent)
    : SettingsPanel(tr("Media information"), parent)
{
    setNoClip();
}

void MediaInfoPanel::clearRows()
{
    while (form()->rowCount() > 0)
        form()->removeRow(0);
}

void MediaInfoPanel::addSection(const QString &title)
{
    auto *label = new QLabel(title, this);
    label->setObjectName(QStringLiteral("mediaInfoSection"));
    form()->addRow(label);
}

void MediaInfoPanel::addValue(const QString &label, const QString &value, const QString &toolTip)
{
    if (value.isEmpty())
        return;
    auto *v = new QLabel(value, this);
    v->setObjectName(QStringLiteral("mediaInfoValue"));
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setToolTip(toolTip);
    form()->addRow(label, v);
}

void MediaInfoPanel::addWideValue(const QString &value, const QString &toolTip)
{
    auto *v = new QLabel(value, this);
    v->setObjectName(QStringLiteral("mediaInfoValue"));
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setToolTip(toolTip);
    form()->addRow(v);
}

void MediaInfoPanel::setInfo(const insv::MediaInfo &m)
{
    clearRows();
    setBodyEnabled(true);
    setHint(tr("The source file of the clip under the playhead."));
    const QLocale locale;

    const QFileInfo fi(QString::fromStdString(m.path));
    addSection(tr("File"));
    addWideValue(fi.fileName(), fi.absoluteFilePath());
    addValue(tr("Size"), locale.formattedDataSize(m.fileBytes, 2, QLocale::DataSizeSIFormat));
    addValue(tr("Duration"), duration(m.durationMs));
    const QDateTime created =
        QDateTime::fromString(QString::fromStdString(m.creationTime), Qt::ISODateWithMs);
    if (created.isValid())
        addValue(tr("Recorded"), locale.toString(created.toLocalTime(), QLocale::ShortFormat));
    addValue(tr("Bitrate"), bitRate(m.totalBitRate));
    if (fi.suffix().compare(QLatin1String("insv"), Qt::CaseInsensitive) == 0) // a 360 clip's
        addValue(tr("Preview file"), m.proxyPath.empty() ? tr("Not found") : tr("Found"),
                 m.proxyPath.empty()
                     ? tr("The camera's low-res preview file (.lrv), expected next to the .insv.")
                     : QString::fromStdString(m.proxyPath));

    addSection(tr("Video"));
    if (m.video.empty()) {
        addValue(tr("Streams"), tr("None"));
    } else {
        const insv::MediaVideoStream &v = m.video.front();
        const QString mode = QString::fromStdString(m.captureMode());
        if (!mode.isEmpty()) {
            addValue(tr("Mode"), tr("%1 360°").arg(mode),
                     tr("Read from the size of the lens pictures. 5.7K+ records pictures the "
                        "same size as 5.7K, so it shows as 5.7K."));
            addValue(tr("Lenses"), tr("2 × %1×%2 fisheye").arg(v.width).arg(v.height));
            addValue(tr("Sphere"), QStringLiteral("%1×%2").arg(2 * v.width).arg(v.width),
                     tr("The whole 360° picture once the two are stitched together, as a flat "
                        "world-map image."));
        } else {
            addValue(tr("Streams"), QString::number(m.video.size()));
            addValue(tr("Resolution"), QStringLiteral("%1×%2").arg(v.width).arg(v.height));
        }
        addValue(tr("Frame rate"), tr("%1 fps").arg(fps(v.fps)));
        if (v.frameCount > 0)
            addValue(tr("Frames"), locale.toString(qlonglong(v.frameCount)));
        addValue(tr("Codec"), upperCodec(v.codec, v.profile));
        QString format = v.bitDepth > 0 ? tr("%1-bit").arg(v.bitDepth) : QString();
        if (!v.chroma.empty())
            format += QLatin1Char(' ') + QString::fromStdString(v.chroma);
        addValue(tr("Format"), format.trimmed(), QString::fromStdString(v.pixelFormat));
        QString colour = v.colorSpace == "bt709" ? QStringLiteral("BT.709")
                                                 : QString::fromStdString(v.colorSpace).toUpper();
        if (!colour.isEmpty())
            colour += QStringLiteral(", ");
        colour += v.fullRange ? tr("full range") : tr("limited range");
        addValue(tr("Colour"), colour);
        if (const QString log = QString::fromStdString(m.logProfile()); !log.isEmpty())
            addValue(tr("Colour profile"), log,
                     tr("A log profile: the picture looks flat and grey until the LUT the "
                        "camera maker publishes for %1 is applied (Colour section).")
                         .arg(log));
        else if (m.colorProfile == "standard")
            addValue(tr("Colour profile"), tr("Standard"));
        else if (!m.colorProfile.empty())
            addValue(tr("Colour profile"), QString::fromStdString(m.colorProfile));
        const QString br = bitRate(v.bitRate);
        addValue(tr("Bitrate"), m.video.size() > 1 && !br.isEmpty() ? tr("%1 per lens").arg(br) : br);
    }

    addSection(tr("Audio"));
    if (m.audio.empty())
        addValue(tr("Tracks"), tr("None"));
    // Two short lines per track: codec + channels, then rate + bitrate.
    for (size_t i = 0; i < m.audio.size(); ++i) {
        const insv::MediaAudioStream &a = m.audio[i];
        QString first = upperCodec(a.codec, a.profile);
        if (!a.channelLayout.empty())
            first += QLatin1Char(' ') + QString::fromStdString(a.channelLayout);
        else if (a.channels > 0)
            first += QLatin1Char(' ') + tr("%n ch", nullptr, a.channels);
        QStringList second;
        if (a.sampleRate > 0)
            second << tr("%1 kHz").arg(a.sampleRate / 1000.0, 0, 'g', 3);
        if (a.bitRate > 0)
            second << bitRate(a.bitRate);
        addValue(tr("Track %1").arg(i + 1),
                 first + QLatin1Char('\n') + second.join(QStringLiteral(", ")));
    }

    if (!m.cameraModel.empty() || !m.serialNumber.empty()) {
        addSection(tr("Camera"));
        addValue(tr("Model"), QString::fromStdString(m.cameraModel));
        addValue(tr("Serial"), QString::fromStdString(m.serialNumber));
        addValue(tr("Firmware"), QString::fromStdString(m.firmware));
        addValue(tr("Motion data"),
                 m.imuRateHz > 0 ? tr("~%1 Hz").arg(qRound(m.imuRateHz)) : tr("Not recorded"),
                 tr("How often the camera recorded its movement, which stabilization works "
                    "from."));
        QString gps = tr("Not recorded");
        if (m.gpsRecords > 0) {
            gps = m.gpsRateHz > 0 ? tr("~%1 Hz").arg(qRound(m.gpsRateHz)) : tr("Recorded");
            if (m.gpsFixes == 0) {
                gps += QLatin1Char('\n') + tr("no position fix");
            } else if (m.gpsTrackSec > 0 && m.durationMs > 0) {
                // The track can drop out, or stop long before the recording does.
                const double share = m.gpsTrackSec * 1000.0 / m.durationMs;
                if (share < 0.9)
                    gps += QLatin1Char('\n')
                           + tr("%1% of the recording").arg(qMax(1, qRound(share * 100)));
            }
        }
        addValue(tr("GPS"), gps,
                 tr("Whether a GPS source, such as the GPS remote, recorded a track. "
                    "The positions aren't shown here."));
        if (m.rollingShutterMs > 0)
            addValue(tr("Sensor readout"), tr("%1 ms").arg(m.rollingShutterMs, 0, 'f', 1),
                     tr("How long the sensor takes to read one picture from top to bottom (the "
                        "rolling shutter)."));
    }
}

void MediaInfoPanel::setError(const QString &path, const QString &error)
{
    clearRows();
    setBodyEnabled(true);
    setHint({});
    addSection(tr("File"));
    addWideValue(QFileInfo(path).fileName(), path);
    addValue(tr("Error"), error);
}

void MediaInfoPanel::setNoClip()
{
    clearRows();
    setHint(tr("No clip under the playhead."));
    setBodyEnabled(false);
}
