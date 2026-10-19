#pragma once

#include <QCoreApplication>
#include <QString>

#include <optional>

#include "../core/OutputSettings.h"

class Autosave;
class Project;
class ProjectLibrary;
class QWidget;

// The project as files, and asking the user about them: saving (into a new
// project folder for Save as, bringing the plate scans along), the "unsaved
// changes" and "recover?" questions, the Open project and New project
// dialogs, and the library's list of recent projects. Loading a project
// into the window -- binding the panels and playback to it -- is
// MainWindow's.
class ProjectFiles {
    Q_DECLARE_TR_FUNCTIONS(ProjectFiles)

public:
    // `window`: the dialogs' parent.
    ProjectFiles(Project *project, ProjectLibrary *library, Autosave *autosave, QWidget *window);

    // Save in place; an untitled project goes to saveAs(). False when it
    // wasn't saved (cancelled, or the error was shown).
    bool save();
    // Into a new project folder named in a dialog (suggested: the project's
    // name, else `untitledName`'s).
    bool saveAs(const QString &untitledName);
    // When the project has unsaved changes: Save / Discard / Cancel. False
    // when the user cancelled (or a Save didn't happen): the caller stops.
    bool confirmDiscard();

    // Before opening `projectFile`: if it has changes autosaved but never
    // saved, ask whether to recover them (else they're thrown away); an
    // older recovery file is removed. True = load the recovery file.
    bool askRecover(const QString &projectFile);
    // Open project…: a project file picked (empty = cancelled), starting in
    // the current project's folder or the projects folder.
    QString pickProjectFile();
    // New project…: where the new project's file goes and its output format;
    // nothing when cancelled.
    struct NewProject {
        QString file;
        OutputSettings output;
    };
    std::optional<NewProject> askNewProject();
    // Saved or opened: to the top of the library.
    void noteOpened();

    // Copy the plate scans of the timeline's clips from the cache of
    // project `fromFile` into that of `toFile` (a project folder's), where
    // it has none: scans take minutes, waveforms are quick to redo.
    static void copyPlateScans(const Project &project, const QString &fromFile,
                               const QString &toFile);

    // Copy the clips' LUTs into the luts/ folder of project `toFile` (a
    // project folder's), pointing the clips at the copies: a project saved
    // into a new folder takes its LUTs along. One that can't be copied stays
    // where it is.
    static void copyLuts(Project &project, const QString &toFile);

private:
    // Saved: drop the scans in the project's cache no clip or undo step
    // names (PlateScanCache::prune).
    void prunePlateScans();

    Project *m_project;
    ProjectLibrary *m_library;
    Autosave *m_autosave;
    QWidget *m_window;
};
