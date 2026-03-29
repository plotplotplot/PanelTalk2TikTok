// project_state.cpp
#include "editor.h"
#include "clip_serialization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>

using namespace editor;

void EditorWindow::loadState()
{
    loadProjectsFromFolders();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;
    m_lastSavedState.clear();

    QJsonObject root;
    QFile historyFile(historyFilePath());
    if (historyFile.open(QIODevice::ReadOnly))
    {
        const QJsonObject historyRoot = QJsonDocument::fromJson(historyFile.readAll()).object();
        m_historyEntries = historyRoot.value(QStringLiteral("entries")).toArray();
        m_historyIndex = historyRoot.value(QStringLiteral("index")).toInt(m_historyEntries.size() - 1);
        if (!m_historyEntries.isEmpty())
        {
            m_historyIndex = qBound(0, m_historyIndex, m_historyEntries.size() - 1);
            root = m_historyEntries.at(m_historyIndex).toObject();
        }
    }

    if (root.isEmpty())
    {
        QFile file(stateFilePath());
        if (file.open(QIODevice::ReadOnly))
        {
            root = QJsonDocument::fromJson(file.readAll()).object();
        }
    }

    applyStateJson(root);
    if (m_historyEntries.isEmpty())
    {
        pushHistorySnapshot();
    }

    if (m_pendingSaveAfterLoad)
    {
        m_pendingSaveAfterLoad = false;
        scheduleSaveState();
    }
    else
    {
        scheduleSaveState();
    }
}

QString EditorWindow::projectsDirPath() const
{
    return QDir(QDir::currentPath()).filePath(QStringLiteral("projects"));
}

QString EditorWindow::currentProjectMarkerPath() const
{
    return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
}

QString EditorWindow::currentProjectIdOrDefault() const
{
    return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
}

QString EditorWindow::projectPath(const QString &projectId) const
{
    return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
}

QString EditorWindow::stateFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
}

QString EditorWindow::historyFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
}

QString EditorWindow::stateFilePath() const
{
    return stateFilePathForProject(currentProjectIdOrDefault());
}

QString EditorWindow::historyFilePath() const
{
    return historyFilePathForProject(currentProjectIdOrDefault());
}

QString EditorWindow::sanitizedProjectId(const QString &name) const
{
    QString id = name.trimmed().toLower();
    for (QChar &ch : id)
    {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')))
        {
            ch = QLatin1Char('-');
        }
    }

    while (id.contains(QStringLiteral("--")))
    {
        id.replace(QStringLiteral("--"), QStringLiteral("-"));
    }

    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (id.isEmpty())
    {
        id = QStringLiteral("project");
    }

    QString uniqueId = id;
    int suffix = 2;
    while (QFileInfo::exists(projectPath(uniqueId)))
    {
        uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
    }
    return uniqueId;
}

void EditorWindow::ensureProjectsDirectory() const
{
    QDir().mkpath(projectsDirPath());
}

QStringList EditorWindow::availableProjectIds() const
{
    ensureProjectsDirectory();
    const QFileInfoList entries =
        QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::IgnoreCase);

    QStringList ids;
    ids.reserve(entries.size());
    for (const QFileInfo &entry : entries)
    {
        ids.push_back(entry.fileName());
    }
    return ids;
}

void EditorWindow::ensureDefaultProjectExists() const
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(QStringLiteral("default")));
}

void EditorWindow::loadProjectsFromFolders()
{
    ensureDefaultProjectExists();

    QFile markerFile(currentProjectMarkerPath());
    if (markerFile.open(QIODevice::ReadOnly))
    {
        m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
    }

    const QStringList projectIds = availableProjectIds();
    if (projectIds.isEmpty())
    {
        m_currentProjectId = QStringLiteral("default");
        return;
    }

    if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId))
    {
        m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                                 ? QStringLiteral("default")
                                 : projectIds.constFirst();
    }

    QSaveFile marker(currentProjectMarkerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        const QByteArray payload = m_currentProjectId.toUtf8();
        if (marker.write(payload) == payload.size())
        {
            marker.commit();
        }
        else
        {
            marker.cancelWriting();
        }
    }
}

