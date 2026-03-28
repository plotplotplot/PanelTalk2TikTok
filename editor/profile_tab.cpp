#include "profile_tab.h"
#include "async_decoder.h"
#include "frame_handle.h"
#include "editor_shared.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStringList>
#include <QTableWidgetItem>
#include <QDir>
#include <QCoreApplication>

extern "C"
{
#include <libavutil/hwcontext.h>
}

ProfileTab::ProfileTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void ProfileTab::wire()
{
    if (m_widgets.profileBenchmarkButton) {
        connect(m_widgets.profileBenchmarkButton, &QPushButton::clicked,
                this, &ProfileTab::onBenchmarkClicked);
    }
}

void ProfileTab::refresh()
{
    if (!m_widgets.profileSummaryTable) return;

    const QJsonObject previewProfile = m_deps.profilingSnapshot();
    const QJsonObject decoderProfile = previewProfile.value(QStringLiteral("decoder")).toObject();
    const QJsonObject cacheProfile = previewProfile.value(QStringLiteral("cache")).toObject();
    const QJsonObject playbackPipelineProfile = previewProfile.value(QStringLiteral("playback_pipeline")).toObject();
    const QJsonObject memoryBudgetProfile = previewProfile.value(QStringLiteral("memory_budget")).toObject();

    updateProfileTable(previewProfile, decoderProfile, cacheProfile,
                       playbackPipelineProfile, memoryBudgetProfile);
}

void ProfileTab::runDecodeBenchmark()
{
    TimelineClip clip;
    if (!m_deps.profileBenchmarkClip(&clip)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Decode Benchmark"),
                             QStringLiteral("Select a visual clip or add one to the timeline first."));
        return;
    }

    const QString mediaPath = m_deps.playbackMediaPath(clip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Decode Benchmark"),
                             QStringLiteral("This clip has no playable media path."));
        return;
    }

    QProgressDialog progress(QStringLiteral("Running decode benchmark..."),
                             QString(),
                             0,
                             0,
                             nullptr);
    progress.setWindowTitle(QStringLiteral("Decode Benchmark"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.show();
    QCoreApplication::processEvents();

    QJsonObject benchmark{
        {QStringLiteral("success"), false},
        {QStringLiteral("clip_label"), clip.label},
        {QStringLiteral("path"), QDir::toNativeSeparators(mediaPath)}
    };

    editor::DecoderContext ctx(mediaPath);
    if (!ctx.initialize()) {
        benchmark[QStringLiteral("error")] = QStringLiteral("Failed to initialize decoder context.");
        m_lastDecodeBenchmark = benchmark;
        m_deps.refreshInspector();
        return;
    }

    const editor::VideoStreamInfo info = ctx.info();
    const int64_t durationFrames = qMax<int64_t>(1, info.durationFrames);
    const int framesToBenchmark = static_cast<int>(qMin<int64_t>(90, durationFrames));
    int decodedFrames = 0;
    int nullFrames = 0;

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < framesToBenchmark; ++i) {
        editor::FrameHandle frame = ctx.decodeFrame(i);
        if (frame.isNull()) {
            ++nullFrames;
        } else {
            ++decodedFrames;
        }
    }
    const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
    const double fps = (1000.0 * decodedFrames) / static_cast<double>(elapsedMs);

    benchmark[QStringLiteral("success")] = true;
    benchmark[QStringLiteral("codec")] = info.codecName;
    benchmark[QStringLiteral("decode_path")] = ctx.isHardwareAccelerated() ? QStringLiteral("hardware") : QStringLiteral("software");
    benchmark[QStringLiteral("frames_decoded")] = decodedFrames;
    benchmark[QStringLiteral("null_frames")] = nullFrames;
    benchmark[QStringLiteral("elapsed_ms")] = static_cast<qint64>(elapsedMs);
    benchmark[QStringLiteral("fps")] = fps;
    m_lastDecodeBenchmark = benchmark;
    m_deps.refreshInspector();
}

void ProfileTab::onBenchmarkClicked()
{
    runDecodeBenchmark();
}

