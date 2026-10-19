#include "ProjectFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>

#include <cstdio>

#include "NewProjectDialog.h"
#include "PlateScanCache.h"
#include "../core/Autosave.h"
#include "../core/ClipItem.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/ProjectLibrary.h"
#include "../core/Timeline.h"

ProjectFiles::ProjectFiles(Project *project, ProjectLibrary *library, Autosave *autosave,
                           QWidget *window)
    : m_project(project), m_library(library), m_autosave(autosave), m_window(window)
{
}

bool ProjectFiles::save()
{
    if (m_project->filePath().isEmpty())
        return saveAs(QString());
    QString error;
    if (!m_project->save(m_project->filePath(), &error)) {
        QMessageBox::warning(m_window, tr("Could not save project"), error);
        return false;
    }
    prunePlateScans();
    noteOpened();
    return true;
}

bool ProjectFiles::saveAs(const QString &untitledName)
{
    NewProjectDialog dlg(NewProjectDialog::Mode::SaveAs, m_library->projectsFolder(), m_window);
    // Suggest the project's name, or the first clip's for an untitled one.
    const QString from = !m_project->filePath().isEmpty() ? m_project->filePath() : untitledName;
    dlg.setName(QFileInfo(from).completeBaseName());
    if (dlg.exec() != QDialog::Accepted)
        return false;
    copyPlateScans(*m_project, m_project->filePath(), dlg.projectFile());
    copyLuts(*m_project, dlg.projectFile());
    QString error;
    if (!m_project->save(dlg.projectFile(), &error)) {
        QMessageBox::warning(m_window, tr("Could not save project"), error);
        return false;
    }
    prunePlateScans();
    noteOpened();
    return true;
}

void ProjectFiles::copyPlateScans(const Project &project, const QString &fromFile,
                                  const QString &toFile)
{
    const QString newCache = projectfolder::cacheDir(toFile);
    const QString oldCache = projectfolder::cacheDir(fromFile);
    if (newCache.isEmpty())
        return;
    const Timeline *timeline = project.timeline();
    for (int i = 0; i < timeline->count(); ++i) {
        const ClipItem &c = timeline->clipAt(i);
        const QString from = PlateScanCache::diskPathIn(oldCache, c.sourcePath, c.plateScan);
        const QString to = PlateScanCache::diskPathIn(newCache, c.sourcePath, c.plateScan);
        if (QFileInfo::exists(from) && !QFileInfo::exists(to)) {
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile::copy(from, to);
        }
    }
}

void ProjectFiles::copyLuts(Project &project, const QString &toFile)
{
    if (projectfolder::lutsDir(toFile).isEmpty())
        return;
    Timeline *timeline = project.timeline();
    for (int i = 0; i < timeline->count(); ++i) {
        const ClipItem &c = timeline->clipAt(i);
        if (c.color.lut.isEmpty() || !QFileInfo::exists(c.color.lut))
            continue;
        QString error;
        const QString copy = projectfolder::importLut(toFile, c.color.lut, &error);
        if (copy.isEmpty()) {
            std::fprintf(stderr, "[lut] %s\n", qPrintable(error));
            continue;
        }
        ColorGrade g = c.color;
        g.lut = copy;
        timeline->setClipColor(c.id, g);
    }
}

void ProjectFiles::prunePlateScans()
{
    const int n = PlateScanCache::prune(projectfolder::cacheDir(m_project->filePath()),
                                        m_project->plateScansInUse());
    if (n > 0)
        std::fprintf(stderr, "[plates] removed %d plate scan(s) no clip uses\n", n);
}

bool ProjectFiles::confirmDiscard()
{
    if (!m_project->modified())
        return true;
    // Qt's Save / Discard labels have no mnemonic; with one, S / D (and
    // Alt+S / Alt+D) press them. Enter saves, Esc cancels.
    QMessageBox box(QMessageBox::Question, tr("Unsaved changes"),
                    tr("The project has unsaved changes. Save them?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, m_window);
    box.button(QMessageBox::Save)->setText(tr("&Save"));
    box.button(QMessageBox::Discard)->setText(tr("&Discard"));
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    const auto choice = box.exec();
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save) {
        save();
        return !m_project->modified(); // a cancelled Save-As leaves it modified
    }
    m_autosave->discard();
    return true; // Discard
}

bool ProjectFiles::askRecover(const QString &projectFile)
{
    // Changes autosaved but never saved (the app didn't close properly):
    // offer them. An older recovery file is stale -- a later save has it all.
    const QString recovery = Autosave::pathFor(projectFile);
    if (!Autosave::hasRecovery(projectFile)) {
        if (QFileInfo::exists(recovery))
            QFile::remove(recovery);
        return false;
    }
    QMessageBox box(QMessageBox::Question, tr("Recover unsaved changes?"),
                    tr("%1 has unsaved changes from %2 that were never saved, probably "
                       "because orbis-reframe didn't close properly.\n\nRecover them, or "
                       "open the project as it was last saved and throw them away?")
                        .arg(QFileInfo(projectFile).completeBaseName(),
                             QLocale().toString(QFileInfo(recovery).lastModified(),
                                                QLocale::ShortFormat)),
                    QMessageBox::NoButton, m_window);
    QPushButton *recoverButton = box.addButton(tr("&Recover"), QMessageBox::AcceptRole);
    box.addButton(tr("&Discard changes"), QMessageBox::DestructiveRole);
    box.setDefaultButton(recoverButton);
    box.exec();
    const bool recover = box.clickedButton() == recoverButton;
    if (!recover)
        QFile::remove(recovery);
    return recover;
}

QString ProjectFiles::pickProjectFile()
{
    const QString current = m_project->filePath();
    return QFileDialog::getOpenFileName(
        m_window, tr("Open project"),
        current.isEmpty() ? m_library->projectsFolder() : QFileInfo(current).absolutePath(),
        tr("orbis-reframe project (*.%1)").arg(Project::kFileExtension));
}

std::optional<ProjectFiles::NewProject> ProjectFiles::askNewProject()
{
    NewProjectDialog dlg(NewProjectDialog::Mode::NewProject, m_library->projectsFolder(), m_window);
    if (dlg.exec() != QDialog::Accepted)
        return std::nullopt;
    return NewProject{dlg.projectFile(), dlg.output()};
}

void ProjectFiles::noteOpened()
{
    QString error;
    if (!m_library->noteOpened(m_project->filePath(), QDateTime::currentDateTime(), &error))
        std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
}