void EditorWindow::saveCurrentProjectMarker()
{
    ensureProjectsDirectory();

    QSaveFile file(currentProjectMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    const QByteArray payload = currentProjectIdOrDefault().toUtf8();
    if (file.write(payload) != payload.size())
    {
        file.cancelWriting();
        return;
    }

    file.commit();
}

QString EditorWindow::currentProjectName() const
{
    return currentProjectIdOrDefault();
}

void EditorWindow::refreshProjectsList()
{
    if (!m_projectsList)
    {
        return;
    }

    loadProjectsFromFolders();
    m_updatingProjectsList = true;
    m_projectsList->clear();

    const QStringList projectIds = availableProjectIds();
    for (const QString &projectId : projectIds)
    {
        auto *item = new QListWidgetItem(projectId, m_projectsList);
        item->setData(Qt::UserRole, projectId);
        item->setToolTip(QDir::toNativeSeparators(projectPath(projectId)));
        if (projectId == currentProjectIdOrDefault())
        {
            item->setSelected(true);
        }
    }

    if (m_projectSectionLabel)
    {
        m_projectSectionLabel->setText(QStringLiteral("PROJECTS  %1").arg(currentProjectName()));
    }

    m_updatingProjectsList = false;
}

void EditorWindow::switchToProject(const QString &projectId)
{
    if (projectId.isEmpty() || projectId == currentProjectIdOrDefault())
    {
        refreshProjectsList();
        return;
    }

    saveStateNow();
    saveHistoryNow();

    m_currentProjectId = projectId;
    m_lastSavedState.clear();
    m_historyEntries = QJsonArray();
    m_historyIndex = -1;

    saveCurrentProjectMarker();
    loadState();
    refreshProjectsList();
    m_inspectorPane->refresh();
}

void EditorWindow::createProject()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("New Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               QStringLiteral("Untitled Project"),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    const QString projectId = sanitizedProjectId(name);
    QDir().mkpath(projectPath(projectId));
    switchToProject(projectId);
}

bool EditorWindow::saveProjectPayload(const QString &projectId,
                                      const QByteArray &statePayload,
                                      const QByteArray &historyPayload)
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(projectId));

    QSaveFile stateFile(stateFilePathForProject(projectId));
    if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    if (stateFile.write(statePayload) != statePayload.size())
    {
        stateFile.cancelWriting();
        return false;
    }
    if (!stateFile.commit())
    {
        return false;
    }

    QSaveFile historyFile(historyFilePathForProject(projectId));
    if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    if (historyFile.write(historyPayload) != historyPayload.size())
    {
        historyFile.cancelWriting();
        return false;
    }

    return historyFile.commit();
}

void EditorWindow::saveProjectAs()
{
    if (!m_timeline)
    {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Save Project As"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               currentProjectName() == QStringLiteral("Default Project")
                                                   ? QStringLiteral("Untitled Project")
                                                   : currentProjectName(),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    saveStateNow();
    saveHistoryNow();

    const QString newProjectId = sanitizedProjectId(name);
    const QByteArray statePayload =
        QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);

    QJsonObject historyRoot;
    historyRoot[QStringLiteral("index")] = m_historyIndex;
    historyRoot[QStringLiteral("entries")] = m_historyEntries;
    const QByteArray historyPayload =
        QJsonDocument(historyRoot).toJson(QJsonDocument::Indented);

    if (!saveProjectPayload(newProjectId, statePayload, historyPayload))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Save Project As Failed"),
                             QStringLiteral("Could not write the new project files."));
        return;
    }

    switchToProject(newProjectId);
}

