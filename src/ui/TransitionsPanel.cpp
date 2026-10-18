#include "TransitionsPanel.h"

#include "../core/Transition.h"

#include <QAction>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QVBoxLayout>

namespace {

constexpr int kTypeRole = Qt::UserRole;

// A crossfade's tile: one picture's colour going over into another's.
QIcon crossfadeTile(const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QLinearGradient g(r.topLeft(), r.topRight());
    g.setColorAt(0.2, QColor(0x3a, 0x5f, 0x8a));
    g.setColorAt(0.8, QColor(0xa0, 0x70, 0x3a));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(r, 4, 4);
    // The two pictures' levels, crossing.
    p.setPen(QPen(QColor(255, 255, 255, 200), 2));
    const qreal m = size.height() / 4.0;
    p.drawLine(QPointF(m, m), QPointF(size.width() - m, size.height() - m));
    p.drawLine(QPointF(m, size.height() - m), QPointF(size.width() - m, m));
    return QIcon(pm);
}

// A dip to black's tile: a picture's colour going down to black and up
// into another's.
QIcon dipTile(const QSize &size, qreal dpr)
{
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QLinearGradient g(r.topLeft(), r.topRight());
    g.setColorAt(0.1, QColor(0x3a, 0x5f, 0x8a));
    g.setColorAt(0.5, Qt::black);
    g.setColorAt(0.9, QColor(0xa0, 0x70, 0x3a));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(r, 4, 4);
    // The picture's level, down to nothing at the cut and up again.
    p.setPen(QPen(QColor(255, 255, 255, 200), 2));
    const qreal m = size.height() / 4.0;
    p.drawLine(QPointF(m, m), QPointF(size.width() / 2.0, size.height() - m));
    p.drawLine(QPointF(size.width() / 2.0, size.height() - m), QPointF(size.width() - m, m));
    return QIcon(pm);
}

} // namespace

TransitionList::TransitionList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("transitionList"));
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWrapping(true);
    setSpacing(6);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFrameShape(QFrame::NoFrame);
}

QStringList TransitionList::mimeTypes() const
{
    return {QString::fromLatin1(Transition::kMimeType)};
}

QMimeData *TransitionList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(Transition::kMimeType),
                  items.first()->data(kTypeRole).toString().toLatin1());
    return mime;
}

TransitionsPanel::TransitionsPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("transitionsPanel"));
    setAttribute(Qt::WA_StyledBackground);

    auto *heading = new QLabel(tr("Transitions"), this);
    heading->setObjectName(QStringLiteral("sidebarPageTitle"));

    m_list = new TransitionList(this);
    const QSize tile(128, 72);
    m_list->setIconSize(tile);
    auto *crossfade =
        new QListWidgetItem(crossfadeTile(tile, devicePixelRatioF()), tr("Crossfade"), m_list);
    crossfade->setData(kTypeRole,
                       QString::fromLatin1(Transition::typeName(Transition::Type::Crossfade)));
    crossfade->setToolTip(tr("The picture and the clips' own sound blend from one clip into the "
                             "next. Drag it onto a cut, or double-click to put it at the cut "
                             "nearest the playhead."));
    auto *dip = new QListWidgetItem(dipTile(tile, devicePixelRatioF()), tr("Dip to black"), m_list);
    dip->setData(kTypeRole, QString::fromLatin1(Transition::typeName(Transition::Type::DipToBlack)));
    dip->setToolTip(tr("The clip before the cut fades out to black, the one after fades in from "
                       "it, and their own sound with them. Drag it onto a cut, or double-click "
                       "to put it at the cut nearest the playhead."));
    m_list->setCurrentItem(crossfade);
    const auto add = [this] {
        if (const QListWidgetItem *item = m_list->currentItem())
            emit addTransitionRequested(item->data(kTypeRole).toString());
    };
    connect(m_list, &QListWidget::itemActivated, this, add);

    auto *addAction = new QAction(tr("&Add at the nearest cut"), m_list);
    addAction->setObjectName(QStringLiteral("transitionAddAction"));
    connect(addAction, &QAction::triggered, this, add);
    m_list->addAction(addAction);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *hint = new QLabel(tr("Drag a transition onto a cut between two clips, or double-click "
                               "it to put it at the cut nearest the playhead. Its length is in "
                               "the Transition tab on the right."),
                            this);
    hint->setObjectName(QStringLiteral("transitionsHint"));
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_list, 1);
}
