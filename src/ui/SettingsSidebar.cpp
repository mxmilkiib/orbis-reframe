#include "SettingsSidebar.h"

#include <QScrollArea>
#include <QScrollBar>

#include <algorithm>

#include "SettingsPanel.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"
#include "panels/OverlayPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ReframePanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"
#include "panels/TitleTextPanel.h"
#include "panels/TransitionPanel.h"

namespace {

template <typename T> T *section(const char *name, const QString &scope)
{
    auto *s = new T;
    s->setObjectName(QLatin1String(name));
    s->setScope(scope);
    return s;
}

} // namespace

SettingsSidebar::SettingsSidebar(QWidget *parent)
    : TabSidebar(Side::Right, QStringLiteral("settingsSidebar"), parent)
{
    const QString thisClip = tr("This clip");
    m_stitching = section<StitchingPanel>("stitchingPanel", thisClip);
    m_stabilization = section<StabilizationPanel>("stabilizationPanel", thisClip);
    m_reframe = section<ReframePanel>("reframePanel", tr("Keys at the playhead"));
    m_colour = section<ColourPanel>("colourPanel", thisClip);
    m_mediaInfo = section<MediaInfoPanel>("mediaInfoPanel", thisClip);
    m_audio = new AudioPanel(this); // a section per clip, made as needed
    m_overlay = section<OverlayPanel>("overlayPanel", tr("Selected overlay clip"));
    const QString selectedTitle = tr("Selected title");
    m_titleText = section<TitleTextPanel>("titleTextPanel", selectedTitle);
    m_titlePlacement = new OverlayPanel(OverlayPanel::Kind::Title);
    m_titlePlacement->setObjectName(QStringLiteral("titlePlacementPanel"));
    m_titlePlacement->setScope(selectedTitle);
    m_transition = section<TransitionPanel>("transitionPanel", tr("Selected transition"));
    m_privacy = section<PrivacyPanel>("privacyPanel", thisClip);
    // Rarely touched: closed until the user opens them.
    m_stitching->setExpanded(false);
    m_mediaInfo->setExpanded(false);

    const QList<SettingsPanel *> video = {m_stitching, m_stabilization, m_reframe, m_colour,
                                          m_mediaInfo};
    m_sections = video;
    m_sections << m_overlay << m_titleText << m_titlePlacement << m_transition << m_privacy;

    const QList<QScrollArea *> pages = {
        makeSettingsPage(QStringLiteral("videoSettings"), video),
        makeSettingsPage(QStringLiteral("audioSettings"), {}),
        makeSettingsPage(QStringLiteral("overlaySettings"), {m_overlay}),
        makeSettingsPage(QStringLiteral("titleSettings"), {m_titleText, m_titlePlacement}),
        makeSettingsPage(QStringLiteral("transitionSettings"), {m_transition}),
        makeSettingsPage(QStringLiteral("privacySettings"), {m_privacy}),
    };
    m_audio->attach(pages[1]);
    addTab(pages[0], QStringLiteral("video"), tr("Video"));
    addTab(pages[1], QStringLiteral("volume-2"), tr("Audio"));
    addTab(pages[2], QStringLiteral("picture-in-picture-2"), tr("Overlay"));
    addTab(pages[3], QStringLiteral("type"), tr("Title"));
    addTab(pages[4], QStringLiteral("blend"), tr("Transition"));
    addTab(pages[5], QStringLiteral("eye-off"), tr("Privacy"));

    // Resizable like the left sidebar, but never narrower than the widest
    // section needs (the pages don't scroll sideways). Scaled with the font.
    int needed = 0;
    for (QScrollArea *page : pages)
        needed = std::max(needed, page->widget()->minimumSizeHint().width() +
                                      page->verticalScrollBar()->sizeHint().width());
    { // the Audio tab's sections come and go: measure one
        AudioClipSection probe;
        needed = std::max(needed, probe.minimumSizeHint().width() +
                                      pages[1]->verticalScrollBar()->sizeHint().width());
    }
    const int standard = std::max({340, fontMetrics().height() * 20, needed});
    setPageWidthRange(std::max(280, needed), standard, 900);
    setPageWidth(standard);
}
