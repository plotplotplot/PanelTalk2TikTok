#include "control_server.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHash>
#include <QJsonArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QAction>
#include <QSemaphore>
#include <QSlider>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <memory>
#include <optional>

namespace {

constexpr int kUiInvokeTimeoutMs = 250;
constexpr qint64 kUiHeartbeatStaleMs = 1000;

QString reasonPhrase(int statusCode) {
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 400: return QStringLiteral("Bad Request");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 408: return QStringLiteral("Request Timeout");
    case 500: return QStringLiteral("Internal Server Error");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QStringLiteral("Status");
    }
}

QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject parseJsonObject(const QByteArray& body, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("request body must be a JSON object");
        }
        return {};
    }
    return document.object();
}

template <typename Result, typename Fn>
bool invokeOnUiThread(QWidget* window, int timeoutMs, Result* out, Fn&& fn) {
    if (!window || !out) {
        return false;
    }

    struct SharedState {
        QSemaphore semaphore;
        Result result{};
        bool invoked = false;
    };

    auto state = std::make_shared<SharedState>();
    const bool scheduled = QMetaObject::invokeMethod(
        window,
        [state, fn = std::forward<Fn>(fn)]() mutable {
            state->result = fn();
            state->invoked = true;
            state->semaphore.release();
        },
        Qt::QueuedConnection);

    if (!scheduled) {
        return false;
    }
    if (!state->semaphore.tryAcquire(1, timeoutMs)) {
        return false;
    }

    *out = state->result;
    return state->invoked;
}

QJsonObject widgetSnapshot(QWidget* widget) {
    QJsonObject object{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()},
        {QStringLiteral("x"), widget->x()},
        {QStringLiteral("y"), widget->y()},
        {QStringLiteral("width"), widget->width()},
        {QStringLiteral("height"), widget->height()}
    };

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        object[QStringLiteral("text")] = button->text();
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("checked")] = button->isChecked();
    } else if (auto* slider = qobject_cast<QSlider*>(widget)) {
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("value")] = slider->value();
        object[QStringLiteral("minimum")] = slider->minimum();
        object[QStringLiteral("maximum")] = slider->maximum();
    } else {
        object[QStringLiteral("clickable")] = false;
    }

    QJsonArray children;
    const auto childWidgets = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : childWidgets) {
        children.append(widgetSnapshot(child));
    }
    object[QStringLiteral("children")] = children;
    return object;
}

QWidget* findWidgetByObjectName(QWidget* root, const QString& objectName) {
    if (!root || objectName.isEmpty()) {
        return nullptr;
    }
    if (root->objectName() == objectName) {
        return root;
    }
    const auto matches = root->findChildren<QWidget*>(objectName, Qt::FindChildrenRecursively);
    return matches.isEmpty() ? nullptr : matches.constFirst();
}

Qt::MouseButton parseMouseButton(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("right")) {
        return Qt::RightButton;
    }
    if (normalized == QStringLiteral("middle")) {
        return Qt::MiddleButton;
    }
    return Qt::LeftButton;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos, Qt::MouseButton button) {
    if (!window) {
        return false;
    }

    QWidget* target = window->childAt(pos);
    if (!target) {
        target = window;
    }

    const QPoint localPos = target->mapFrom(window, pos);
    const QPoint globalPos = target->mapToGlobal(localPos);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        localPos,
        globalPos,
        button,
        button,
        Qt::NoModifier);
    QMouseEvent releaseEvent(
        QEvent::MouseButtonRelease,
        localPos,
        globalPos,
        button,
        Qt::NoButton,
        Qt::NoModifier);

    const bool pressOk = QApplication::sendEvent(target, &pressEvent);
    const bool releaseOk = QApplication::sendEvent(target, &releaseEvent);
    bool contextOk = true;
    if (button == Qt::RightButton) {
        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, localPos, globalPos);
        contextOk = QApplication::sendEvent(target, &contextEvent);
    }
    return pressOk && releaseOk && contextOk;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos) {
    return sendSyntheticClick(window, pos, Qt::LeftButton);
}

