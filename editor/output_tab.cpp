#include "output_tab.h"

#include <QSignalBlocker>

OutputTab::OutputTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void OutputTab::wire()
{
    if (m_widgets.outputWidthSpin) {
        connect(m_widgets.outputWidthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputWidthChanged);
    }
    if (m_widgets.outputHeightSpin) {
        connect(m_widgets.outputHeightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputHeightChanged);
    }
    if (m_widgets.exportStartSpin) {
        connect(m_widgets.exportStartSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onExportStartChanged);
    }
    if (m_widgets.exportEndSpin) {
        connect(m_widgets.exportEndSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onExportEndChanged);
    }
    if (m_widgets.outputFormatCombo) {
        connect(m_widgets.outputFormatCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &OutputTab::onOutputFormatChanged);
    }
    if (m_widgets.renderButton) {
        connect(m_widgets.renderButton, &QPushButton::clicked,
                this, &OutputTab::onRenderClicked);
    }
}

void OutputTab::refresh()
{
    if (m_updating ||
        !m_widgets.outputWidthSpin ||
        !m_widgets.outputHeightSpin ||
        !m_widgets.exportStartSpin ||
        !m_widgets.exportEndSpin) {
        return;
    }

    m_updating = true;

    const bool hasTimeline = m_deps.hasTimeline && m_deps.hasTimeline();
    const QVector<ExportRangeSegment> ranges =
        m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges() : QVector<ExportRangeSegment>{};
    const int64_t startFrame = ranges.isEmpty() ? 0 : ranges.constFirst().startFrame;
    const int64_t endFrame = ranges.isEmpty() ? 0 : ranges.constLast().endFrame;

    {
        QSignalBlocker startBlocker(m_widgets.exportStartSpin);
        QSignalBlocker endBlocker(m_widgets.exportEndSpin);
        m_widgets.exportStartSpin->setEnabled(hasTimeline);
        m_widgets.exportEndSpin->setEnabled(hasTimeline);
        m_widgets.exportStartSpin->setValue(static_cast<int>(startFrame));
        m_widgets.exportEndSpin->setValue(static_cast<int>(endFrame));
    }

    updateRangeSummary();
    updateRenderButtonState();
    m_updating = false;
}

void OutputTab::applyRangeFromInspector()
{
    if (m_updating ||
        !m_deps.hasTimeline || !m_deps.hasTimeline() ||
        !m_widgets.exportStartSpin ||
        !m_widgets.exportEndSpin ||
        !m_deps.setExportRange) {
        return;
    }

    const int64_t startFrame = qMin<int64_t>(m_widgets.exportStartSpin->value(),
                                             m_widgets.exportEndSpin->value());
    const int64_t endFrame = qMax<int64_t>(m_widgets.exportStartSpin->value(),
                                           m_widgets.exportEndSpin->value());
    m_deps.setExportRange(startFrame, endFrame);
    refresh();
}

void OutputTab::renderFromInspector()
{
    if (!m_deps.hasTimeline || !m_deps.hasTimeline() ||
        !m_deps.hasClips || !m_deps.hasClips() ||
        !m_deps.getTimelineClips ||
        !m_deps.renderTimeline) {
        return;
    }

    if (m_deps.stopPlayback) {
        m_deps.stopPlayback();
    }

    RenderRequest request;
    request.outputFormat = m_widgets.outputFormatCombo
        ? m_widgets.outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }
    request.outputSize = QSize(
        m_widgets.outputWidthSpin ? m_widgets.outputWidthSpin->value() : 1080,
        m_widgets.outputHeightSpin ? m_widgets.outputHeightSpin->value() : 1920);
    request.clips = m_deps.getTimelineClips();
    request.renderSyncMarkers = m_deps.getRenderSyncMarkers
        ? m_deps.getRenderSyncMarkers()
        : QVector<RenderSyncMarker>{};
    request.exportRanges = m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges()
                                                          : QVector<ExportRangeSegment>{};
    request.exportStartFrame = request.exportRanges.isEmpty()
        ? (m_deps.exportStartFrame ? m_deps.exportStartFrame() : 0)
        : request.exportRanges.constFirst().startFrame;
    request.exportEndFrame = request.exportRanges.isEmpty()
        ? (m_deps.exportEndFrame ? m_deps.exportEndFrame() : 0)
        : request.exportRanges.constLast().endFrame;

    m_deps.renderTimeline(request);
}

void OutputTab::onOutputWidthChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setOutputSize) {
        m_deps.setOutputSize(QSize(value, m_widgets.outputHeightSpin ? m_widgets.outputHeightSpin->value() : 1920));
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onOutputHeightChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setOutputSize) {
        m_deps.setOutputSize(QSize(m_widgets.outputWidthSpin ? m_widgets.outputWidthSpin->value() : 1080, value));
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onExportStartChanged(int value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyRangeFromInspector();
}

void OutputTab::onExportEndChanged(int value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyRangeFromInspector();
}

void OutputTab::onOutputFormatChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onRenderClicked()
{
    renderFromInspector();
}

void OutputTab::updateRangeSummary()
{
    if (!m_widgets.outputRangeSummaryLabel) return;

    const QVector<ExportRangeSegment> ranges =
        m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges() : QVector<ExportRangeSegment>{};
    if (ranges.isEmpty()) {
        m_widgets.outputRangeSummaryLabel->setText(QStringLiteral("Timeline export range: none"));
        return;
    }

    QStringList segments;
    segments.reserve(ranges.size());
    for (const ExportRangeSegment& range : ranges) {
        segments.push_back(QStringLiteral("%1-%2").arg(range.startFrame).arg(range.endFrame));
    }

    if (ranges.size() == 1) {
        m_widgets.outputRangeSummaryLabel->setText(
            QStringLiteral("Timeline export range: %1").arg(segments.constFirst()));
    } else {
        m_widgets.outputRangeSummaryLabel->setText(
            QStringLiteral("Timeline export ranges (%1): %2\nStart/End fields show the overall span.")
                .arg(ranges.size())
                .arg(segments.join(QStringLiteral(" | "))));
    }
    m_widgets.outputRangeSummaryLabel->setToolTip(m_widgets.outputRangeSummaryLabel->text());
}

void OutputTab::updateRenderButtonState()
{
    if (!m_widgets.renderButton) return;
    const bool enabled = (!m_deps.hasTimeline || m_deps.hasTimeline()) &&
                         (!m_deps.hasClips || m_deps.hasClips());
    m_widgets.renderButton->setEnabled(enabled);
}
