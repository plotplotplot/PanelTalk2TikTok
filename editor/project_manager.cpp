#include "project_manager.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
{
}

QString ProjectManager::projectsDirPath() const
{
    return QDir(QDir::currentPath()).filePath(QStringLiteral("projects"));
}

QString ProjectManager::currentProjectMarkerPath() const
{
    return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
}

QString ProjectManager::currentProjectIdOrDefault() const
{
    return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
}

QString ProjectManager::projectPath(const QString &projectId) const
{
    return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
}

QString ProjectManager::stateFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
}

QString ProjectManager::historyFilePathForProject(const QString &projectId) const
{
    return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
}

QString ProjectManager::stateFilePath() const
{
    return stateFilePathForProject(currentProjectIdOrDefault());
}

QString ProjectManager::historyFilePath() const
{
    return historyFilePathForProject(currentProjectIdOrDefault());
}

QString ProjectManager::sanitizedProjectId(const QString &name) const
{
    QString id = name.trimmed().toLower();
    for (QChar &ch : id) {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))) {
            ch = QLatin1Char('-');
        }
    }
    while (id.contains(QStringLiteral("--"))) {
        id.replace(QStringLiteral("--"), QStringLiteral("-"));
    }
    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (id.isEmpty()) {
        id = QStringLiteral("project");
    }
    QString uniqueId = id;
    int suffix = 2;
    while (QFileInfo::exists(projectPath(uniqueId))) {
        uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
    }
    return uniqueId;
}

void ProjectManager::ensureProjectsDirectory() const
{
    QDir().mkpath(projectsDirPath());
}

QStringList ProjectManager::availableProjectIds() const
{
    ensureProjectsDirectory();
    const QFileInfoList entries = QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    QStringList ids;
    ids.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        ids.push_back(entry.fileName());
    }
    return ids;
}
void ProjectManager::loadProjectsFromFolders()
{
    ensureDefaultProjectExists();
    QFile markerFile(currentProjectMarkerPath());
    if (markerFile.open(QIODevice::ReadOnly)) {
        m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
    }
    const QStringList projectIds = availableProjectIds();
    if (projectIds.isEmpty()) {
        m_currentProjectId = QStringLiteral("default");
        return;
    }
    if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId)) {
        m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                                 ? QStringLiteral("default")
                                 : projectIds.constFirst();
    }
    QSaveFile marker(currentProjectMarkerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QByteArray payload = m_currentProjectId.toUtf8();
        if (marker.write(payload) == payload.size()) {
            marker.commit();
        } else {
            marker.cancelWriting();
        }
    }
}

void ProjectManager::saveCurrentProjectMarker()
{
    ensureProjectsDirectory();
    QSaveFile file(currentProjectMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    const QByteArray payload = currentProjectIdOrDefault().toUtf8();
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QString ProjectManager::currentProjectName() const
{
    return currentProjectIdOrDefault();
}

void ProjectManager::refreshProjectsList()
{
    loadProjectsFromFolders();
    emit projectsListRefreshed();
}

void ProjectManager::switchToProject(const QString &projectId)
{
    if (projectId.isEmpty() || projectId == currentProjectIdOrDefault()) {
        refreshProjectsList();
        return;
    }
    
    m_currentProjectId = projectId;
    saveCurrentProjectMarker();
    emit projectChanged(projectId);
}

void ProjectManager::createProject()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("New Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               QStringLiteral("Untitled Project"),
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    const QString projectId = sanitizedProjectId(name);
    QDir().mkpath(projectPath(projectId));
    switchToProject(projectId);
}

bool ProjectManager::saveProjectPayload(const QString &projectId,
                                        const QByteArray &statePayload,
                                        const QByteArray &historyPayload)
{
    ensureProjectsDirectory();
    QDir().mkpath(projectPath(projectId));

    QSaveFile stateFile(stateFilePathForProject(projectId));
    if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (stateFile.write(statePayload) != statePayload.size()) {
        stateFile.cancelWriting();
        return false;
    }
    if (!stateFile.commit()) {
        return false;
    }

    QSaveFile historyFile(historyFilePathForProject(projectId));
    if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (historyFile.write(historyPayload) != historyPayload.size()) {
        historyFile.cancelWriting();
        return false;
    }
    return historyFile.commit();
}

void ProjectManager::saveProjectAs(const QString &currentName,
                                   std::function<QByteArray()> buildStateJson,
                                   const QJsonArray &historyEntries,
                                   int historyIndex)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("Save Project As"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               currentName == QStringLiteral("Default Project")
                                                   ? QStringLiteral("Untitled Project")
                                                   : currentName,
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    const QString newProjectId = sanitizedProjectId(name);
    const QByteArray statePayload = QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
    QJsonObject historyRoot;
    historyRoot[QStringLiteral("index")] = historyIndex;
    historyRoot[QStringLiteral("entries")] = historyEntries;
    const QByteArray historyPayload = QJsonDocument(historyRoot).toJson(QJsonDocument::Indented);

    if (!saveProjectPayload(newProjectId, statePayload, historyPayload)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Save Project As Failed"),
                             QStringLiteral("Could not write the new project files."));
        return;
    }

    switchToProject(newProjectId);
}

void ProjectManager::renameProject(const QString &projectId)
{
    if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId))) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(nullptr,
                                               QStringLiteral("Rename Project"),
                                               QStringLiteral("Project name"),
                                               QLineEdit::Normal,
                                               projectId,
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    const QString renamedProjectId = sanitizedProjectId(name);
    if (renamedProjectId == projectId) {
        return;
    }
    QDir projectsDir(projectsDirPath());
    if (!projectsDir.rename(projectId, renamedProjectId)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Rename Project Failed"),
                             QStringLiteral("Could not rename the project folder."));
        return;
    }
    if (m_currentProjectId == projectId) {
        m_currentProjectId = renamedProjectId;
        saveCurrentProjectMarker();
    }
    refreshProjectsList();
}

