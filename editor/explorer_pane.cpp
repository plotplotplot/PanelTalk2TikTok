// explorer_pane.cpp
#include "explorer_pane.h"
#include "preview.h"
#include "render.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDrag>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {
QIcon sequenceFolderIcon(const QSize& size = QSize(48, 48))
{
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect bodyRect(4, 10, size.width() - 8, size.height() - 14);
    const QRect tabRect(8, 5, size.width() / 2, 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#314968")));
    painter.drawRoundedRect(bodyRect, 8, 8);
    painter.setBrush(QColor(QStringLiteral("#4b6f99")));
    painter.drawRoundedRect(tabRect, 6, 6);

    painter.setBrush(QColor(QStringLiteral("#dbe8ff")));
    for (int i = 0; i < 3; ++i) {
        const QRect frameRect(11 + (i * 10), 19 + (i * 3), 14, 18);
        painter.drawRoundedRect(frameRect, 3, 3);
    }

    painter.setPen(QColor(QStringLiteral("#eef5ff")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSizeF(8.5);
    painter.setFont(font);
    painter.drawText(QRect(0, size.height() - 17, size.width(), 12),
                     Qt::AlignCenter,
                     QStringLiteral("SEQ"));
    return QIcon(pixmap);
}

class SequenceAwareIconProvider final : public QFileIconProvider
{
public:
    QIcon icon(const QFileInfo& info) const override
    {
        if (info.isDir() && isImageSequencePath(info.absoluteFilePath())) {
            return sequenceFolderIcon();
        }
        return QFileIconProvider::icon(info);
    }
};
}

ExplorerPane::ExplorerPane(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildUi());

    setExplorerRootPath(QDir::currentPath(), false);
}

void ExplorerPane::setPreviewWindow(PreviewWindow *preview)
{
    m_preview = preview;
}

void ExplorerPane::setInitialRootPath(const QString &path)
{
    setExplorerRootPath(path, false);
}

QString ExplorerPane::currentRootPath() const
{
    return m_currentRootPath;
}

QString ExplorerPane::galleryPath() const
{
    return m_galleryFolderPath;
}

QStringList ExplorerPane::currentExpandedExplorerPaths() const
{
    QStringList expanded;
    if (!m_tree || !m_fsModel)
    {
        return expanded;
    }

    const QModelIndex rootIndex = m_tree->rootIndex();
    std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent)
    {
        const int rowCount = m_fsModel->rowCount(parent);
        for (int row = 0; row < rowCount; ++row)
        {
            const QModelIndex child = m_fsModel->index(row, 0, parent);
            if (!child.isValid())
            {
                continue;
            }

            const QFileInfo info = m_fsModel->fileInfo(child);
            if (!info.isDir())
            {
                continue;
            }

            if (m_tree->isExpanded(child))
            {
                expanded.push_back(info.absoluteFilePath());
                collect(child);
            }
        }
    };

    collect(rootIndex);
    return expanded;
}

void ExplorerPane::restoreExpandedExplorerPaths(const QStringList &paths)
{
    if (!m_tree || !m_fsModel)
    {
        return;
    }

    for (const QString &path : paths)
    {
        const QModelIndex index = m_fsModel->index(path);
        if (index.isValid())
        {
            m_tree->expand(index);
        }
    }
}