QJsonObject menuSnapshot(QMenu* menu) {
    QJsonArray actions;
    if (menu) {
        const auto menuActions = menu->actions();
        for (QAction* action : menuActions) {
            if (!action || action->isSeparator()) {
                continue;
            }
            actions.append(QJsonObject{
                {QStringLiteral("text"), action->text()},
                {QStringLiteral("enabled"), action->isEnabled()}
            });
        }
    }
    return QJsonObject{
        {QStringLiteral("ok"), menu != nullptr},
        {QStringLiteral("visible"), menu && menu->isVisible()},
        {QStringLiteral("actions"), actions}
    };
}

QMenu* activePopupMenu() {
    QWidget* widget = QApplication::activePopupWidget();
    return qobject_cast<QMenu*>(widget);
}

struct Request {
    QString method;
    QString path;
    QUrl url;
    QByteArray body;
};

class ControlServerWorker final : public QObject {
    Q_OBJECT
public:
    ControlServerWorker(QWidget* window,
                        std::function<QJsonObject()> fastSnapshotCallback,
                        std::function<QJsonObject()> profilingCallback)
        : m_window(window)
        , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
        , m_profilingCallback(std::move(profilingCallback)) {}

    ~ControlServerWorker() override = default;

    bool startListening(quint16 port) {
        m_server = std::make_unique<QTcpServer>();
        connect(m_server.get(), &QTcpServer::newConnection, this, &ControlServerWorker::onNewConnection);
        if (!m_server->listen(QHostAddress::LocalHost, port)) {
            return false;
        }
        m_listenPort = m_server->serverPort();
        fprintf(stderr, "ControlServer listening on http://127.0.0.1: %u\n", static_cast<unsigned>(m_listenPort));
        fflush(stderr);
        return true;
    }

    void stopListening() {
        for (QTcpSocket* socket : m_buffers.keys()) {
            socket->disconnectFromHost();
            socket->deleteLater();
        }
        m_buffers.clear();
        if (m_server) {
            m_server->close();
        }
    }

private slots:
    void onNewConnection() {
        if (!m_server) {
            return;
        }
        while (QTcpSocket* socket = m_server->nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                m_buffers.remove(socket);
                socket->deleteLater();
            });
        }
    }

