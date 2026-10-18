#pragma once

#include <QList>

#include "TabSidebar.h"

class AudioPanel;
class ColourPanel;
class MediaInfoPanel;
class OverlayPanel;
class PrivacyPanel;
class ReframePanel;
class SettingsPanel;
class StabilizationPanel;
class StitchingPanel;
class TitleTextPanel;
class TransitionPanel;

// The right sidebar: the settings, in icon tabs by subject, each a column of
// collapsible sections (SettingsPanel) that say what they apply to.
//   Video   -- stitching, stabilization, reframe, colour, media information
//              (in workflow order; stitching and media information start
//              closed);
//   Audio   -- the selected audio clip, else every audio clip of the
//              video clip (AudioPanel: a section per clip);
//   Overlay -- the selected overlay video;
//   Title   -- the selected title: its text, then where it sits and when;
//   Transition -- the selected transition: its length;
//   Privacy -- the clip's number-plate blur.
// Owns the sections; MainWindow wires them to the model.
//
// Object names: settingsSidebar (TabSidebar's parts follow from it), the
// pages videoSettings / audioSettings / overlaySettings / titleSettings /
// transitionSettings / privacySettings (their tab buttons <page>Tab), and
// the sections stitchingPanel, stabilizationPanel, reframePanel,
// colourPanel, mediaInfoPanel, overlayPanel, titleTextPanel,
// titlePlacementPanel, transitionPanel, privacyPanel (headers
// <section>Header); the Audio tab's are AudioPanel's.
class SettingsSidebar : public TabSidebar {
    Q_OBJECT

public:
    explicit SettingsSidebar(QWidget *parent = nullptr);

    // The tabs, in order.
    enum Tab { VideoTab, AudioTab, OverlayTab, TitleTab, TransitionTab, PrivacyTab };

    StitchingPanel *stitching() const { return m_stitching; }
    StabilizationPanel *stabilization() const { return m_stabilization; }
    ReframePanel *reframe() const { return m_reframe; }
    ColourPanel *colour() const { return m_colour; }
    MediaInfoPanel *mediaInfo() const { return m_mediaInfo; }
    AudioPanel *audio() const { return m_audio; }
    OverlayPanel *overlay() const { return m_overlay; }
    TitleTextPanel *titleText() const { return m_titleText; }
    OverlayPanel *titlePlacement() const { return m_titlePlacement; } // kind Title
    TransitionPanel *transition() const { return m_transition; }
    PrivacyPanel *privacy() const { return m_privacy; }

    // Every fixed section, tab by tab, top to bottom (not the Audio tab's:
    // AudioPanel makes those per clip).
    QList<SettingsPanel *> sections() const { return m_sections; }

private:
    StitchingPanel *m_stitching = nullptr;
    StabilizationPanel *m_stabilization = nullptr;
    ReframePanel *m_reframe = nullptr;
    ColourPanel *m_colour = nullptr;
    MediaInfoPanel *m_mediaInfo = nullptr;
    AudioPanel *m_audio = nullptr;
    OverlayPanel *m_overlay = nullptr;
    TitleTextPanel *m_titleText = nullptr;
    OverlayPanel *m_titlePlacement = nullptr;
    TransitionPanel *m_transition = nullptr;
    PrivacyPanel *m_privacy = nullptr;
    QList<SettingsPanel *> m_sections;
};