bool ExplorerPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == (m_tree ? m_tree->viewport() : nullptr))
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                m_treeDragStartPos = mouseEvent->pos();
            }
        }
        else if (event->type() == QEvent::MouseMove && m_tree && m_fsModel)
        {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons() & Qt::LeftButton)
            {
                if ((mouseEvent->pos() - m_treeDragStartPos).manhattanLength() >= QApplication::startDragDistance())
                {
                    const QModelIndex index = m_tree->indexAt(m_treeDragStartPos);
                    if (index.isValid())
                    {
                        const QFileInfo info = m_fsModel->fileInfo(index);
                        if (info.exists() && (info.isFile() || isImageSequencePath(info.absoluteFilePath())))
                        {
                            hideExplorerHoverPreview();
                            auto *mimeData = new QMimeData;
                            mimeData->setUrls({QUrl::fromLocalFile(info.absoluteFilePath())});
                            auto *drag = new QDrag(m_tree);
                            drag->setMimeData(mimeData);
                            drag->exec(Qt::CopyAction);
                            return true;
                        }
                    }
                }
            }
            if (!mouseEvent->buttons().testFlag(Qt::LeftButton))
            {
                const QModelIndex index = m_tree->indexAt(mouseEvent->pos());
                if (!index.isValid())
                {
                    hideExplorerHoverPreview();
                }
                else
                {
                    const QFileInfo info = m_fsModel->fileInfo(index);
                    if (!info.exists() || (!info.isFile() && !isImageSequencePath(info.absoluteFilePath())))
                    {
                        hideExplorerHoverPreview();
                    }
                }
            }
        }
        else if (event->type() == QEvent::Leave)
        {
            hideExplorerHoverPreview();
        }
    }
    else if (watched == (m_galleryList ? m_galleryList->viewport() : nullptr))
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                m_galleryDragStartPos = mouseEvent->pos();
            }
        }
        else if (event->type() == QEvent::MouseMove && m_galleryList)
        {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons() & Qt::LeftButton)
            {
                if ((mouseEvent->pos() - m_galleryDragStartPos).manhattanLength() >= QApplication::startDragDistance())
                {
                    QListWidgetItem *item = m_galleryList->itemAt(m_galleryDragStartPos);
                    if (item)
                    {
                        const QFileInfo info(item->data(Qt::UserRole).toString());
                        if (info.exists() && (info.isFile() || isImageSequencePath(info.absoluteFilePath())))
                        {
                            hideExplorerHoverPreview();
                            auto *mimeData = new QMimeData;
                            mimeData->setUrls({QUrl::fromLocalFile(info.absoluteFilePath())});
                            auto *drag = new QDrag(m_galleryList);
                            drag->setMimeData(mimeData);
                            drag->exec(Qt::CopyAction);
                            return true;
                        }
                    }
                }
            }
            if (!mouseEvent->buttons().testFlag(Qt::LeftButton))
            {
                QListWidgetItem *item = m_galleryList->itemAt(mouseEvent->pos());
                if (!item)
                {
                    hideExplorerHoverPreview();
                }
                else
                {
                    const QFileInfo info(item->data(Qt::UserRole).toString());
                    if (!info.exists() || (!info.isFile() && !isImageSequencePath(info.absoluteFilePath())))
                    {
                        hideExplorerHoverPreview();
                    }
                }
            }
        }
        else if (event->type() == QEvent::Leave)
        {
            hideExplorerHoverPreview();
        }
    }

    return QWidget::eventFilter(watched, event);
}

QWidget *ExplorerPane::buildUi()
{
    auto *pane = new QWidget(this);
    pane->setStyleSheet(
        QStringLiteral(
            "QWidget { background: #10161d; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 6px 10px; }"
            "QPushButton:hover, QToolButton:hover { background: #233142; }"
            "QLabel#explorerSectionTitle { color: #8fa3b8; font-weight: 700; letter-spacing: 1px; }"
            "QTreeView, QListWidget { background: #0c1015; border: 1px solid #202934; border-radius: 10px; }"));

    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);

    m_folderPickerButton = new QPushButton(QStringLiteral("Root…"), pane);
    m_refreshExplorerButton = new QToolButton(pane);
    m_refreshExplorerButton->setText(QStringLiteral("Refresh"));

    toolbar->addWidget(m_folderPickerButton, 1);
    toolbar->addWidget(m_refreshExplorerButton);
    layout->addLayout(toolbar);

    m_rootPathLabel = new QLabel(pane);
    m_rootPathLabel->setWordWrap(true);
    layout->addWidget(m_rootPathLabel);

    m_explorerStack = new QStackedWidget(pane);
    m_explorerStack->addWidget(buildTreePage());
    m_explorerStack->addWidget(buildGalleryPage());
    layout->addWidget(m_explorerStack, 1);

    connect(m_folderPickerButton, &QPushButton::clicked, this, &ExplorerPane::chooseExplorerRoot);
    connect(m_refreshExplorerButton, &QToolButton::clicked, this, [this]()
    {
        setExplorerRootPath(m_currentRootPath, true);
    });

    return pane;
}

