// MainWindow's Colour tab: the clip's grade into the panel and the viewport,
// copy / paste between clips, and its LUT -- loading a .cube, copied into
// the project's luts/ folder (projectfolder::importLut).

#include "MainWindow.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include "TimelineWidget.h"
#include "panels/ColourPanel.h"
#include "../core/ClipItem.h"
#include "../core/ColorLut.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h"

void MainWindow::connectColour()
{
    // Colour panel: an edit writes the grade onto the clip under the playhead;
    // clipColorChanged pushes it straight into the viewport if it's the
    // clip currently showing -- no re-stitch, applied at reprojection time.
    connect(m_colourPanel, &ColourPanel::gradeEdited, this, [this](const ColorGrade &g) {
        if (const int id = editClipId())
            m_timeline->setClipColor(id, g);
    });
    // Copy / paste a grade between clips: the panel's buttons act on its clip,
    // the timeline's right-click menu on the clicked one (or every clip --
    // one undo step, the edits land in one burst).
    const auto copyGrade = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (!c)
            return;
        m_copiedGrade = c->color;
        m_colourPanel->setCanPaste(true);
        m_timelineWidget->setCanPasteGrade(true);
    };
    const auto pasteGrade = [this](int clipId) {
        if (m_copiedGrade && clipId)
            m_timeline->setClipColor(clipId, *m_copiedGrade);
    };
    connect(m_colourPanel, &ColourPanel::copyRequested, this,
            [this, copyGrade] { copyGrade(editClipId()); });
    connect(m_colourPanel, &ColourPanel::pasteRequested, this,
            [this, pasteGrade] { pasteGrade(editClipId()); });
    connect(m_timelineWidget, &TimelineWidget::gradeCopyRequested, this, copyGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteRequested, this, pasteGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteAllRequested, this, [this] {
        if (!m_copiedGrade)
            return;
        for (int i = 0; i < m_timeline->count(); ++i)
            m_timeline->setClipColor(m_timeline->clipAt(i).id, *m_copiedGrade);
    });
    connect(m_timeline, &Timeline::clipColorChanged, this, [this](int id, ColorGrade grade) {
        if (const ClipItem *c = m_timeline->clipById(id); c && c->flat)
            pushInsetsToController(); // its picture is a layer, graded there
        if (id == m_clipId) // on screen
            applyColorGrade(grade);
        if (id == editClipId()) // in the panel
            syncColourPanel(grade);
    });
    connect(m_colourPanel, &ColourPanel::loadLutRequested, this, &MainWindow::loadLut);
}

void MainWindow::applyColorGrade(const ColorGrade &grade)
{
    const color::Grade g = grade.render();
    m_viewport->SetColorGrade(&g);
}

QStringList MainWindow::availableLuts() const
{
    QStringList luts;
    const QString dir = projectfolder::lutsDir(m_project->filePath());
    if (!dir.isEmpty()) {
        const QFileInfoList files =
            QDir(dir).entryInfoList({QStringLiteral("*.cube"), QStringLiteral("*.CUBE")},
                                    QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &f : files)
            luts << f.absoluteFilePath();
    }
    for (int i = 0; i < m_timeline->count(); ++i) {
        const QString &lut = m_timeline->clipAt(i).color.lut;
        if (!lut.isEmpty() && !luts.contains(lut))
            luts << lut;
    }
    return luts;
}

void MainWindow::syncColourPanel(const ColorGrade &grade)
{
    m_colourPanel->setLuts(availableLuts());
    m_colourPanel->setState(grade);
    QString problem;
    if (!grade.lut.isEmpty()) {
        std::string why;
        if (!color::cachedCube(grade.lut.toStdString(), &why))
            problem = tr("%1 isn't applied: %2.")
                          .arg(QFileInfo(grade.lut).fileName(), QString::fromStdString(why));
    }
    m_colourPanel->setLutProblem(problem);

    // Filmed in log and no LUT yet: point at the picker.
    QString wanted;
    const ClipItem *c = m_timeline->clipById(editClipId());
    if (c && !c->flat && grade.lut.isEmpty() && !c->mediaMissing) {
        const insv::MediaInfo *info = mediaInfoFor(c->sourcePath);
        if (const QString log = info ? QString::fromStdString(info->logProfile()) : QString();
            !log.isEmpty())
            wanted = tr("This clip was filmed in %1, a log profile, so it looks flat and grey. "
                        "Load the LUT the camera maker publishes for %1 to give it normal "
                        "colours.")
                         .arg(log);
    }
    m_colourPanel->setLutWanted(wanted);
}

void MainWindow::loadLut()
{
    const int id = editClipId();
    const ClipItem *c = m_timeline->clipById(id);
    if (!c)
        return;
    const QString picked = QFileDialog::getOpenFileName(
        this, tr("Load LUT"), m_lastLutDir.isEmpty() ? QDir::homePath() : m_lastLutDir,
        tr("3D LUTs (*.cube);;All files (*)"));
    if (picked.isEmpty())
        return;
    m_lastLutDir = QFileInfo(picked).absolutePath();
    std::string why;
    if (!color::loadCube(picked.toStdString(), &why)) {
        QMessageBox::warning(this, tr("Can't use this LUT"),
                             tr("%1 can't be used as a LUT: %2.")
                                 .arg(QFileInfo(picked).fileName(), QString::fromStdString(why)));
        return;
    }
    QString error;
    const QString lut = projectfolder::importLut(m_project->filePath(), picked, &error);
    if (lut.isEmpty()) {
        QMessageBox::warning(this, tr("Can't use this LUT"), error);
        return;
    }
    ColorGrade g = c->color;
    g.lut = lut;
    m_timeline->setClipColor(id, g);
    syncColourPanel(m_timeline->clipById(id)->color); // the new LUT in the list
}
