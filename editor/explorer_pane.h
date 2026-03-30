// explorer_pane.h
#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QHash>
#include <QPoint>
#include <QThread>
#include <QMutex>
#include <QImage>
#include <QSet>

class QFileSystemModel;
class QTreeView;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QToolButton;
class QStackedWidget;
class PreviewWindow;

// Background worker for generating video thumbnails without blocking the UI.
class ThumbnailWorker final : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailWorker(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void generateThumbnail(const QString &filePath, const QString &cacheKey);

signals:
    void thumbnailReady(const QString &filePath, const QString &cacheKey, const QImage &image);
};

class ExplorerPane final : public QWidget
{
    Q_OBJECT

public:
    explicit ExplorerPane(QWidget *parent = nullptr);
    ~ExplorerPane() override;

    void setPreviewWindow(PreviewWindow *preview);
    void setInitialRootPath(const QString &path);
    QString currentRootPath() const;
    QString galleryPath() const;

    QStringList currentExpandedExplorerPaths() const;
    void restoreExpandedExplorerPaths(const QStringList &paths);

signals:
    void fileActivated(const QString &filePath);
    void folderRootChanged(const QString &path);
    void galleryPathChanged(const QString &path);
    void transcriptionRequested(const QString &filePath, const QString &label);
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setExplorerRootPath(const QString &path, bool emitSignals = true);
    void setExplorerGalleryPath(const QString &folderPath, bool emitSignals = true);
    void populateExplorerGallery(const QString &folderPath);
    void chooseExplorerRoot();

    QPixmap previewPixmapForFile(const QString &filePath) const;
    QImage decodeVideoThumbnail(const QString &filePath) const;
    void showExplorerHoverPreview(const QString &filePath);
    void hideExplorerHoverPreview();
    void onThumbnailReady(const QString &filePath, const QString &cacheKey, const QImage &image);

    QWidget *buildUi();
    QWidget *buildTreePage();
    QWidget *buildGalleryPage();

private:
    QFileSystemModel *m_fsModel = nullptr;
    QTreeView *m_tree = nullptr;
    QStackedWidget *m_explorerStack = nullptr;
    QListWidget *m_galleryList = nullptr;
    QLabel *m_explorerHoverPreview = nullptr;
    QPushButton *m_folderPickerButton = nullptr;
    QToolButton *m_refreshExplorerButton = nullptr;
    QToolButton *m_galleryBackButton = nullptr;
    QLabel *m_rootPathLabel = nullptr;
    QLabel *m_galleryTitleLabel = nullptr;

    PreviewWindow *m_preview = nullptr;
    QString m_currentRootPath;
    QString m_galleryFolderPath;
    mutable QHash<QString, QPixmap> m_previewPixmapCache;
    mutable QSet<QString> m_pendingThumbnails;
    QThread *m_thumbnailThread = nullptr;
    ThumbnailWorker *m_thumbnailWorker = nullptr;
    QString m_hoverFilePath;
    QPoint m_treeDragStartPos;
    QPoint m_galleryDragStartPos;
};
