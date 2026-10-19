#pragma once

#include <QString>

// A project's own folder: everything that belongs to one project in one place,
// so deleting the folder removes it all. Media is referenced, not copied;
// colour LUTs are copied in (small, and easy to lose track of).
//
//   <name>/<name>.orp   the project file
//   <name>/cache/       plate scans, waveforms, thumbnails (rebuilt if deleted)
//   <name>/exports/     where exports are suggested
//   <name>/screenshots/ where screenshots are suggested
//   <name>/luts/        the clips' colour LUTs (.cube), made when one is picked
//
// A project file that isn't <name>/<name>.orp is a loose one (projects saved
// before project folders existed): it still opens and saves where it is, and
// uses the shared user cache.
namespace projectfolder {

// The project file of project `name` created under `parentDir`.
QString projectFilePath(const QString &parentDir, const QString &name);

// Why `name` can't name a project (it becomes a folder and a file name), or
// empty when it can.
QString nameProblem(const QString &name);

// Make <parentDir>/<name>/ with its sub-folders. Fails, with *error set, on a
// bad name or when <parentDir>/<name> already exists. Returns the path of the
// project file to save (not written here).
QString create(const QString &parentDir, const QString &name, QString *error = nullptr);

// True when `projectFile` is <name>/<name>.orp, inside its own folder.
bool isProjectFolder(const QString &projectFile);

// The folders of the project whose file is `projectFile`; empty for a loose
// project file (or none). Not created here (see ensure()).
QString cacheDir(const QString &projectFile);
QString exportsDir(const QString &projectFile);
QString screenshotsDir(const QString &projectFile);
QString lutsDir(const QString &projectFile);

// `lutFile` copied into the luts/ folder of the project whose file is
// `projectFile`, and the copy's path: the same name, or "<name> 2.cube", ...
// when a different file has it (the same file again is reused). A loose or
// untitled project has no folder: `lutFile` itself. Empty, with *error set,
// when the copy fails.
QString importLut(const QString &projectFile, const QString &lutFile, QString *error = nullptr);

// Rename the project whose file is `projectFile` to `newName`: a project
// folder's folder and file (<parent>/<newName>/<newName>.orp), a loose
// project's file only. Fails, with *error set, on a bad name or when the new
// name is taken; nothing changes then. Returns the new project file.
QString rename(const QString &projectFile, const QString &newName, QString *error = nullptr);

// Copy the project whose file is `projectFile` as a new project `newName`
// beside it: a project folder gets a new folder with the project file and
// its cache/ (plate scans are slow to redo) and luts/, not its exports or
// screenshots;
// a loose project a new file next to it. Media is referenced, not copied
// (Project::copyFile). Returns the new project file, empty with *error set.
QString duplicate(const QString &projectFile, const QString &newName, QString *error = nullptr);

// A free name for a copy of the project: "<name> copy", "<name> copy 2", ...
QString copyName(const QString &projectFile);

// `dir` made if missing (a folder deleted by hand); `dir` itself on success,
// empty when it can't be made or `dir` is empty.
QString ensure(const QString &dir);

} // namespace projectfolder