private:
    void onReadyRead(QTcpSocket* socket) {
        m_buffers[socket].append(socket->readAll());
        std::optional<Request> request = tryParseRequest(m_buffers[socket]);
        if (!request.has_value()) {
            return;
        }
        handleRequest(socket, *request);
        m_buffers.remove(socket);
    }

    std::optional<Request> tryParseRequest(const QByteArray& data) const {
        const int headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return std::nullopt;
        }

        const QList<QByteArray> lines = data.left(headerEnd).split('\n');
        if (lines.isEmpty()) {
            return std::nullopt;
        }

        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 2) {
            return std::nullopt;
        }

        int contentLength = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QByteArray key = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (key == "content-length") {
                contentLength = value.toInt();
            }
        }

        const int totalSize = headerEnd + 4 + contentLength;
        if (data.size() < totalSize) {
            return std::nullopt;
        }

        Request request;
        request.method = QString::fromLatin1(requestLine.at(0));
        request.path = QString::fromLatin1(requestLine.at(1));
        request.url = QUrl(QStringLiteral("http://127.0.0.1") + request.path);
        request.body = data.mid(headerEnd + 4, contentLength);
        return request;
    }

    QJsonObject fastSnapshot() const {
        return m_fastSnapshotCallback ? m_fastSnapshotCallback() : QJsonObject{};
    }

    bool uiThreadResponsive() const {
        const QJsonObject snapshot = fastSnapshot();
        return snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <= kUiHeartbeatStaleMs;
    }

    void writeResponse(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType) {
        const QByteArray header =
            "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reasonPhrase(statusCode).toUtf8() + "\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            "Connection: close\r\n\r\n";
        socket->write(header);
        socket->write(body);
        socket->disconnectFromHost();
    }

    void writeJson(QTcpSocket* socket, int statusCode, const QJsonObject& object) {
        writeResponse(socket, statusCode, jsonBytes(object), "application/json");
    }

    void writeError(QTcpSocket* socket, int statusCode, const QString& error) {
        writeJson(socket, statusCode, QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        });
    }

    void handleRequest(QTcpSocket* socket, const Request& request) {
        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/health")) {
            QJsonObject snapshot = fastSnapshot();
            snapshot[QStringLiteral("ok")] = true;
            snapshot[QStringLiteral("port")] = static_cast<qint64>(m_listenPort);
            snapshot[QStringLiteral("pid")] = snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()));
            writeJson(socket, 200, snapshot);
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/playhead")) {
            const QJsonObject snapshot = fastSnapshot();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("pid"), snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()))},
                {QStringLiteral("current_frame"), snapshot.value(QStringLiteral("current_frame")).toInteger()},
                {QStringLiteral("playback_active"), snapshot.value(QStringLiteral("playback_active")).toBool()},
                {QStringLiteral("main_thread_heartbeat_age_ms"), snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1)}
            });
            return;
        }

        if (!uiThreadResponsive()) {
            writeError(socket, 503, QStringLiteral("ui thread is unresponsive"));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/profile")) {
            QJsonObject profile;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &profile, [this]() {
                    return m_profilingCallback ? m_profilingCallback() : QJsonObject{};
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for ui-thread profile"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("profile"), profile}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/ui")) {
            QJsonObject tree;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &tree, [this]() {
                    return widgetSnapshot(m_window);
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for ui hierarchy"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("ui"), tree},
                {QStringLiteral("window"), tree}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/screenshot")) {
            QByteArray pngBytes;
            if (!invokeOnUiThread(m_window, 500, &pngBytes, [this]() {
                    QByteArray bytes;
                    QBuffer buffer(&bytes);
                    buffer.open(QIODevice::WriteOnly);
                    m_window->grab().save(&buffer, "PNG");
                    return bytes;
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for screenshot"));
                return;
            }
            writeResponse(socket, 200, pngBytes, "image/png");
            return;
        }

        if ((request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")) &&
            request.url.path() == QStringLiteral("/click")) {
            const QUrlQuery query(request.url);
            int x = query.queryItemValue(QStringLiteral("x")).toInt();
            int y = query.queryItemValue(QStringLiteral("y")).toInt();
            QString buttonName = query.queryItemValue(QStringLiteral("button"));
            if (request.method == QStringLiteral("POST")) {
                QString error;
                const QJsonObject body = parseJsonObject(request.body, &error);
                if (!error.isEmpty()) {
                    writeError(socket, 400, error);
                    return;
                }
                x = body.value(QStringLiteral("x")).toInt(x);
                y = body.value(QStringLiteral("y")).toInt(y);
                buttonName = body.value(QStringLiteral("button")).toString(buttonName);
            }
            const Qt::MouseButton button = parseMouseButton(buttonName);

            QJsonObject result;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &result, [this, x, y, button, buttonName]() {
                    const bool clicked = sendSyntheticClick(m_window, QPoint(x, y), button);
                    return QJsonObject{
                        {QStringLiteral("ok"), clicked},
                        {QStringLiteral("x"), x},
                        {QStringLiteral("y"), y},
                        {QStringLiteral("button"), buttonName.isEmpty() ? QStringLiteral("left") : buttonName}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for click"));
                return;
            }
            writeJson(socket, result.value(QStringLiteral("ok")).toBool() ? 200 : 500, result);
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/menu")) {
            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, []() {
                    return menuSnapshot(activePopupMenu());
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for menu"));
                return;
            }
            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/menu")) {
            QString error;
            const QJsonObject body = parseJsonObject(request.body, &error);
            if (!error.isEmpty()) {
                writeError(socket, 400, error);
                return;
            }
            const QString text = body.value(QStringLiteral("text")).toString();
            if (text.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing text"));
                return;
            }

            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, [text]() {
                    QMenu* menu = activePopupMenu();
                    if (!menu) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("no active popup menu")}
                        };
                    }

                    for (QAction* action : menu->actions()) {
                        if (!action || action->isSeparator()) {
                            continue;
                        }
                        if (action->text() == text) {
                            const bool enabled = action->isEnabled();
                            if (enabled) {
                                action->trigger();
                            }
                            return QJsonObject{
                                {QStringLiteral("ok"), enabled},
                                {QStringLiteral("text"), text},
                                {QStringLiteral("enabled"), enabled}
                            };
                        }
                    }

                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("menu action not found")},
                        {QStringLiteral("text"), text},
                        {QStringLiteral("menu"), menuSnapshot(menu)}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for menu action"));
                return;
            }

            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/click-item")) {
            QString error;
            const QJsonObject body = parseJsonObject(request.body, &error);
            if (!error.isEmpty()) {
                writeError(socket, 400, error);
                return;
            }
            const QString id = body.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing id"));
                return;
            }

            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, [this, id]() {
                    QWidget* widget = findWidgetByObjectName(m_window, id);
                    if (!widget) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("widget not found")},
                            {QStringLiteral("id"), id}
                        };
                    }

                    const QJsonObject before = widgetSnapshot(widget);
                    const QJsonObject profileBefore = m_profilingCallback ? m_profilingCallback() : QJsonObject{};

                    bool clicked = false;
                    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                        button->click();
                        clicked = true;
                    } else {
                        clicked = sendSyntheticClick(m_window, widget->mapTo(m_window, widget->rect().center()));
                    }

                    const QJsonObject after = widgetSnapshot(widget);
                    const QJsonObject profileAfter = m_profilingCallback ? m_profilingCallback() : QJsonObject{};
                    const bool confirmed = clicked && (before != after || profileBefore != profileAfter);

                    return QJsonObject{
                        {QStringLiteral("ok"), clicked},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("confirmed"), confirmed},
                        {QStringLiteral("before"), before},
                        {QStringLiteral("after"), after},
                        {QStringLiteral("profile_before"), profileBefore},
                        {QStringLiteral("profile_after"), profileAfter}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for click-item"));
                return;
            }

            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.url.path().isEmpty()) {
            writeError(socket, 400, QStringLiteral("invalid request"));
            return;
        }

        writeError(socket, request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST") ? 404 : 405,
                   request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")
                       ? QStringLiteral("not found")
                       : QStringLiteral("method not allowed"));
    }

    QPointer<QWidget> m_window;
    std::function<QJsonObject()> m_fastSnapshotCallback;
    std::function<QJsonObject()> m_profilingCallback;
    std::unique_ptr<QTcpServer> m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_listenPort = 0;
};

} // namespace

