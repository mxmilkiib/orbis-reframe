#pragma once

#include <QListWidget>
#include <QWidget>

// The transitions to pick from, as tiles. Dragging one out hands it over as
// a Transition::kMimeType drag (the timeline takes it onto a cut).
class TransitionList : public QListWidget {
    Q_OBJECT

public:
    explicit TransitionList(QWidget *parent = nullptr);

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

// The left sidebar's Transitions tab: how one clip goes over into the next
// at a cut. For now a crossfade; drag it onto a cut, or double-click
// (Enter) to put it at the cut nearest the playhead. Its length is set in
// the right sidebar's Transition tab.
class TransitionsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TransitionsPanel(QWidget *parent = nullptr);

signals:
    // Put a transition of this type (Transition::typeName) at the cut
    // nearest the playhead.
    void addTransitionRequested(const QString &type);

private:
    TransitionList *m_list = nullptr;
};