QWidget *ExplorerPane::buildTreePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    m_fsModel->setIconProvider(new SequenceAwareIconProvider);

    m_tree = new QTreeView(page);
    m_tree->setModel(m_fsModel);
    m_tree->setHeaderHidden(false);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setAnimated(true);
    m_tree->setAlternatingRowColors(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setMouseTracking(true);
    m_tree->viewport()->installEventFilter(this);

    layout->addWidget(m_tree, 1);

    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index)
    {
        if (!index.isValid() || !m_fsModel)
        {
            return;
        }

        const QFileInfo info = m_fsModel->fileInfo(index);
        if (info.isDir() && !isImageSequencePath(info.absoluteFilePath()))
        {
            setExplorerGalleryPath(info.absoluteFilePath(), true);
        }
        else if (info.exists() && (info.isFile() || isImageSequencePath(info.absoluteFilePath())))
        {
            emit fileActivated(info.absoluteFilePath());
        }
    });

    connect(m_tree, &QTreeView::entered, this, [this](const QModelIndex &index)
    {
        if (!index.isValid() || !m_fsModel)
        {
            hideExplorerHoverPreview();
            return;
        }

        const QFileInfo info = m_fsModel->fileInfo(index);
        if (info.exists() && (info.isFile() || isImageSequencePath(info.absoluteFilePath())))
        {
            showExplorerHoverPreview(info.absoluteFilePath());
        }
        else
        {
            hideExplorerHoverPreview();
        }
    });

    connect(m_tree, &QTreeView::collapsed, this, [this]()
    {
        emit stateChanged();
    });

    connect(m_tree, &QTreeView::expanded, this, [this]()
    {
        emit stateChanged();
    });

    return page;
}

QWidget *ExplorerPane::buildGalleryPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    m_galleryBackButton = new QToolButton(page);
    m_galleryBackButton->setText(QStringLiteral("Back"));
    m_galleryTitleLabel = new QLabel(page);
    m_galleryTitleLabel->setWordWrap(true);

    topRow->addWidget(m_galleryBackButton);
    topRow->addWidget(m_galleryTitleLabel, 1);
    layout->addLayout(topRow);

    m_galleryList = new QListWidget(page);
    m_galleryList->setViewMode(QListView::IconMode);
    m_galleryList->setResizeMode(QListView::Adjust);
    m_galleryList->setMovement(QListView::Static);
    m_galleryList->setIconSize(QSize(48, 48));
    m_galleryList->setSpacing(8);
    m_galleryList->setDragEnabled(true);
    m_galleryList->setDragDropMode(QAbstractItemView::DragOnly);
    m_galleryList->setMouseTracking(true);
    m_galleryList->viewport()->installEventFilter(this);

    layout->addWidget(m_galleryList, 1);

    connect(m_galleryBackButton, &QToolButton::clicked, this, [this]()
    {
        m_explorerStack->setCurrentIndex(0);
        m_galleryFolderPath.clear();
        hideExplorerHoverPreview();
        emit galleryPathChanged(QString());
        emit stateChanged();
    });

    connect(m_galleryList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item)
    {
        if (!item)
        {
            return;
        }

        const QString path = item->data(Qt::UserRole).toString();
        const QFileInfo info(path);
        if (info.isDir() && !isImageSequencePath(path))
        {
            setExplorerGalleryPath(path, true);
        }
        else if (info.exists() && (info.isFile() || isImageSequencePath(path)))
        {
            emit fileActivated(path);
        }
    });

    connect(m_galleryList, &QListWidget::itemEntered, this, [this](QListWidgetItem *item)
    {
        if (!item)
        {
            hideExplorerHoverPreview();
            return;
        }

        const QFileInfo info(item->data(Qt::UserRole).toString());
        if (info.exists() && (info.isFile() || isImageSequencePath(info.absoluteFilePath())))
        {
            showExplorerHoverPreview(info.absoluteFilePath());
        }
        else
        {
            hideExplorerHoverPreview();
        }
    });

    connect(m_galleryList, &QListWidget::itemSelectionChanged, this, [this]()
    {
        if (m_galleryList && m_galleryList->selectedItems().isEmpty())
        {
            hideExplorerHoverPreview();
        }
    });

    connect(m_galleryList, &QAbstractItemView::viewportEntered, this, [this]()
    {
        hideExplorerHoverPreview();
    });

    return page;
}

void ExplorerPane::setExplorerRootPath(const QString &path, bool emitSignals)
{
    if (!m_fsModel || !m_tree)
    {
        return;
    }

    QString resolvedPath = path;
    if (resolvedPath.isEmpty() || !QFileInfo::exists(resolvedPath) || !QFileInfo(resolvedPath).isDir())
    {
        resolvedPath = QDir::currentPath();
    }

    m_currentRootPath = QDir(resolvedPath).absolutePath();
    const QModelIndex rootIndex = m_fsModel->setRootPath(m_currentRootPath);
    m_tree->setRootIndex(rootIndex);
    hideExplorerHoverPreview();

    if (m_rootPathLabel)
    {
        m_rootPathLabel->setText(m_currentRootPath);
    }

    if (!m_galleryFolderPath.isEmpty())
    {
        setExplorerGalleryPath(m_galleryFolderPath, false);
    }

    if (emitSignals)
    {
        emit folderRootChanged(m_currentRootPath);
        emit stateChanged();
    }
}

