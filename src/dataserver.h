#pragma once

#include <qstring_hash_impl.h>

#include <QObject>
#include <functional>
#include <memory>
#include <unordered_map>

QT_FORWARD_DECLARE_CLASS(QWebSocketServer)
QT_FORWARD_DECLARE_CLASS(QWebSocket)
QT_FORWARD_DECLARE_CLASS(Quasar)

class DataPlugin;

using DataPluginMapType = std::unordered_map<QString, std::unique_ptr<DataPlugin>>;
using HandlerFuncType   = std::function<void(const QJsonObject&, QWebSocket*)>;

class DataServer : public QObject
{
    Q_OBJECT;

    using HandleReqCallMapType = std::unordered_map<QString, HandlerFuncType>;

public:
    explicit DataServer(QObject* parent);
    ~DataServer();

    DataPluginMapType& getPlugins() { return m_plugins; };

    bool addHandler(QString type, HandlerFuncType handler);

private:
    void loadDataPlugins();
    void handleRequest(const QJsonObject& req, QWebSocket* sender);

    void handleSubscribeReq(const QJsonObject& req, QWebSocket* sender);
    void handlePollReq(const QJsonObject& req, QWebSocket* sender);

private slots:
    void onNewConnection();
    void processMessage(QString message);
    void socketDisconnected();

private:
    HandleReqCallMapType m_reqcallmap;
    Quasar*              m_parent;
    QWebSocketServer*    m_pWebSocketServer;
    DataPluginMapType    m_plugins;
};
