#include "TransitionPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

#include <cmath>

TransitionPanel::TransitionPanel(QWidget *parent) : SettingsPanel(tr("Transition"), parent)
{
    m_cut = new QLabel(this);
    m_cut->setObjectName(QStringLiteral("transitionCut"));
    m_cut->setWordWrap(true);
    form()->addRow(tr("Between"), m_cut);
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("transitionType"));
    m_type->addItem(tr("Crossfade"), int(Transition::Type::Crossfade));
    m_type->addItem(tr("Dip to black"), int(Transition::Type::DipToBlack));
    m_type->setToolTip(tr("Crossfade: the two clips blend. Dip to black: the one fades out to "
                          "black, the other in from it."));
    form()->addRow(tr("Type"), m_type);
    m_duration = new QDoubleSpinBox(this);
    m_duration->setObjectName(QStringLiteral("transitionDuration"));
    m_duration->setRange(Transition::kMinMs / 1000.0, Transition::kMaxMs / 1000.0);
    m_duration->setDecimals(1);
    m_duration->setSingleStep(0.1);
    m_duration->setSuffix(tr(" s"));
    m_duration->setKeyboardTracking(false); // commit on Enter / focus-out / arrows
    form()->addRow(tr("Duration"), m_duration);
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("transitionNote"));
    m_note->setWordWrap(true);
    form()->addRow(QString(), m_note);
    m_remove = new QPushButton(tr("Remove transition"), this);
    m_remove->setObjectName(QStringLiteral("transitionRemove"));
    m_remove->setToolTip(tr("Back to a hard cut (Delete)"));
    form()->addRow(QString(), m_remove);

    connect(m_duration, &QDoubleSpinBox::valueChanged, this, [this](double s) {
        if (!m_updating)
            emit durationEdited(std::llround(s * 1000.0));
    });
    connect(m_type, &QComboBox::activated, this, [this](int i) {
        if (!m_updating)
            emit typeEdited(Transition::Type(m_type->itemData(i).toInt()));
    });
    connect(m_remove, &QPushButton::clicked, this, &TransitionPanel::removeRequested);
    setNoTransition(QString());
}

void TransitionPanel::setTransition(const Transition &t, const QString &cut, const QString &note)
{
    m_updating = true;
    m_cut->setText(cut);
    m_type->setCurrentIndex(m_type->findData(int(t.type)));
    m_duration->setValue(t.durationMs / 1000.0);
    m_duration->setToolTip(
        t.usesHandles() ? tr("How long the two clips blend, centred on the cut. It uses footage "
                             "trimmed off the clips: half of it past the end of the clip "
                             "before, half before the start of the clip after.")
                        : tr("How long it takes, centred on the cut: the clip before fades out "
                             "over the first half, the clip after fades in over the second."));
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
    for (QWidget *w : {static_cast<QWidget *>(m_type), static_cast<QWidget *>(m_duration),
                       static_cast<QWidget *>(m_remove)})
        w->setEnabled(true);
    setHint(t.usesHandles()
                ? tr("The picture and the two clips' own sound blend through the cut.")
                : tr("The picture and the two clips' own sound fade out to black and in again "
                     "at the cut."));
    m_updating = false;
}

void TransitionPanel::setNoTransition(const QString &hint)
{
    m_updating = true;
    m_cut->setText(QString());
    m_type->setCurrentIndex(-1);
    m_note->clear();
    m_note->hide();
    m_type->setEnabled(false);
    m_duration->setEnabled(false);
    m_remove->setEnabled(false);
    setHint(hint.isEmpty() ? tr("Click a transition on the timeline to set it up. The "
                                "Transitions tab on the left has them.")
                           : hint);
    m_updating = false;
}
