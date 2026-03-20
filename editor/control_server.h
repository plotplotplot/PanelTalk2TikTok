#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

class QWidget;
class QTcpServer;
class QTcpSocket;
class QThread;

class ControlServer final : public QObject {
    Q_OBJECT
public:
    explicit ControlServer(
        QWidget* window,
        std::function<QJsonObject()> fastSnapshotCallback = {},
        std::function<QJsonObject()> profilingCallback = {},
        QObject* parent = nullptr);
    ~ControlServer() override;

    bool start(quint16 port);

private slots:
    void onNewConnection();

private:
    struct ParsedRequest;

    QWidget* m_window = nullptr;
    std::function<QJsonObject()> m_fastSnapshotCallback;
    std::function<QJsonObject()> m_profilingCallback;
    std::unique_ptr<QThread> m_thread;
    QObject* m_worker = nullptr;
};
