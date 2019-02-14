#include "dataserver.h"

#include "dataextension.h"
#include "extension_support_internal.h"
#include "widgetdefs.h"

#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

#include <unordered_set>

#define DS_SEND_WARN(c, m)   \
    QString e(m);            \
    sendErrorToClient(c, e); \
    qWarning() << e;

using namespace std::placeholders;

DataServer::DataServer(QObject* parent) :
    QObject(parent),
    m_pWebSocketServer(nullptr),
    m_Methods{{"subscribe", std::bind(&DataServer::handleMethodSubscribe, this, _1, _2)},
              {"query", std::bind(&DataServer::handleMethodQuery, this, _1, _2)},
              {"auth", std::bind(&DataServer::handleMethodAuth, this, _1, _2)},
              {"mutate", std::bind(&DataServer::handleMethodMutate, this, _1, _2)}},
    m_InternalTargets{{"settings", std::bind(&DataServer::handleTargetSettings, this, _1, _2, _3)},
                      {"launcher", std::bind(&DataServer::handleTargetLauncher, this, _1, _2, _3)}}
{
    qRegisterMetaType<AppLauncherData>("AppLauncherData");
    qRegisterMetaTypeStreamOperators<AppLauncherData>("AppLauncherData");

    m_pWebSocketServer = new QWebSocketServer(QStringLiteral("Data Server"), QWebSocketServer::SecureMode, this);

    QSslConfiguration sslConfiguration;
    QFile             certFile(QStringLiteral(":/Resources/localhost.crt"));
    QFile             keyFile(QStringLiteral(":/Resources/localhost.key"));
    certFile.open(QIODevice::ReadOnly);
    keyFile.open(QIODevice::ReadOnly);
    QSslCertificate certificate(&certFile, QSsl::Pem);
    QSslKey         sslKey(&keyFile, QSsl::Rsa, QSsl::Pem);
    certFile.close();
    keyFile.close();
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
    sslConfiguration.setLocalCertificate(certificate);
    sslConfiguration.setPrivateKey(sslKey);
    sslConfiguration.setProtocol(QSsl::TlsV1SslV3);
    m_pWebSocketServer->setSslConfiguration(sslConfiguration);

    QSettings settings;
    quint16   port = settings.value(QUASAR_CONFIG_PORT, QUASAR_DATA_SERVER_DEFAULT_PORT).toUInt();

    if (!m_pWebSocketServer->listen(QHostAddress::LocalHost, port))
    {
        qWarning() << "Data server failed to bind port" << port;
    }
    else
    {
        qInfo() << "Data server running locally on port" << port;
        connect(m_pWebSocketServer, &QWebSocketServer::newConnection, this, &DataServer::onNewConnection);

        loadExtensions();
    }

    m_LauncherMap = settings.value(QUASAR_CONFIG_LAUNCHERMAP).toMap();
}

DataServer::~DataServer()
{
    m_Methods.clear();

    m_Extensions.clear();

    m_AuthedClientsMap.clear();

    m_AuthCodeMap.clear();

    m_pWebSocketServer->close();
}

bool DataServer::addMethodHandler(QString type, MethodFuncType handler)
{
    if (m_Methods.count(type))
    {
        qWarning() << "Handler for request type " << type << " already exists";
        return false;
    }

    m_Methods[type] = handler;

    return true;
}

bool DataServer::findExtension(QString extcode)
{
    std::shared_lock<std::shared_mutex> lk(m_ExtensionsMtx);
    return (m_Extensions.count(extcode) > 0);
}

QString DataServer::generateAuthCode(QString ident, ClientAccessLevel lvl)
{
    quint64 v  = QRandomGenerator::global()->generate64();
    QString hv = QString::number(v, 16).toUpper();

    std::unique_lock<std::mutex> lk(m_AuthCodeMtx);
    m_AuthCodeMap.insert({hv, {ident, lvl, system_clock::now() + 10s}});

    return hv;
}

