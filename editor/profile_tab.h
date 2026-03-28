#pragma once

#include <QObject>
#include <QTableWidget>
#include <QPushButton>
#include <QString>
#include <QJsonObject>
#include <functional>

#include "editor_shared.h"

class ProfileTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QTableWidget* profileSummaryTable = nullptr;
        QPushButton* profileBenchmarkButton = nullptr;
    };

    struct Dependencies
    {
        std::function<QJsonObject()> profilingSnapshot;
        std::function<bool(TimelineClip*)> profileBenchmarkClip;
        std::function<QString(const TimelineClip&)> playbackMediaPath;
        std::function<void()> refreshInspector;
    };

    explicit ProfileTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~ProfileTab() override = default;

    void wire();
    void refresh();
    void runDecodeBenchmark();

private slots:
    void onBenchmarkClicked();

private:
    void updateProfileTable(const QJsonObject& previewProfile,
                            const QJsonObject& decoderProfile,
                            const QJsonObject& cacheProfile,
                            const QJsonObject& playbackPipelineProfile,
                            const QJsonObject& memoryBudgetProfile);
    QStringList availableHardwareDeviceTypes() const;
    
    Widgets m_widgets;
    Dependencies m_deps;
    QJsonObject m_lastDecodeBenchmark;
};