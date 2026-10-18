#pragma once

#include "../SettingsPanel.h"

#include "../../core/Transition.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

// The Transition tab's section: the selected transition (Transition.h) --
// its type, how long it is, how long it plays when there isn't that much
// footage past the trims, and a button to remove it.
//
// Pure view like the other panels: edits go back to MainWindow as signals.
class TransitionPanel : public SettingsPanel {
    Q_OBJECT

public:
    explicit TransitionPanel(QWidget *parent = nullptr);

    // Reflect a transition: its setting, the cut's two clips' names, and
    // what plays (Timeline::transitionSpan) -- `note` says why that's
    // shorter, empty when it isn't. Does not emit.
    void setTransition(const Transition &t, const QString &cut, const QString &note);
    // None selected: `hint` says how to pick one; the fields are greyed out.
    void setNoTransition(const QString &hint);

signals:
    void typeEdited(Transition::Type type);
    void durationEdited(qint64 durationMs);
    void removeRequested();

private:
    QLabel *m_cut = nullptr;
    QComboBox *m_type = nullptr;
    QDoubleSpinBox *m_duration = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_remove = nullptr;
    bool m_updating = false;
};