void DataServer::loadExtensions()
{
    QDir          dir("extensions/");
    QFileInfoList list = dir.entryInfoList(QStringList() << "*.dll"
                                                         << "*.so"
                                                         << "*.dylib",
                                           QDir::Files);

    if (list.count() == 0)
    {
        qInfo() << "No data extensions found";
        return;
    }

    // just lock the whole thing while initializing extensions at startup
    // to prevent out of order reads
    std::unique_lock<std::shared_mutex> lk(m_ExtensionsMtx);

    for (QFileInfo& file : list)
    {
        QString libpath = file.path() + "/" + file.fileName();

        qInfo() << "Loading data extension" << libpath;

        DataExtension* extn = DataExtension::load(libpath, this);

        if (!extn)
        {
            qWarning() << "Failed to load extension" << libpath;
        }
        else if (m_InternalTargets.count(extn->getCode()))
        {
            qWarning() << "The extension code" << extn->getCode() << " is reserved. Unloading " << libpath;
        }
        else if (m_Extensions.count(extn->getCode()))
        {
            qWarning() << "Extension with code " << extn->getCode() << " already loaded. Unloading" << libpath;
        }
        else
        {
            qInfo() << "Extension " << extn->getCode() << " loaded.";
            m_Extensions[extn->getCode()].reset(extn);
            extn = nullptr;
        }

        if (extn != nullptr)
        {
            delete extn;
        }
    }
}

void DataServer::handleRequest(const QJsonObject& req, QWebSocket* sender)
{
    if (req.isEmpty())
    {
        DS_SEND_WARN(sender, "Invalid JSON request");
        return;
    }

    QString mtd = req["method"].toString();

    if (!m_Methods.count(mtd))
    {
        DS_SEND_WARN(sender, "Unknown method type " + mtd);
        return;
    }

    m_Methods[mtd](req, sender);
}

void DataServer::handleMethodSubscribe(const QJsonObject& req, QWebSocket* sender)
{
    QString widgetName;

    {
        std::shared_lock<std::shared_mutex> lk(m_AuthedClientsMtx);

        auto it = m_AuthedClientsMap.find(sender);
        if (it == m_AuthedClientsMap.end())
        {
            DS_SEND_WARN(sender, "Unauthenticated client");
            return;
        }

        widgetName = it->second.ident;
    }

    auto parms = req["params"].toObject();

    if (parms.count() != 2)
    {
        DS_SEND_WARN(sender, "Invalid parameters for method 'subscribe'");
        return;
    }

    QString extcode = parms["target"].toString();
    QString extparm = parms["params"].toString();

    std::shared_lock<std::shared_mutex> lk(m_ExtensionsMtx);

    if (!m_Extensions.count(extcode))
    {
        DS_SEND_WARN(sender, "Unknown extension code " + extcode);
        return;
    }

    QStringList dlist = extparm.split(',', QString::SkipEmptyParts);

    for (QString& src : dlist)
    {
        if (m_Extensions[extcode]->addSubscriber(src, sender, widgetName))
        {
            qInfo() << "Widget " << widgetName << " subscribed to extension " << extcode << " data " << src;
        }
        else
        {
            DS_SEND_WARN(sender, "Failed to subscribed to extension " + extcode + " data " + src);
        }
    }
}

void DataServer::handleMethodQuery(const QJsonObject& req, QWebSocket* sender)
{
    client_data_t clidat;

    {
        std::shared_lock<std::shared_mutex> lk(m_AuthedClientsMtx);

        auto it = m_AuthedClientsMap.find(sender);
        if (it == m_AuthedClientsMap.end())
        {
            DS_SEND_WARN(sender, "Unauthenticated client");
            return;
        }

        clidat = it->second;
    }

    auto parms = req["params"].toObject();

    if (parms.count() != 2)
    {
        DS_SEND_WARN(sender, "Invalid parameters for method 'query'");
        return;
    }

    QString extcode = parms["target"].toString();
    QString extparm = parms["params"].toString();

    if (m_InternalTargets.count(extcode))
    {
        m_InternalTargets[extcode](extparm, clidat, sender);
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_ExtensionsMtx);

    if (!m_Extensions.count(extcode))
    {
        DS_SEND_WARN(sender, "Unknown extension code " + extcode);
        return;
    }

    // Add client to poll queue
    if (m_Extensions[extcode]->addSubscriber(extparm, sender, clidat.ident))
    {
        m_Extensions[extcode]->pollAndSendData(extparm, sender, clidat.ident);
    }
}

