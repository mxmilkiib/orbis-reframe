#include "AboutDialog.h"

#include "SystemCheck.h"

#include "../core/DataPaths.h"
#include "../core/Version.h"
#include "../export/ReframeFlatten.h"
#include "../render/EquirectViewportWindow.h"

#include <NoiseFilter.h> // insv::voiceModesReady

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>
#include <QtVersion>

namespace {

QString yesNo(bool b)
{
    return b ? QStringLiteral("yes") : QStringLiteral("no");
}

// NVIDIA packs its driver version differently from the Vulkan convention.
QString driverVersion(const VkPhysicalDeviceProperties &p)
{
    const uint32_t v = p.driverVersion;
    if (p.vendorID == 0x10DE)
        return QStringLiteral("%1.%2.%3")
            .arg((v >> 22) & 0x3ff)
            .arg((v >> 14) & 0xff, 2, 10, QLatin1Char('0'))
            .arg((v >> 6) & 0xff, 2, 10, QLatin1Char('0')); // 595.99.02, as nvidia-smi
    return QStringLiteral("%1.%2.%3").arg(VK_API_VERSION_MAJOR(v)).arg(VK_API_VERSION_MINOR(v)).arg(VK_API_VERSION_PATCH(v));
}

QString diagnostics(EquirectViewportWindow *viewport)
{
    QStringList out;
    out << QStringLiteral("orbis-reframe %1").arg(QString::fromUtf8(core::version::string()));
    out << QStringLiteral("System: %1, kernel %2").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    out << QStringLiteral("Qt: %1 (%2)").arg(QString::fromLatin1(qVersion()), QGuiApplication::platformName());

    const VkPhysicalDeviceProperties *p = viewport ? viewport->physicalDeviceProperties() : nullptr;
    if (p) {
        out << QStringLiteral("GPU: %1, driver %2, Vulkan %3.%4.%5")
                   .arg(QString::fromUtf8(p->deviceName), driverVersion(*p))
                   .arg(VK_API_VERSION_MAJOR(p->apiVersion))
                   .arg(VK_API_VERSION_MINOR(p->apiVersion))
                   .arg(VK_API_VERSION_PATCH(p->apiVersion));
        const auto ext = viewport->supportedDeviceExtensions();
        out << QStringLiteral("Vulkan video decode: H.264 %1, H.265 %2")
                   .arg(yesNo(ext.contains("VK_KHR_video_decode_h264")),
                        yesNo(ext.contains("VK_KHR_video_decode_h265")));
    } else {
        out << QStringLiteral("GPU: unknown (the viewport hasn't started Vulkan)");
    }
    QStringList encoders;
    if (const char *n = exportpipe::hwEncoderName(true))
        encoders << QString::fromLatin1(n);
    if (const char *n = exportpipe::hwEncoderName(false))
        encoders << QString::fromLatin1(n);
    out << QStringLiteral("Video encoder (export): %1")
               .arg(encoders.isEmpty() ? QStringLiteral("none found")
                                       : encoders.join(QLatin1String(", ")));
    // Why the encoders tried before (or instead of) those can't be used.
    for (const bool h265 : {true, false})
        if (const std::string why = exportpipe::hwEncoderReport(h265); !why.empty())
            out << QStringLiteral("  %1 not usable: %2")
                       .arg(h265 ? QStringLiteral("H.265") : QStringLiteral("H.264"),
                            QString::fromStdString(why));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    out << QStringLiteral("Audio playback: built in");
#else
    out << QStringLiteral("Audio playback: not in this build");
#endif

    const QString cantTrack = syscheck::trackUnavailableReason();
    out << QStringLiteral("Track subject: %1").arg(cantTrack.isEmpty() ? QStringLiteral("ready") : cantTrack);
    const QString cantScan = syscheck::plateScanUnavailableReason();
    out << QStringLiteral("Plate scan: %1").arg(cantScan.isEmpty() ? QStringLiteral("ready") : cantScan);
    std::string why;
    const bool voice = insv::voiceModesReady(&why);
    out << QStringLiteral("Voice noise modes: %1")
               .arg(voice ? QStringLiteral("ready") : QString::fromStdString(why));
    out << QStringLiteral("Models: %1").arg(QString::fromStdString(core::paths::modelDir({})));
    return out.join(QLatin1Char('\n'));
}

} // namespace

void showAboutDialog(QWidget *parent, EquirectViewportWindow *viewport)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("About orbis-reframe"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *about = new QLabel(&dlg);
    about->setTextFormat(Qt::RichText);
    about->setWordWrap(true);
    about->setOpenExternalLinks(true);
    about->setText(QObject::tr(
        "<h3>orbis-reframe %1</h3>"
        "<p>Video editor for Insta360 X5 360&deg; footage: stitching, gyro stabilization "
        "and reframing.</p>"
        "<p>Copyright &copy; 2026 Markku Kokko<br>"
        "Licensed under the GNU General Public License v3.0 or later. The .insv library "
        "(src/insv) is licensed under MIT OR Apache-2.0.</p>"
        "<p>Built with Qt, Vulkan, FFmpeg, OpenCV, ncnn, ONNX Runtime and nlohmann/json; "
        "DeepFilterNet 3, VitTrack and a YOLOv9 plate model; Lucide icons. Their licenses "
        "are listed in THIRD_PARTY.md, installed with the documentation "
        "(share/doc/orbis-reframe).</p>"
        "<p><a href=\"https://github.com/Schabo/orbis-reframe\">Source code and releases</a> "
        "&middot; <a href=\"https://github.com/Schabo/orbis-reframe/issues\">Report a problem</a></p>"
        "<p>Not affiliated with or endorsed by Insta360.</p>")
                       .arg(QString::fromUtf8(core::version::string()).toHtmlEscaped()));
    // A word-wrapped label doesn't count its wrapped height towards the
    // dialog's minimum size, so a dialog narrower or shorter than its first
    // layout cut the last lines off. Reserve the height the text needs at the
    // narrowest width the dialog allows (the diagnostics box's), and make that
    // the window's minimum.
    constexpr int kMinTextWidth = 560;
    about->ensurePolished();
    about->setMinimumWidth(kMinTextWidth);
    about->setMinimumHeight(about->heightForWidth(kMinTextWidth));
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->addWidget(about);
    layout->addSpacing(layout->spacing());

    layout->addWidget(new QLabel(QObject::tr("Diagnostics (include these in bug reports):"), &dlg));
    auto *diag = new QPlainTextEdit(&dlg);
    diag->setReadOnly(true);
    diag->setFont(QFont(QStringLiteral("monospace")));
    QApplication::setOverrideCursor(Qt::WaitCursor); // the voice check may load ONNX Runtime
    diag->setPlainText(diagnostics(viewport));
    QApplication::restoreOverrideCursor();
    diag->setMinimumSize(kMinTextWidth, 200);
    layout->addWidget(diag);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(QObject::tr("Copy diagnostics"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dlg,
                     [diag] { QGuiApplication::clipboard()->setText(diag->toPlainText()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    dlg.exec();
}
