#include "project_manager.h"

// NOTE:
// This is the biggest split. The original methods below are preserved as an
// extraction pack because they are tightly coupled to EditorWindow state and UI.
//
// The clean next step is to move these onto either:
//   - a ProjectManager that owns only persistence/history state, plus
//   - a small EditorWindow bridge for widget/timeline access.
//
// Original extracted methods follow.

ProjectManager::ProjectManager(EditorWindow *owner)
    : m_owner(owner)
{
}

/*
QString projectsDirPath() const
    {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("projects"));
    }

QString currentProjectMarkerPath() const
    {
        return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
    }

QString currentProjectIdOrDefault() const
    {
        return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
    }

QString projectPath(const QString &projectId) const
    {
        return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
    }

QString stateFilePathForProject(const QString &projectId) const
    {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
    }

QString historyFilePathForProject(const QString &projectId) const
    {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
    }

QString stateFilePath() const
    {
        return stateFilePathForProject(currentProjectIdOrDefault());
    }

QString historyFilePath() const
    {
        return historyFilePathForProject(currentProjectIdOrDefault());
    }

QString sanitizedProjectId(const QString &name) const
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

void ensureProjectsDirectory() const
    {
        QDir().mkpath(projectsDirPath());
    }

QStringList availableProjectIds() const
    {
        ensureProjectsDirectory();
        const QFileInfoList entries = QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        QStringList ids;
        ids.reserve(entries.size());
        for (const QFileInfo &entry : entries)
        {
            ids.push_back(entry.fileName());
        }
        return ids;
    }

void ensureDefaultProjectExists() const
    {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(QStringLiteral("default")));
    }

void loadProjectsFromFolders()
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

void saveCurrentProjectMarker()
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

QString currentProjectName() const
    {
        return currentProjectIdOrDefault();
    }

void refreshProjectsList()
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

void switchToProject(const QString &projectId)
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

void createProject()
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

bool saveProjectPayload(const QString &projectId,
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

void saveProjectAs()
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
        const QByteArray statePayload = QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
        QJsonObject historyRoot;
        historyRoot[QStringLiteral("index")] = m_historyIndex;
        historyRoot[QStringLiteral("entries")] = m_historyEntries;
        const QByteArray historyPayload = QJsonDocument(historyRoot).toJson(QJsonDocument::Indented);

        if (!saveProjectPayload(newProjectId, statePayload, historyPayload))
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Save Project As Failed"),
                                 QStringLiteral("Could not write the new project files."));
            return;
        }

        switchToProject(newProjectId);
    }

void renameProject(const QString &projectId)
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

QJsonObject buildStateJson() const
    {
        QJsonObject root;
        root[QStringLiteral("explorerRoot")] = m_explorerPane ? m_explorerPane->currentRootPath() : QString();
        root[QStringLiteral("explorerGalleryPath")] = m_explorerPane ? m_explorerPane->galleryPath() : QString();
        root[QStringLiteral("currentFrame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
        root[QStringLiteral("playing")] = m_playbackTimer.isActive();
        root[QStringLiteral("selectedClipId")] = m_timeline ? m_timeline->selectedClipId() : QString();
        QJsonArray expandedFolders;
        if (m_explorerPane) {
            for (const QString &path : m_explorerPane->currentExpandedExplorerPaths()) {
                expandedFolders.push_back(path);
            }
        }
        root[QStringLiteral("explorerExpandedFolders")] = expandedFolders;
        root[QStringLiteral("outputWidth")] = m_outputWidthSpin ? m_outputWidthSpin->value() : 1080;
        root[QStringLiteral("outputHeight")] = m_outputHeightSpin ? m_outputHeightSpin->value() : 1920;
        root[QStringLiteral("outputFormat")] =
            m_outputFormatCombo ? m_outputFormatCombo->currentData().toString() : QStringLiteral("mp4");
        root[QStringLiteral("speechFilterEnabled")] =
            m_speechFilterEnabledCheckBox ? m_speechFilterEnabledCheckBox->isChecked() : false;
        root[QStringLiteral("transcriptPrependMs")] = m_transcriptPrependMs;
        root[QStringLiteral("transcriptPostpendMs")] = m_transcriptPostpendMs;
        root[QStringLiteral("selectedInspectorTab")] =
            m_inspectorTabs ? m_inspectorTabs->currentIndex() : 0;
        root[QStringLiteral("timelineZoom")] = m_timeline ? m_timeline->timelineZoom() : 4.0;
        root[QStringLiteral("timelineVerticalScroll")] =
            m_timeline ? m_timeline->verticalScrollOffset() : 0;
        root[QStringLiteral("exportStartFrame")] = m_timeline ? static_cast<qint64>(m_timeline->exportStartFrame()) : 0;
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

void scheduleSaveState()
    {
        if (m_loadingState || !m_timeline)
        {
            return;
        }
        m_stateSaveTimer.start();
    }

void saveStateNow()
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

void saveHistoryNow()
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

        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            return;
        }
        file.commit();
    }

void pushHistorySnapshot()
    {
        if (m_loadingState || m_restoringHistory || !m_timeline)
        {
            return;
        }

        const QJsonObject snapshot = buildStateJson();
        if (m_historyIndex >= 0 && m_historyIndex < m_historyEntries.size() &&
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

void applyStateJson(const QJsonObject &root)
    {
        m_loadingState = true;

        QString rootPath = root.value(QStringLiteral("explorerRoot")).toString(QDir::currentPath());
        QString galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
        const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
        const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
        const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
        const bool speechFilterEnabled =
            root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
        const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(0);
        const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(0);
        const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
        const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
        const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
        const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
        const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
        QVector<ExportRangeSegment> loadedExportRanges;
        const QJsonArray exportRanges = root.value(QStringLiteral("exportRanges")).toArray();
        loadedExportRanges.reserve(exportRanges.size());
        for (const QJsonValue &value : exportRanges)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject obj = value.toObject();
            ExportRangeSegment range;
            range.startFrame = qMax<int64_t>(0, obj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
            range.endFrame = qMax<int64_t>(0, obj.value(QStringLiteral("endFrame")).toVariant().toLongLong());
            loadedExportRanges.push_back(range);
        }
        QStringList expandedExplorerPaths;
        for (const QJsonValue &value : root.value(QStringLiteral("explorerExpandedFolders")).toArray())
        {
            const QString path = value.toString();
            if (!path.isEmpty())
            {
                expandedExplorerPaths.push_back(path);
            }
        }
        QVector<TimelineClip> loadedClips;
        QVector<RenderSyncMarker> loadedRenderSyncMarkers;
        const int64_t currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
        const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString();
        QVector<TimelineTrack> loadedTracks;

        const QJsonArray clips = root.value(QStringLiteral("timeline")).toArray();
        loadedClips.reserve(clips.size());
        for (const QJsonValue &value : clips)
        {
            if (!value.isObject())
                continue;
            TimelineClip clip = clipFromJson(value.toObject());
            if (clip.trackIndex < 0)
            {
                clip.trackIndex = loadedClips.size();
            }
            if (!clip.filePath.isEmpty())
            {
                loadedClips.push_back(clip);
            }
        }

        const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
        loadedTracks.reserve(tracks.size());
        for (int i = 0; i < tracks.size(); ++i)
        {
            const QJsonObject obj = tracks.at(i).toObject();
            TimelineTrack track;
            track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
            track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
            loadedTracks.push_back(track);
        }

        const QJsonArray renderSyncMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
        loadedRenderSyncMarkers.reserve(renderSyncMarkers.size());
        for (const QJsonValue &value : renderSyncMarkers)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject obj = value.toObject();
            RenderSyncMarker marker;
            marker.clipId = obj.value(QStringLiteral("clipId")).toString();
            marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
            marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
            marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
            loadedRenderSyncMarkers.push_back(marker);
        }

        const QString resolvedRootPath = QDir(rootPath).absolutePath();
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(resolvedRootPath);
            m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
        }
        if (m_outputWidthSpin)
        {
            QSignalBlocker block(m_outputWidthSpin);
            m_outputWidthSpin->setValue(outputWidth);
        }
        if (m_outputHeightSpin)
        {
            QSignalBlocker block(m_outputHeightSpin);
            m_outputHeightSpin->setValue(outputHeight);
        }
        if (m_outputFormatCombo)
        {
            QSignalBlocker block(m_outputFormatCombo);
            const int formatIndex = m_outputFormatCombo->findData(outputFormat);
            if (formatIndex >= 0)
            {
                m_outputFormatCombo->setCurrentIndex(formatIndex);
            }
        }
        if (m_speechFilterEnabledCheckBox)
        {
            QSignalBlocker block(m_speechFilterEnabledCheckBox);
            m_speechFilterEnabledCheckBox->setChecked(speechFilterEnabled);
        }
        m_transcriptPrependMs = transcriptPrependMs;
        m_transcriptPostpendMs = transcriptPostpendMs;
        if (m_transcriptPrependMsSpin)
        {
            QSignalBlocker block(m_transcriptPrependMsSpin);
            m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs);
        }
        if (m_transcriptPostpendMsSpin)
        {
            QSignalBlocker block(m_transcriptPostpendMsSpin);
            m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs);
        }
        if (m_inspectorTabs && m_inspectorTabs->count() > 0)
        {
            QSignalBlocker block(m_inspectorTabs);
            m_inspectorTabs->setCurrentIndex(qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1));
        }
        if (m_preview)
        {
            m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        }

        m_timeline->setTracks(loadedTracks);
        m_timeline->setClips(loadedClips);
        m_timeline->setTimelineZoom(timelineZoom);
        m_timeline->setVerticalScrollOffset(timelineVerticalScroll);
        if (!loadedExportRanges.isEmpty())
        {
            m_timeline->setExportRanges(loadedExportRanges);
        }
        else
        {
            m_timeline->setExportRange(exportStartFrame, exportEndFrame > 0 ? exportEndFrame : m_timeline->totalFrames());
        }
        m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
        m_timeline->setSelectedClipId(selectedClipId);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setExportRanges(effectivePlaybackRanges());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_preview->setSelectedClipId(selectedClipId);
        if (m_audioEngine)
        {
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->setExportRanges(effectivePlaybackRanges());
            m_audioEngine->seek(currentFrame);
        }
        setCurrentFrame(currentFrame);

        m_playbackTimer.stop();
        m_fastPlaybackActive.store(false);
        m_preview->setPlaybackState(false);
        if (m_audioEngine)
        {
            m_audioEngine->stop();
        }

        m_loadingState = false;
        QTimer::singleShot(0, this, [this, resolvedRootPath]()
                           {
            if (m_explorerPane) {
                m_explorerPane->setInitialRootPath(resolvedRootPath);
            }
            refreshProjectsList();
            m_inspectorPane->refresh(); });
    }

void undoHistory()
    {
        if (m_historyIndex <= 0 || m_historyEntries.isEmpty())
        {
            return;
        }

        m_restoringHistory = true;
        m_historyIndex -= 1;
        applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
        m_restoringHistory = false;
        saveHistoryNow();
        scheduleSaveState();
    }
*/
