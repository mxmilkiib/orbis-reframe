#include "SystemCheck.h"

#include "../export/ReframeFlatten.h"
#include "../render/EquirectViewportWindow.h"

#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include <SubjectTracker.h> // track::modelsAvailable
#endif
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include <PlateScanner.h> // privacy::modelAvailable
#endif

#include <QCheckBox>
#include <QCoreApplication>
#include <QMessageBox>
#include <QSettings>
#include <QStringList>

namespace syscheck {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("syscheck", text);
}

QSettings settings()
{
    // Not the default QSettings(): that needs an organization name, which
    // would also move the cache folder (~/.cache/orbis-reframe).
    return QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
}

const QString kHiddenKey = QStringLiteral("startupWarnings/hidden");

} // namespace

bool hwEncoderAvailable()
{
    return exportpipe::hwEncoderAvailable();
}

QString hwEncoderMissingText()
{
    return tr("Exporting needs a hardware video encoder (NVIDIA NVENC, Vulkan "
              "video or VAAPI), and none was found. Editing and playback work, "
              "but File ▸ Export flat video… doesn't.\n\nWith an NVIDIA card, "
              "install the encoder library: on Debian, sudo apt install "
              "libnvidia-encode1 (NVIDIA's own driver installer includes it).");
}

bool gpuVideoDecode(EquirectViewportWindow *viewport)
{
    if (!viewport || !viewport->physicalDeviceProperties())
        return false;
    return viewport->supportedDeviceExtensions().contains("VK_KHR_video_decode_h265");
}

QString gpuDecodeMissingText()
{
    return tr("Your graphics card or driver can't decode video on the GPU (Vulkan video), so "
              "orbis-reframe decodes it on the processor instead. 5.7K and 8K clips will play "
              "slowly and may stutter; exports take longer.\n\nAn NVIDIA card from the GTX 16 / "
              "RTX 20 series onwards with a current driver decodes on the GPU.");
}

QString trackUnavailableReason()
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    if (track::modelsAvailable(track::TrackerParams{}))
        return {};
    return tr("Track subject needs the VitTrack model, which is missing from %1. Reinstall "
              "orbis-reframe to get it back.")
        .arg(QString::fromStdString(track::defaultModelDir()));
#else
    return tr("This build of orbis-reframe can't track (it was built without OpenCV's "
              "tracking modules).");
#endif
}

QString plateScanUnavailableReason()
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const std::string param = privacy::defaultModelParam();
    if (privacy::modelAvailable(param))
        return {};
    return tr("Scanning needs the plate model, which is missing (%1). Reinstall orbis-reframe "
              "to get it back.")
        .arg(QString::fromStdString(param));
#else
    return tr("This build can't scan (it was built without ncnn).");
#endif
}

void warnAtStartup(QWidget *parent, EquirectViewportWindow *viewport)
{
    if (!viewport || !viewport->physicalDeviceProperties())
        return;
    const QStringList hidden = settings().value(kHiddenKey).toStringList();
    QStringList ids, texts;
    if (!gpuVideoDecode(viewport) && !hidden.contains(QStringLiteral("gpu-decode"))) {
        ids << QStringLiteral("gpu-decode");
        texts << gpuDecodeMissingText();
    }
    if (!hwEncoderAvailable() && !hidden.contains(QStringLiteral("nvenc"))) {
        ids << QStringLiteral("nvenc");
        texts << hwEncoderMissingText();
    }
    if (ids.isEmpty())
        return;

    QMessageBox box(QMessageBox::Warning, tr("orbis-reframe"),
                    texts.join(QStringLiteral("\n\n")), QMessageBox::Ok, parent);
    box.setCheckBox(new QCheckBox(tr("Don't show this again")));
    box.exec();
    if (box.checkBox()->isChecked()) {
        QStringList now = hidden;
        now << ids;
        settings().setValue(kHiddenKey, now);
    }
}

} // namespace syscheck