void ProfileTab::updateProfileTable(const QJsonObject& previewProfile,
                                    const QJsonObject& decoderProfile,
                                    const QJsonObject& cacheProfile,
                                    const QJsonObject& playbackPipelineProfile,
                                    const QJsonObject& memoryBudgetProfile)
{
    QVector<QPair<QString, QString>> rows;
    auto addRow = [&rows](const QString& label, const QString& value) {
        rows.push_back({label, value});
    };

    const auto formatDuration = [](qint64 ms) -> QString {
        if (ms <= 0) return QStringLiteral("0 ms");
        const qint64 totalSeconds = ms / 1000;
        const qint64 minutes = totalSeconds / 60;
        const qint64 seconds = totalSeconds % 60;
        const qint64 remainderMs = ms % 1000;
        if (minutes > 0) {
            return QStringLiteral("%1m %2.%3s").arg(minutes).arg(seconds).arg(remainderMs / 100, 1, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1.%2s").arg(seconds).arg(remainderMs / 100, 1, 10, QLatin1Char('0'));
    };

    const auto formatStage = [](const QJsonObject& stats, const QString& totalKey, const QString& perFrameKey) -> QString {
        const qint64 totalMs = stats.value(totalKey).toVariant().toLongLong();
        const double perFrame = stats.value(perFrameKey).toDouble();
        if (totalMs <= 0) return QStringLiteral("0 ms");
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(totalMs)
            .arg(QString::number(perFrame, 'f', 2));
    };

    addRow(QStringLiteral("Preview Backend"),
           previewProfile.value(QStringLiteral("backend_name")).toString(QStringLiteral("unknown")));
    addRow(QStringLiteral("Playback Active"),
           previewProfile.value(QStringLiteral("playback_active")).toBool() ? QStringLiteral("Yes") : QStringLiteral("No"));
    addRow(QStringLiteral("Current Timeline Frame"),
           QString::number(previewProfile.value(QStringLiteral("current_frame")).toVariant().toLongLong()));
    addRow(QStringLiteral("Timeline Clips"),
           QString::number(previewProfile.value(QStringLiteral("timeline_clip_count")).toInt()));
    addRow(QStringLiteral("Decoder Workers"),
           QString::number(decoderProfile.value(QStringLiteral("worker_count")).toInt()));
    addRow(QStringLiteral("Pending Decode Requests"),
           QString::number(decoderProfile.value(QStringLiteral("pending_requests")).toInt()));
    addRow(QStringLiteral("Cached Frames"),
           QString::number(cacheProfile.value(QStringLiteral("total_cached_frames")).toInt()));
    addRow(QStringLiteral("Cache Hit Rate"),
           QStringLiteral("%1%").arg(cacheProfile.value(QStringLiteral("hit_rate")).toDouble() * 100.0, 0, 'f', 1));
    addRow(QStringLiteral("Playback Buffered Frames"),
           QString::number(playbackPipelineProfile.value(QStringLiteral("buffered_frames")).toInt()));
    addRow(QStringLiteral("Dropped Presentation Frames"),
           QString::number(playbackPipelineProfile.value(QStringLiteral("dropped_presentation_frames")).toInt()));
    addRow(QStringLiteral("CPU Memory Usage"),
           QStringLiteral("%1 / %2").arg(memoryBudgetProfile.value(QStringLiteral("cpu_usage")).toVariant().toLongLong())
                                    .arg(memoryBudgetProfile.value(QStringLiteral("cpu_max")).toVariant().toLongLong()));
    addRow(QStringLiteral("GPU Memory Usage"),
           QStringLiteral("%1 / %2").arg(memoryBudgetProfile.value(QStringLiteral("gpu_usage")).toVariant().toLongLong())
                                    .arg(memoryBudgetProfile.value(QStringLiteral("gpu_max")).toVariant().toLongLong()));
    addRow(QStringLiteral("FFmpeg HW Device Types"),
           availableHardwareDeviceTypes().isEmpty()
               ? QStringLiteral("none")
               : availableHardwareDeviceTypes().join(QStringLiteral(", ")));
    addRow(QStringLiteral("Decode Policy"),
           QStringLiteral("Opaque video: hardware when supported"));
    addRow(QStringLiteral("Decode Policy (Alpha/Images)"),
           QStringLiteral("Software decode"));
    addRow(QStringLiteral("Export Encode Policy"),
           QStringLiteral("Prefer hardware H.264 for MP4, fallback to software"));

    const QJsonObject activeExport = previewProfile.value(QStringLiteral("export")).toObject();
    const QJsonObject exportStats = activeExport.value(QStringLiteral("live")).toObject();
    if (exportStats.isEmpty()) {
        addRow(QStringLiteral("Export Status"), QStringLiteral("Idle"));
    } else {
        addRow(QStringLiteral("Export Status"),
               exportStats.value(QStringLiteral("status")).toString(QStringLiteral("Idle")));
        addRow(QStringLiteral("Export Output"),
               exportStats.value(QStringLiteral("output_path")).toString(QStringLiteral("unknown")));
        addRow(QStringLiteral("Export Progress"),
               QStringLiteral("%1 / %2 frames")
                   .arg(exportStats.value(QStringLiteral("frames_completed")).toVariant().toLongLong())
                   .arg(exportStats.value(QStringLiteral("total_frames")).toVariant().toLongLong()));
        addRow(QStringLiteral("Export Throughput"),
               QStringLiteral("%1 fps").arg(exportStats.value(QStringLiteral("fps")).toDouble(), 0, 'f', 1));
        addRow(QStringLiteral("Export Render Stage"),
               formatStage(exportStats, QStringLiteral("render_stage_ms"), QStringLiteral("render_stage_per_frame_ms")));
        addRow(QStringLiteral("Export GPU Readback"),
               formatStage(exportStats, QStringLiteral("gpu_readback_ms"), QStringLiteral("gpu_readback_per_frame_ms")));
        addRow(QStringLiteral("Export Encode Stage"),
               formatStage(exportStats, QStringLiteral("encode_stage_ms"), QStringLiteral("encode_stage_per_frame_ms")));
    }

    if (m_lastDecodeBenchmark.isEmpty()) {
        addRow(QStringLiteral("Last Benchmark"), QStringLiteral("Not run yet"));
    } else if (!m_lastDecodeBenchmark.value(QStringLiteral("success")).toBool()) {
        addRow(QStringLiteral("Last Benchmark"), QStringLiteral("Failed"));
        addRow(QStringLiteral("Benchmark Error"),
               m_lastDecodeBenchmark.value(QStringLiteral("error")).toString(QStringLiteral("unknown")));
    } else {
        addRow(QStringLiteral("Last Benchmark Clip"),
               m_lastDecodeBenchmark.value(QStringLiteral("clip_label")).toString());
        addRow(QStringLiteral("Last Benchmark Path"),
               m_lastDecodeBenchmark.value(QStringLiteral("path")).toString());
        addRow(QStringLiteral("Last Benchmark Codec"),
               m_lastDecodeBenchmark.value(QStringLiteral("codec")).toString());
        addRow(QStringLiteral("Last Benchmark Decode Path"),
               m_lastDecodeBenchmark.value(QStringLiteral("decode_path")).toString());
        addRow(QStringLiteral("Last Benchmark Frames Decoded"),
               QString::number(m_lastDecodeBenchmark.value(QStringLiteral("frames_decoded")).toInt()));
        addRow(QStringLiteral("Last Benchmark Throughput"),
               QStringLiteral("%1 fps").arg(m_lastDecodeBenchmark.value(QStringLiteral("fps")).toDouble(), 0, 'f', 1));
    }

    m_widgets.profileSummaryTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        auto* nameItem = new QTableWidgetItem(rows[row].first);
        auto* valueItem = new QTableWidgetItem(rows[row].second);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        m_widgets.profileSummaryTable->setItem(row, 0, nameItem);
        m_widgets.profileSummaryTable->setItem(row, 1, valueItem);
    }
    m_widgets.profileSummaryTable->resizeRowsToContents();
}

QStringList ProfileTab::availableHardwareDeviceTypes() const
{
    QStringList types;
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types(type)) {
        if (const char* name = av_hwdevice_get_type_name(type)) {
            types.push_back(QString::fromLatin1(name));
        }
    }
    return types;
}