void EditorWindow::renameProject(const QString &projectId)
{
    if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId)))
    {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Rename Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               projectId,
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }

    const QString renamedProjectId = sanitizedProjectId(name);
    if (renamedProjectId == projectId)
    {
        return;
    }

    QDir projectsDir(projectsDirPath());
    if (!projectsDir.rename(projectId, renamedProjectId))
    {
        QMessageBox::warning(this,
                             QStringLiteral("Rename Project Failed"),
                             QStringLiteral("Could not rename the project folder."));
        return;
    }

    if (m_currentProjectId == projectId)
    {
        m_currentProjectId = renamedProjectId;
        saveCurrentProjectMarker();
    }

    refreshProjectsList();
}

QJsonObject EditorWindow::buildStateJson() const
{
    QJsonObject root;
    root[QStringLiteral("explorerRoot")] =
        m_explorerPane ? m_explorerPane->currentRootPath() : QString();
    root[QStringLiteral("explorerGalleryPath")] =
        m_explorerPane ? m_explorerPane->galleryPath() : QString();
    root[QStringLiteral("currentFrame")] =
        static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
    root[QStringLiteral("playing")] = m_playbackTimer.isActive();
    root[QStringLiteral("selectedClipId")] =
        m_timeline ? m_timeline->selectedClipId() : QString();

    QJsonArray expandedFolders;
    if (m_explorerPane)
    {
        for (const QString &path : m_explorerPane->currentExpandedExplorerPaths())
        {
            expandedFolders.push_back(path);
        }
    }
    root[QStringLiteral("explorerExpandedFolders")] = expandedFolders;

    root[QStringLiteral("outputWidth")] =
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080;
    root[QStringLiteral("outputHeight")] =
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920;
    root[QStringLiteral("outputFormat")] =
        m_outputFormatCombo ? m_outputFormatCombo->currentData().toString()
                            : QStringLiteral("mp4");
    root[QStringLiteral("lastRenderOutputPath")] = m_lastRenderOutputPath;
    root[QStringLiteral("renderUseProxies")] =
        m_renderUseProxiesCheckBox ? m_renderUseProxiesCheckBox->isChecked() : false;
    root[QStringLiteral("previewHideOutsideOutput")] =
        m_previewHideOutsideOutputCheckBox ? m_previewHideOutsideOutputCheckBox->isChecked() : false;
    root[QStringLiteral("speechFilterEnabled")] =
        m_speechFilterEnabledCheckBox ? m_speechFilterEnabledCheckBox->isChecked() : false;
    root[QStringLiteral("transcriptPrependMs")] = m_transcriptPrependMs;
    root[QStringLiteral("transcriptPostpendMs")] = m_transcriptPostpendMs;
    root[QStringLiteral("speechFilterFadeSamples")] = m_speechFilterFadeSamples;
    root[QStringLiteral("transcriptFollowCurrentWord")] =
        m_transcriptFollowCurrentWordCheckBox
            ? m_transcriptFollowCurrentWordCheckBox->isChecked()
            : true;
    root[QStringLiteral("gradingFollowCurrent")] =
        m_gradingFollowCurrentCheckBox ? m_gradingFollowCurrentCheckBox->isChecked() : true;
    root[QStringLiteral("gradingAutoScroll")] =
        m_gradingAutoScrollCheckBox ? m_gradingAutoScrollCheckBox->isChecked() : true;
    root[QStringLiteral("keyframesFollowCurrent")] =
        m_keyframesFollowCurrentCheckBox ? m_keyframesFollowCurrentCheckBox->isChecked() : true;
    root[QStringLiteral("keyframesAutoScroll")] =
        m_keyframesAutoScrollCheckBox ? m_keyframesAutoScrollCheckBox->isChecked() : true;
    root[QStringLiteral("selectedInspectorTab")] =
        m_inspectorTabs ? m_inspectorTabs->currentIndex() : 0;
    root[QStringLiteral("timelineZoom")] =
        m_timeline ? m_timeline->timelineZoom() : 4.0;
    root[QStringLiteral("timelineVerticalScroll")] =
        m_timeline ? m_timeline->verticalScrollOffset() : 0;
    root[QStringLiteral("exportStartFrame")] =
        m_timeline ? static_cast<qint64>(m_timeline->exportStartFrame()) : 0;
    root[QStringLiteral("exportEndFrame")] =
        m_timeline ? static_cast<qint64>(m_timeline->exportEndFrame()) : 300;

    QJsonArray exportRanges;
    if (m_timeline)
    {
        for (const ExportRangeSegment &range : m_timeline->exportRanges())
        {
            QJsonObject rangeObj;
            rangeObj[QStringLiteral("startFrame")] = static_cast<qint64>(range.startFrame);
            rangeObj[QStringLiteral("endFrame")] = static_cast<qint64>(range.endFrame);
            exportRanges.push_back(rangeObj);
        }
    }
    root[QStringLiteral("exportRanges")] = exportRanges;

    QJsonArray timeline;
    if (m_timeline)
    {
        for (const TimelineClip &clip : m_timeline->clips())
        {
            timeline.push_back(clipToJson(clip));
        }
    }
    root[QStringLiteral("timeline")] = timeline;

    QJsonArray renderSyncMarkers;
    if (m_timeline)
    {
        for (const RenderSyncMarker &marker : m_timeline->renderSyncMarkers())
        {
            QJsonObject markerObj;
            markerObj[QStringLiteral("clipId")] = marker.clipId;
            markerObj[QStringLiteral("frame")] = static_cast<qint64>(marker.frame);
            markerObj[QStringLiteral("action")] = renderSyncActionToString(marker.action);
            markerObj[QStringLiteral("count")] = marker.count;
            renderSyncMarkers.push_back(markerObj);
        }
    }
    root[QStringLiteral("renderSyncMarkers")] = renderSyncMarkers;

    QJsonArray tracks;
    if (m_timeline)
    {
        for (const TimelineTrack &track : m_timeline->tracks())
        {
            QJsonObject trackObj;
            trackObj[QStringLiteral("name")] = track.name;
            trackObj[QStringLiteral("height")] = track.height;
            tracks.push_back(trackObj);
        }
    }
    root[QStringLiteral("tracks")] = tracks;

    return root;
}