void ExplorerPane::populateExplorerGallery(const QString &folderPath)
{
    if (!m_galleryList)
    {
        return;
    }

    m_galleryList->clear();

    SequenceAwareIconProvider iconProvider;
    QDir dir(folderPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo &info : entries)
    {
        const bool isSequence = info.isDir() && isImageSequencePath(info.absoluteFilePath());
        QIcon itemIcon = iconProvider.icon(info);
        if (isSequence)
        {
            const QPixmap preview = previewPixmapForFile(info.absoluteFilePath());
            if (!preview.isNull())
            {
                itemIcon = QIcon(preview);
            }
        }
        auto *item = new QListWidgetItem(itemIcon, info.fileName(), m_galleryList);
        item->setData(Qt::UserRole, info.absoluteFilePath());
        item->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        if (isSequence)
        {
            item->setText(QStringLiteral("%1 [Sequence]").arg(imageSequenceDisplayLabel(info.absoluteFilePath())));
        }
        else if (info.isDir())
        {
            item->setText(QStringLiteral("[%1]").arg(info.fileName()));
        }
    }

    if (m_galleryTitleLabel)
    {
        m_galleryTitleLabel->setText(QDir::toNativeSeparators(folderPath));
    }
}

void ExplorerPane::setExplorerGalleryPath(const QString &folderPath, bool emitSignals)
{
    if (!m_explorerStack || !m_galleryList)
    {
        return;
    }

    if (folderPath.isEmpty() || !QFileInfo(folderPath).isDir())
    {
        m_galleryFolderPath.clear();
        m_explorerStack->setCurrentIndex(0);
        hideExplorerHoverPreview();

        if (emitSignals)
        {
            emit galleryPathChanged(QString());
            emit stateChanged();
        }
        return;
    }

    m_galleryFolderPath = QDir(folderPath).absolutePath();
    populateExplorerGallery(m_galleryFolderPath);
    m_explorerStack->setCurrentIndex(1);

    if (emitSignals)
    {
        emit galleryPathChanged(m_galleryFolderPath);
        emit stateChanged();
    }
}

void ExplorerPane::chooseExplorerRoot()
{
    const QString startPath = m_currentRootPath.isEmpty() ? QDir::currentPath() : m_currentRootPath;
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Media Folder"),
        startPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!selected.isEmpty())
    {
        setExplorerRootPath(selected, true);
    }
}

