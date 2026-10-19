#pragma once

#include "../SettingsPanel.h"

#include "../../core/ClipItem.h" // ColorGrade

#include <QStringList>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

// Per-clip colour grade: a LUT (.cube) with its Strength, then Exposure /
// Contrast / Saturation / Temperature sliders for the clip currently under
// the playhead. Mirrors the Audio panel's pattern -- an edit hands the new
// ColorGrade back to MainWindow to write onto that clip via
// Timeline::setClipColor; nothing here talks to the renderer directly.
// Applied at render time in both playback and export -- see ColorGrade's
// own comment for where. Reset / Copy / Paste and Load LUT... ask
// MainWindow, which holds the copied grade (shared with the timeline's
// right-click menu) and copies a loaded LUT into the project.
class ColourPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit ColourPanel(QWidget *parent = nullptr);

    // Reflect a clip's grade. Does not emit gradeEdited.
    void setState(const ColorGrade &grade);
    // No clip under the playhead (e.g. an empty timeline): the neutral grade,
    // greyed out. Does not emit.
    void setNoClip();
    // Whether a grade has been copied (enables Paste).
    void setCanPaste(bool canPaste);
    // The LUTs the picker offers (absolute paths; the project's luts/ and
    // the clips'), besides None and Load LUT.... Call before setState.
    void setLuts(const QStringList &paths);
    // Why the clip's LUT isn't applied (missing, unreadable), shown under
    // the picker; empty = it is.
    void setLutProblem(const QString &problem);
    // The clip needs a LUT (filmed in log, and it has none): the picker
    // gets a red frame and `why` under it. Empty = it doesn't.
    void setLutWanted(const QString &why);

signals:
    void gradeEdited(const ColorGrade &grade);
    // Load LUT... picked: MainWindow asks for the file.
    void loadLutRequested();
    void copyRequested();
    void pasteRequested();

private:
    ColorGrade currentGrade() const;
    void emitIfEdited();
    // Select `path` in the picker (adding it when it isn't listed).
    void selectLut(const QString &path);

    QComboBox *m_lut = nullptr; // None, the LUTs (data = path), Load LUT...
    QSlider *m_lutStrength = nullptr; // percent: 0..100
    QLabel *m_lutStrengthValue = nullptr;
    QLabel *m_lutProblem = nullptr;
    QLabel *m_lutWanted = nullptr;
    QStringList m_luts;
    QString m_shownLut; // the clip's, while Load LUT... is picked
    QSlider *m_exposure = nullptr; // tenths of a stop: -30..30 -> -3.0..3.0 EV
    QLabel *m_exposureValue = nullptr;
    QSlider *m_contrast = nullptr; // percent: -100..100
    QLabel *m_contrastValue = nullptr;
    QSlider *m_saturation = nullptr; // percent: -100..100
    QLabel *m_saturationValue = nullptr;
    QSlider *m_temperature = nullptr; // percent: -100..100
    QLabel *m_temperatureValue = nullptr;
    QPushButton *m_reset = nullptr;
    QPushButton *m_paste = nullptr;
    bool m_updating = false;
};