void DataServer::handleMethodAuth(const QJsonObject& req, QWebSocket* sender)
{
    {
        std::shared_lock<std::shared_mutex> lk(m_AuthedClientsMtx);
        if (m_AuthedClientsMap.count(sender))
        {
            DS_SEND_WARN(sender, "Client already authenticated.");
            return;
        }
    }

    auto parms = req["params"].toObject();

    if (parms.count() != 1)
    {
        DS_SEND_WARN(sender, "Invalid parameters for method 'auth'");
        return;
    }

    QString authcode = parms["code"].toString();

    std::unique_lock<std::mutex> lk(m_AuthCodeMtx);

    auto it = m_AuthCodeMap.find(authcode);
    if (it == m_AuthCodeMap.end())
    {
        DS_SEND_WARN(sender, "Invalid authentication code");
        return;
    }

    std::unique_lock<std::shared_mutex> lkm(m_AuthedClientsMtx);
    m_AuthedClientsMap.insert({sender, it->second});

    qInfo() << "Widget ident " << it->second.ident << " authenticated.";

    m_AuthCodeMap.erase(it);
}

void DataServer::handleMethodMutate(const QJsonObject& req, QWebSocket* sender)
{
    // TODO THIS SH IT
}

void DataServer::handleTargetSettings(QString params, client_data_t client, QWebSocket* sender)
{
    if (client.access < CAL_SETTINGS)
    {
        DS_SEND_WARN(sender, "Insufficient access for query target 'settings'");
        return;
    }

    // compile all settings into json
    QSettings settings;

    QJsonObject sjson;

    // general
    QJsonObject general;

    general["dataport"] = settings.value(QUASAR_CONFIG_PORT, QUASAR_DATA_SERVER_DEFAULT_PORT).toInt();
    general["loglevel"] = settings.value(QUASAR_CONFIG_LOGLEVEL, QUASAR_CONFIG_DEFAULT_LOGLEVEL).toInt();
    general["cookies"]  = settings.value(QUASAR_CONFIG_COOKIES).toString();
    general["savelog"]  = settings.value(QUASAR_CONFIG_LOGFILE, false).toBool();

#ifdef Q_OS_WIN
    // ------------------Startup launch
    QString   startupFolder = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/Startup";
    QFileInfo lnk(startupFolder + "/Quasar.lnk");

    general["startup"] = lnk.exists();
#else
    general["startup"] = false;
#endif

    sjson["general"] = general;

    // extensions
    QJsonArray extensions;

    {
        std::shared_lock<std::shared_mutex> lk(m_ExtensionsMtx);

        for (auto& iext : m_Extensions)
        {
            QJsonObject jext;

            auto& ext = iext.second;

            // ext info
            jext["name"]        = iext.first;
            jext["fullname"]    = ext->getName();
            jext["version"]     = ext->getVersion();
            jext["author"]      = ext->getAuthor();
            jext["description"] = ext->getDesc();
            jext["website"]; // TODO ext web

            // ext rates
            QJsonArray rates;

            for (auto& isrc : ext->getDataSources())
            {
                QJsonObject rate;

                auto& src       = isrc.second;
                rate["name"]    = src.name;
                rate["enabled"] = settings.value(ext->getSettingsKey(src.name + QUASAR_DP_ENABLED), true).toBool();
                rate["rate"]    = src.refreshmsec;

                rates.append(rate);
            }

            jext["rates"] = rates;

            // ext settings
            if (auto eset = ext->getSettings())
            {
                static const QString entryTypeStrArr[] = {"int", "double", "bool"};

                QJsonArray extsettings;

                for (auto& it : eset->map)
                {
                    QJsonObject s;

                    auto& entry = it.second;
                    s["name"]   = it.first;
                    s["desc"]   = entry.description;
                    s["type"]   = entryTypeStrArr[entry.type];

                    switch (entry.type)
                    {
                        case QUASAR_SETTING_ENTRY_INT:
                        {
                            s["min"]  = entry.inttype.min;
                            s["max"]  = entry.inttype.max;
                            s["step"] = entry.inttype.step;
                            s["def"]  = entry.inttype.def;
                            s["val"]  = entry.inttype.val;
                            break;
                        }

                        case QUASAR_SETTING_ENTRY_DOUBLE:
                        {
                            s["min"]  = entry.doubletype.min;
                            s["max"]  = entry.doubletype.max;
                            s["step"] = entry.doubletype.step;
                            s["def"]  = entry.doubletype.def;
                            s["val"]  = entry.doubletype.val;
                            break;
                        }

                        case QUASAR_SETTING_ENTRY_BOOL:
                        {
                            s["def"] = entry.booltype.def;
                            s["val"] = entry.booltype.val;
                            break;
                        }
                    }

                    extsettings.append(s);
                }

                jext["settings"] = extsettings;
            }
            else
            {
                jext["settings"] = QJsonValue(QJsonValue::Null);
            }
        }
    }

    sjson["extensions"] = extensions;

    // launcher
    QJsonObject launcher;

    qDebug() << "TODO implement launcher";

    sjson["launcher"] = launcher;

    auto sdata = QJsonObject{{"data", QJsonObject{{"settings", sjson}}}};

    // send
    QJsonDocument doc(sdata);
    sender->sendTextMessage(QString::fromUtf8(doc.toJson()));
}

