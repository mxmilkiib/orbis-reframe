#include "ColourPanel.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

// The exposure slider counts tenths of a stop; the others whole percent.
constexpr float kExposureStepsPerEv = 10.0f;
constexpr int kMinExposureSteps = int(ColorGrade::kMinExposureEv * kExposureStepsPerEv);
constexpr int kMaxExposureSteps = int(ColorGrade::kMaxExposureEv * kExposureStepsPerEv);
constexpr int kMinPercent = int(ColorGrade::kMinPercent);
constexpr int kMaxPercent = int(ColorGrade::kMaxPercent);

// The picker's Load LUT... item.
constexpr int kLoadRole = Qt::UserRole + 1;

// A slider + live value label in one row, the shape every row here shares
// (matches StabilizationPanel's Strength/Amount rows).
struct SliderRow {
    QWidget *row = nullptr;
    QSlider *slider = nullptr;
    QLabel *value = nullptr;
};

SliderRow makeSliderRow(QWidget *parent, int lo, int hi, int step, int page)
{
    SliderRow r;
    r.row = new QWidget(parent);
    auto *layout = new QHBoxLayout(r.row);
    layout->setContentsMargins(0, 0, 0, 0);
    r.slider = new QSlider(Qt::Horizontal, r.row);
    r.slider->setRange(lo, hi);
    r.slider->setSingleStep(step);
    r.slider->setPageStep(page);
    r.slider->setTickPosition(QSlider::TicksBelow);
    r.slider->setTickInterval((hi - lo) / 4);
    r.value = new QLabel(r.row);
    r.value->setMinimumWidth(r.row->fontMetrics().horizontalAdvance(QStringLiteral("-100%")));
    layout->addWidget(r.slider, 1);
    layout->addWidget(r.value);
    return r;
}

} // namespace