QPixmap ExplorerPane::previewPixmapForFile(const QString &filePath) const
{
    const QFileInfo info(filePath);
    const bool isSequence = isImageSequencePath(filePath);
    if (!info.exists() || (!info.isFile() && !isSequence))
    {
        return {};
    }

    const QString cacheKey = info.absoluteFilePath() + QLatin1Char('|') +
                             QString::number(info.lastModified().toMSecsSinceEpoch());
    if (m_previewPixmapCache.contains(cacheKey))
    {
        return m_previewPixmapCache.value(cacheKey);
    }

    if (isSequence)
    {
        const QPixmap pixmap = sequenceFolderIcon(QSize(280, 180)).pixmap(QSize(280, 180));
        if (!pixmap.isNull())
        {
            m_previewPixmapCache.insert(cacheKey, pixmap);
        }
        return pixmap;
    }

    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("png") ||
        suffix == QStringLiteral("jpg") ||
        suffix == QStringLiteral("jpeg") ||
        suffix == QStringLiteral("webp"))
    {
        QImage image(filePath);
        const QPixmap pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image);
        if (!pixmap.isNull())
        {
            m_previewPixmapCache.insert(cacheKey, pixmap);
        }
        return pixmap;
    }

    const MediaProbeResult probe = probeMediaFile(filePath);
    if (probe.mediaType == ClipMediaType::Image)
    {
        const QString imagePath = filePath;
        if (imagePath.isEmpty())
        {
            return {};
        }
        QImage image(imagePath);
        const QPixmap pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image);
        if (!pixmap.isNull())
        {
            m_previewPixmapCache.insert(cacheKey, pixmap);
        }
        return pixmap;
    }

    if (probe.mediaType != ClipMediaType::Video)
    {
        return {};
    }

    AVFormatContext *formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, QFile::encodeName(filePath).constData(), nullptr, nullptr) < 0)
    {
        return {};
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0)
    {
        avformat_close_input(&formatCtx);
        return {};
    }

    int videoStreamIndex = -1;
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i)
    {
        if (formatCtx->streams[i] &&
            formatCtx->streams[i]->codecpar &&
            formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (videoStreamIndex < 0)
    {
        avformat_close_input(&formatCtx);
        return {};
    }

    AVStream *stream = formatCtx->streams[videoStreamIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder)
    {
        avformat_close_input(&formatCtx);
        return {};
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx)
    {
        avformat_close_input(&formatCtx);
        return {};
    }

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return {};
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgbaFrame = av_frame_alloc();
    QPixmap pixmap;

    if (!packet || !frame || !rgbaFrame)
    {
        av_frame_free(&rgbaFrame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return {};
    }

    SwsContext *sws = nullptr;

    auto decodeFrame = [&](AVFrame *decodedFrame)
    {
        if (decodedFrame->width <= 0 || decodedFrame->height <= 0)
        {
            return;
        }

        sws = sws_getCachedContext(
            sws,
            decodedFrame->width,
            decodedFrame->height,
            static_cast<AVPixelFormat>(decodedFrame->format),
            decodedFrame->width,
            decodedFrame->height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

        if (!sws)
        {
            return;
        }

        if (rgbaFrame->width != decodedFrame->width || rgbaFrame->height != decodedFrame->height ||
            rgbaFrame->format != AV_PIX_FMT_RGBA)
        {
            av_frame_unref(rgbaFrame);
            rgbaFrame->format = AV_PIX_FMT_RGBA;
            rgbaFrame->width = decodedFrame->width;
            rgbaFrame->height = decodedFrame->height;
            if (av_frame_get_buffer(rgbaFrame, 32) < 0)
            {
                av_frame_unref(rgbaFrame);
                return;
            }
        }

        if (av_frame_make_writable(rgbaFrame) < 0)
        {
            return;
        }

        sws_scale(
            sws,
            decodedFrame->data,
            decodedFrame->linesize,
            0,
            decodedFrame->height,
            rgbaFrame->data,
            rgbaFrame->linesize);

        QImage image(rgbaFrame->data[0],
                     decodedFrame->width,
                     decodedFrame->height,
                     rgbaFrame->linesize[0],
                     QImage::Format_RGBA8888);
        pixmap = QPixmap::fromImage(image.copy());
    };

    while (av_read_frame(formatCtx, packet) >= 0 && pixmap.isNull())
    {
        if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) >= 0)
        {
            while (avcodec_receive_frame(codecCtx, frame) >= 0)
            {
                decodeFrame(frame);
                av_frame_unref(frame);
                if (!pixmap.isNull())
                {
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    if (pixmap.isNull())
    {
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0 && pixmap.isNull())
        {
            decodeFrame(frame);
            av_frame_unref(frame);
        }
    }

    sws_freeContext(sws);
    av_frame_free(&rgbaFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);

    if (!pixmap.isNull())
    {
        m_previewPixmapCache.insert(cacheKey, pixmap);
    }

    return pixmap;
}

void ExplorerPane::hideExplorerHoverPreview()
{
    if (m_explorerHoverPreview)
    {
        m_explorerHoverPreview->hide();
    }
}

void ExplorerPane::showExplorerHoverPreview(const QString &filePath)
{
    const QPixmap source = previewPixmapForFile(filePath);
    if (source.isNull())
    {
        hideExplorerHoverPreview();
        return;
    }

    if (!m_explorerHoverPreview)
    {
        m_explorerHoverPreview = new QLabel(nullptr, Qt::ToolTip);
        m_explorerHoverPreview->setObjectName(QStringLiteral("explorerHoverPreview"));
        m_explorerHoverPreview->setAlignment(Qt::AlignCenter);
        m_explorerHoverPreview->setStyleSheet(
            QStringLiteral(
                "QLabel#explorerHoverPreview { "
                "background: #05080c; color: #edf2f7; border: 1px solid #24303c; "
                "border-radius: 10px; padding: 8px; }"));
    }

    QSize targetSize(280, 180);
    if (m_preview)
    {
        const QSize previewSize = m_preview->size() - QSize(32, 32);
        if (previewSize.width() > 0 && previewSize.height() > 0)
        {
            targetSize = previewSize;
        }
    }

    const QPixmap scaled = source.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_explorerHoverPreview->setPixmap(scaled);
    m_explorerHoverPreview->resize(scaled.size() + QSize(16, 16));

    QPoint previewAnchor(80, 80);
    if (m_preview)
    {
        const QRect previewRect = m_preview->rect();
        previewAnchor = m_preview->mapToGlobal(
            QPoint(qMax(24, (previewRect.width() - m_explorerHoverPreview->width()) / 2),
                   qMax(24, previewRect.height() / 8)));
    }

    m_explorerHoverPreview->move(previewAnchor);
    m_explorerHoverPreview->show();
    m_explorerHoverPreview->raise();
}