void DataServer::handleTargetLauncher(QString params, client_data_t client, QWebSocket* sender)
{
    // IIf get, send data
    if (params == "get")
    {
        // TODO THIS STUFF
        return;
    }

    // Otherwise launch code
    std::unique_lock<std::shared_mutex> lock(m_LauncherMtx);

    if (!m_LauncherMap.contains(params))
    {
        qWarning() << "Launcher command " << params << " not defined";
        return;
    }

    AppLauncherData d;

    QVariant& v = m_LauncherMap[params];

    if (v.canConvert<AppLauncherData>())
    {
        d = v.value<AppLauncherData>();

        QString cmd = d.file;

        if (cmd.contains("://"))
        {
            // treat as url
            qInfo() << "Launching URL " << cmd;
            QDesktopServices::openUrl(QUrl(cmd));
        }
        else
        {
            qInfo() << "Launching " << cmd << d.arguments;

            // treat as file/command
            QFileInfo info(cmd);

            if (info.exists())
            {
                cmd = info.canonicalFilePath();
            }

            bool result = QProcess::startDetached(cmd, QStringList() << d.arguments, d.startpath);

            if (!result)
            {
                qWarning() << "Failed to launch " << cmd;
            }
        }
    }
}

void DataServer::checkAuth(QWebSocket* client)
{
    {
        // Check auth
        std::shared_lock<std::shared_mutex> lk(m_AuthedClientsMtx);
        if (!m_AuthedClientsMap.count(client))
        {
            // unauthenticated client, cut the connection
            DS_SEND_WARN(client, "Unauthenticated client, disconnecting");
            client->close(QWebSocketProtocol::CloseCodeNormal, "Unauthenticated client");
        }
    }

    {
        // Clean expired auth codes
        std::unique_lock<std::mutex> lk(m_AuthCodeMtx);
        for (auto it = m_AuthCodeMap.begin(); it != m_AuthCodeMap.end();)
        {
            if (it->second.expiry > system_clock::now())
            {
                it = m_AuthCodeMap.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void DataServer::sendErrorToClient(QWebSocket* client, QString err)
{
    // Craft error json msg
    QJsonObject msg;
    msg["error"] = err;

    QJsonDocument doc(msg);

    client->sendTextMessage(QString::fromUtf8(doc.toJson()));
}

void DataServer::onNewConnection()
{
    QWebSocket* pSocket = m_pWebSocketServer->nextPendingConnection();

    pSocket->setParent(this);
    connect(pSocket, &QWebSocket::textMessageReceived, this, &DataServer::processMessage);
    connect(pSocket, &QWebSocket::disconnected, this, &DataServer::socketDisconnected);

    QTimer* authtimer = new QTimer(pSocket);
    authtimer->setSingleShot(true);

    connect(authtimer, &QTimer::timeout, this, [this, pSocket, authtimer] {
        checkAuth(pSocket);
        authtimer->deleteLater();
    });

    authtimer->start(10000);
}

void DataServer::processMessage(QString message)
{
    QWebSocket* pSender = qobject_cast<QWebSocket*>(sender());

    if (pSender)
    {
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());

        if (doc.isNull())
        {
            qWarning() << "Error parsing WebSocket message";
            qWarning() << message;
            return;
        }

        handleRequest(doc.object(), pSender);
    }
}

void DataServer::socketDisconnected()
{
    QWebSocket* pClient = qobject_cast<QWebSocket*>(sender());
    if (pClient)
    {
        {
            std::shared_lock<std::shared_mutex> lk(m_ExtensionsMtx);
            for (auto& p : m_Extensions)
            {
                p.second->removeSubscriber(pClient);
            }
        }

        {
            std::unique_lock<std::shared_mutex> lkm(m_AuthedClientsMtx);
            m_AuthedClientsMap.erase(pClient);
        }

        pClient->deleteLater();
    }
}