void EditorWindow::scheduleSaveState()
{
    if (m_loadingState || !m_timeline)
    {
        return;
    }
    m_stateSaveTimer.start();
}

void EditorWindow::saveStateNow()
{
    if (!m_timeline)
    {
        return;
    }
    if (m_loadingState)
    {
        m_pendingSaveAfterLoad = true;
        return;
    }

    m_stateSaveTimer.stop();
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(currentProjectIdOrDefault()));

    const QByteArray serializedState =
        QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
    if (serializedState == m_lastSavedState)
    {
        return;
    }

    QSaveFile file(stateFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    if (file.write(serializedState) != serializedState.size())
    {
        file.cancelWriting();
        return;
    }

    if (!file.commit())
    {
        return;
    }

    m_lastSavedState = serializedState;
}

void EditorWindow::saveHistoryNow()
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(currentProjectIdOrDefault()));

    QJsonObject root;
    root[QStringLiteral("index")] = m_historyIndex;
    root[QStringLiteral("entries")] = m_historyEntries;

    QSaveFile file(historyFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }

    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size())
    {
        file.cancelWriting();
        return;
    }

    file.commit();
}

void EditorWindow::pushHistorySnapshot()
{
    if (m_loadingState || m_restoringHistory || !m_timeline)
    {
        return;
    }

    const QJsonObject snapshot = buildStateJson();
    if (m_historyIndex >= 0 &&
        m_historyIndex < m_historyEntries.size() &&
        m_historyEntries.at(m_historyIndex).toObject() == snapshot)
    {
        return;
    }

    while (m_historyEntries.size() > m_historyIndex + 1)
    {
        m_historyEntries.removeAt(m_historyEntries.size() - 1);
    }

    m_historyEntries.append(snapshot);
    if (m_historyEntries.size() > 200)
    {
        m_historyEntries.removeAt(0);
    }

    m_historyIndex = m_historyEntries.size() - 1;
    saveHistoryNow();
}
