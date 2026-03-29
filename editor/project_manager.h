#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>

class ProjectManager : public QObject
{
    Q_OBJECT

public:
    explicit ProjectManager(QObject *parent = nullptr);
    ~ProjectManager() override = default;

    // Project path helpers
    QString projectsDirPath() const;
    QString currentProjectMarkerPath() const;
    QString currentProjectIdOrDefault() const;
    QString projectPath(const QString &projectId) const;
    QString stateFilePathForProject(const QString &projectId) const;
    QString historyFilePathForProject(const QString &projectId) const;
    QString stateFilePath() const;
    QString historyFilePath() const;
    
    QString sanitizedProjectId(const QString &name) const;
    void ensureProjectsDirectory() const;
    QStringList availableProjectIds() const;
    void ensureDefaultProjectExists() const;
    
    void loadProjectsFromFolders();
    void saveCurrentProjectMarker();
    QString currentProjectName() const;
    void refreshProjectsList();
    void switchToProject(const QString &projectId);
    void createProject();
    bool saveProjectPayload(const QString &projectId, const QByteArray &statePayload, const QByteArray &historyPayload);
    void saveProjectAs(const QString &currentName, std::function<QByteArray()> buildStateJson, 
                       const QJsonArray &historyEntries, int historyIndex);
    void renameProject(const QString &projectId);

signals:
    void projectChanged(const QString &projectId);
    void projectsListRefreshed();

private:
    QString m_currentProjectId;
};