ColourPanel::ColourPanel(QWidget *parent)
    : SettingsPanel(tr("Colour"), parent)
{
    m_lut = new QComboBox(this);
    m_lut->setObjectName(QStringLiteral("colourLut"));
    // The row's width, whatever the names: camera makers' are long.
    m_lut->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_lut->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_lut->setMinimumContentsLength(8);
    m_lut->setToolTip(tr("A 3D LUT (.cube) applied before the sliders below, e.g. the camera "
                         "maker's one that turns log footage into normal colours."));
    form()->addRow(tr("LUT"), m_lut);
    m_lutProblem = new QLabel(this);
    m_lutProblem->setObjectName(QStringLiteral("colourLutProblem"));
    m_lutProblem->setWordWrap(true);
    m_lutProblem->setVisible(false);
    form()->addRow(m_lutProblem);
    m_lutWanted = new QLabel(this);
    m_lutWanted->setObjectName(QStringLiteral("colourLutWanted"));
    m_lutWanted->setWordWrap(true);
    m_lutWanted->setVisible(false);
    form()->addRow(m_lutWanted);

    SliderRow strength = makeSliderRow(this, 0, kMaxPercent, 1, 10);
    m_lutStrength = strength.slider;
    m_lutStrength->setObjectName(QStringLiteral("colourLutStrength"));
    m_lutStrengthValue = strength.value;
    m_lutStrength->setToolTip(tr("How much of the LUT: 100 % is the LUT's look, 0 % the clip "
                                 "without it."));
    form()->addRow(tr("Strength"), strength.row);

    SliderRow exposure = makeSliderRow(this, kMinExposureSteps, kMaxExposureSteps, 1, 5);
    m_exposure = exposure.slider;
    m_exposure->setObjectName(QStringLiteral("colourExposure"));
    m_exposureValue = exposure.value;
    m_exposure->setToolTip(tr("Brighter or darker, in stops: +1 doubles the light."));
    form()->addRow(tr("Exposure"), exposure.row);

    SliderRow contrast = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_contrast = contrast.slider;
    m_contrast->setObjectName(QStringLiteral("colourContrast"));
    m_contrastValue = contrast.value;
    form()->addRow(tr("Contrast"), contrast.row);

    SliderRow saturation = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_saturation = saturation.slider;
    m_saturation->setObjectName(QStringLiteral("colourSaturation"));
    m_saturationValue = saturation.value;
    saturation.slider->setToolTip(tr("How strong the colours are. -100 % is black and white."));
    form()->addRow(tr("Saturation"), saturation.row);

    SliderRow temperature = makeSliderRow(this, kMinPercent, kMaxPercent, 1, 10);
    m_temperature = temperature.slider;
    m_temperature->setObjectName(QStringLiteral("colourTemperature"));
    m_temperatureValue = temperature.value;
    temperature.slider->setToolTip(tr("Warmer (+, more orange) or cooler (-, more blue)."));
    form()->addRow(tr("Temperature"), temperature.row);

    auto *buttonRow = new QWidget(this);
    auto *buttons = new QHBoxLayout(buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    m_reset = new QPushButton(tr("Reset"), buttonRow);
    m_reset->setObjectName(QStringLiteral("colourReset"));
    m_reset->setToolTip(tr("No LUT and all four sliders back to 0: the clip as the camera "
                           "recorded it."));
    auto *copy = new QPushButton(tr("Copy grade"), buttonRow);
    copy->setObjectName(QStringLiteral("colourCopy"));
    copy->setToolTip(tr("Copy this clip's LUT and settings, to paste onto other clips."));
    m_paste = new QPushButton(tr("Paste grade"), buttonRow);
    m_paste->setObjectName(QStringLiteral("colourPaste"));
    m_paste->setToolTip(tr("Give this clip the copied settings. To paste onto every clip, "
                           "right-click a clip on the timeline."));
    m_paste->setEnabled(false);
    buttons->addWidget(m_reset);
    buttons->addWidget(copy);
    buttons->addWidget(m_paste);
    form()->addRow(buttonRow);
    connect(m_reset, &QPushButton::clicked, this, [this] { emit gradeEdited(ColorGrade{}); });
    connect(copy, &QPushButton::clicked, this, &ColourPanel::copyRequested);
    connect(m_paste, &QPushButton::clicked, this, &ColourPanel::pasteRequested);

    setHint(tr("Applies to this clip in playback and export."));

    setLuts({});
    setState(ColorGrade{});

    connect(m_lut, &QComboBox::activated, this, [this](int index) {
        if (m_lut->itemData(index, kLoadRole).toBool()) {
            selectLut(m_shownLut); // until a file is picked
            emit loadLutRequested();
            return;
        }
        emitIfEdited();
    });
    connect(m_lutStrength, &QSlider::valueChanged, this, [this](int v) {
        m_lutStrengthValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    connect(m_exposure, &QSlider::valueChanged, this, [this](int v) {
        m_exposureValue->setText(tr("%1 EV").arg(v / kExposureStepsPerEv, 0, 'f', 1));
        emitIfEdited();
    });
    connect(m_contrast, &QSlider::valueChanged, this, [this](int v) {
        m_contrastValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    connect(m_saturation, &QSlider::valueChanged, this, [this](int v) {
        m_saturationValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
    connect(m_temperature, &QSlider::valueChanged, this, [this](int v) {
        m_temperatureValue->setText(tr("%1%").arg(v));
        emitIfEdited();
    });
}

ColorGrade ColourPanel::currentGrade() const
{
    ColorGrade g;
    g.lut = m_lut->currentData().toString();
    g.lutStrength = static_cast<float>(m_lutStrength->value());
    g.exposureEv = static_cast<float>(m_exposure->value()) / kExposureStepsPerEv;
    g.contrast = static_cast<float>(m_contrast->value());
    g.saturation = static_cast<float>(m_saturation->value());
    g.temperature = static_cast<float>(m_temperature->value());
    return g;
}

void ColourPanel::setState(const ColorGrade &grade)
{
    m_updating = true;
    setBodyEnabled(true);

    m_shownLut = grade.lut;
    selectLut(grade.lut);
    const int strength = std::clamp(int(std::lround(grade.lutStrength)), 0, kMaxPercent);
    m_lutStrength->setValue(strength);
    m_lutStrengthValue->setText(tr("%1%").arg(strength));
    m_lutStrength->setEnabled(!grade.lut.isEmpty());

    const int ev = std::clamp(int(std::lround(grade.exposureEv * kExposureStepsPerEv)),
                              kMinExposureSteps, kMaxExposureSteps);
    m_exposure->setValue(ev);
    m_exposureValue->setText(tr("%1 EV").arg(ev / kExposureStepsPerEv, 0, 'f', 1));

    const int contrast = std::clamp(int(std::lround(grade.contrast)), kMinPercent, kMaxPercent);
    m_contrast->setValue(contrast);
    m_contrastValue->setText(tr("%1%").arg(contrast));

    const int saturation = std::clamp(int(std::lround(grade.saturation)), kMinPercent, kMaxPercent);
    m_saturation->setValue(saturation);
    m_saturationValue->setText(tr("%1%").arg(saturation));

    const int temperature = std::clamp(int(std::lround(grade.temperature)), kMinPercent, kMaxPercent);
    m_temperature->setValue(temperature);
    m_temperatureValue->setText(tr("%1%").arg(temperature));

    m_reset->setEnabled(currentGrade() != ColorGrade{});
    m_updating = false;
}

void ColourPanel::setNoClip()
{
    setLutProblem({});
    setLutWanted({});
    setState(ColorGrade{});
    setBodyEnabled(false);
}

void ColourPanel::setLuts(const QStringList &paths)
{
    m_luts = paths;
    const bool was = m_updating;
    m_updating = true;
    m_lut->clear();
    m_lut->addItem(tr("None"), QString());
    for (const QString &p : m_luts) {
        m_lut->addItem(QFileInfo(p).completeBaseName(), p);
        m_lut->setItemData(m_lut->count() - 1, p, Qt::ToolTipRole);
    }
    m_lut->insertSeparator(m_lut->count());
    m_lut->addItem(tr("Load LUT…"));
    m_lut->setItemData(m_lut->count() - 1, true, kLoadRole);
    selectLut(m_shownLut);
    m_updating = was;
}

void ColourPanel::selectLut(const QString &path)
{
    int i = m_lut->findData(path);
    if (i < 0) { // not listed (yet): before the separator
        i = m_lut->count() - 2;
        m_lut->insertItem(i, QFileInfo(path).completeBaseName(), path);
        m_lut->setItemData(i, path, Qt::ToolTipRole);
    }
    const QSignalBlocker block(m_lut);
    m_lut->setCurrentIndex(i);
}

void ColourPanel::setLutProblem(const QString &problem)
{
    m_lutProblem->setText(problem);
    m_lutProblem->setVisible(!problem.isEmpty());
}

void ColourPanel::setLutWanted(const QString &why)
{
    m_lutWanted->setText(why);
    m_lutWanted->setVisible(!why.isEmpty());
    // theme.qss: QComboBox[attention="true"] has the error colour's frame.
    const bool attention = !why.isEmpty();
    if (m_lut->property("attention").toBool() != attention) {
        m_lut->setProperty("attention", attention);
        m_lut->style()->unpolish(m_lut);
        m_lut->style()->polish(m_lut);
    }
}

void ColourPanel::setCanPaste(bool canPaste)
{
    m_paste->setEnabled(canPaste);
}

void ColourPanel::emitIfEdited()
{
    if (m_updating)
        return;
    m_lutStrength->setEnabled(!m_lut->currentData().toString().isEmpty());
    m_reset->setEnabled(currentGrade() != ColorGrade{});
    emit gradeEdited(currentGrade());
}