ControlServer::ControlServer(QWidget* window,
                             std::function<QJsonObject()> fastSnapshotCallback,
                             std::function<QJsonObject()> profilingCallback,
                             QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
    , m_profilingCallback(std::move(profilingCallback)) {}

ControlServer::~ControlServer() {
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [worker = m_worker]() {
            static_cast<ControlServerWorker*>(worker)->stopListening();
        }, Qt::BlockingQueuedConnection);
        m_worker->deleteLater();
        m_worker = nullptr;
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool ControlServer::start(quint16 port) {
    if (m_thread) {
        return false;
    }

    m_thread = std::make_unique<QThread>();
    auto* worker = new ControlServerWorker(m_window, m_fastSnapshotCallback, m_profilingCallback);
    m_worker = worker;
    worker->moveToThread(m_thread.get());
    connect(m_thread.get(), &QThread::finished, worker, &QObject::deleteLater);
    m_thread->start();

    bool started = false;
    if (!QMetaObject::invokeMethod(
            worker,
            [&started, worker, port]() {
                started = worker->startListening(port);
            },
            Qt::BlockingQueuedConnection)) {
        return false;
    }

    if (!started) {
        worker->deleteLater();
        m_worker = nullptr;
        m_thread->quit();
        m_thread->wait();
        m_thread.reset();
        return false;
    }

    return true;
}

void ControlServer::onNewConnection() {}

#include "control_server.moc"
