#include "startstream.h"
#include "backend/computermanager.h"
#include "backend/computerseeker.h"
#include "streaming/session.h"

#include <QCoreApplication>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>

#define COMPUTER_SEEK_TIMEOUT 30000
#define APP_SEEK_TIMEOUT 10000

namespace CliStartStream
{

enum State {
    StateInit,
    StateSeekComputer,
    StateSeekApp,
    StateStartSession,
    StateFailure,
};

class Event
{
public:
    enum Type {
        AppQuitCompleted,
        AppQuitRequested,
        ComputerFound,
        ComputerUpdated,
        Executed,
        Timedout,
    };

    Event(Type type)
        : type(type), computerManager(nullptr), computer(nullptr) {}

    Type type;
    ComputerManager *computerManager;
    NvComputer *computer;
    QString errorMessage;
};

class LauncherPrivate
{
    Q_DECLARE_PUBLIC(Launcher)

public:
    LauncherPrivate(Launcher *q) : q_ptr(q) {}

    void handleEvent(Event event)
    {
        Q_Q(Launcher);
        Session* session;
        NvApp app;

        switch (event.type) {
        // Occurs when CliStartStreamSegue becomes visible and the UI calls launcher's execute()
        case Event::Executed:
            if (m_State == StateInit) {
                m_State = StateSeekComputer;
                m_ComputerManager = event.computerManager;

                m_ComputerSeeker = new ComputerSeeker(m_ComputerManager, m_ComputerName, q);
                q->connect(m_ComputerSeeker, &ComputerSeeker::computerFound,
                           q, &Launcher::onComputerFound);
                // If we weren't provided a predefined PIN, generate one now
                if (m_PredefinedPin.isEmpty()) {
                    m_PredefinedPin = m_ComputerManager->generatePinString();
                }
                q->connect(m_ComputerSeeker, &ComputerSeeker::errorTimeout,
                           q, &Launcher::onTimeout);
                m_ComputerSeeker->start(COMPUTER_SEEK_TIMEOUT);

                q->connect(m_ComputerManager, &ComputerManager::computerStateChanged,
                           q, &Launcher::onComputerUpdated);
                q->connect(m_ComputerManager, &ComputerManager::quitAppCompleted,
                           q, &Launcher::onQuitAppCompleted);

                emit q->searchingComputer();
            }
            break;
        // Occurs when searched computer is found
        case Event::ComputerFound:
            if (m_State == StateSeekComputer) {
                if (event.computer->pairState == NvComputer::PS_PAIRED) {
                    m_State = StateSeekApp;
                    m_Computer = event.computer;
                    m_TimeoutTimer->start(APP_SEEK_TIMEOUT);
                    emit q->searchingApp();
                } else {
                    Q_ASSERT(!m_PredefinedPin.isEmpty());

                    // Create the network manager and request
                    QNetworkAccessManager* networkManager = new QNetworkAccessManager(m_ComputerManager);
                    QUrl url(QString("https://%1:47990/api/pin").arg(event.computer->activeAddress.address()));
                    QNetworkRequest request(url);
                    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
                    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
                    request.setSslConfiguration(sslConfig);

                    // Configure the request headers
                    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

                    // Setup basic authentication
                    QString credentials = QString("%1:%2").arg("utkarshdalal").arg("MoBiLe123");
                    QByteArray data = credentials.toLocal8Bit().toBase64();
                    QString headerData = "Basic " + data;
                    request.setRawHeader("Authorization", headerData.toLocal8Bit());

                    // Create JSON object to send
                    QJsonObject json;
                    json["pin"] = m_PredefinedPin;

                    // Convert JSON object to byte array
                    QJsonDocument doc(json);
                    QByteArray postData = doc.toJson();

                    // Send the request
                    QNetworkReply* reply = networkManager->post(request, postData);
                    // Handle the reply asynchronously
                    QObject::connect(reply, &QNetworkReply::finished, [this, reply, event, q, app, session]() {
                        if (reply->error() == QNetworkReply::NoError) {
                            QByteArray response = reply->readAll();
                            // Handle response here
                            qDebug() << "Success: " << response;
                            event.computer->pairState = NvComputer::PS_PAIRED;
                            if (m_ComputerManager->forceUpdateAppList(event.computer)) {
                                qDebug() << "Forced appList update succeeded.";
                            } else {
                                qDebug() << "Forced appList update failed.";
                            }
                            m_State = StateSeekApp;
                            m_Computer = event.computer;
                            m_TimeoutTimer->start(APP_SEEK_TIMEOUT);
                            emit q->searchingApp();
                        } else {
                            QString msg = QObject::tr("Failed to pair with %1").arg(event.computer->name);
                            emit q->failed(msg);
                            qDebug() << "Error:" << reply->errorString();
                        }
                    });
                    m_ComputerManager->pairHost(event.computer, m_PredefinedPin);
                }
            }
            break;
        // Occurs when a computer is updated
        case Event::ComputerUpdated:
            if (m_State == StateSeekApp) {
                int index = getAppIndex();
                if (-1 != index) {
                    app = m_Computer->appList[index];
                    m_TimeoutTimer->stop();
                    if (isNotStreaming() || isStreamingApp(app)) {
                        m_State = StateStartSession;
                        session = new Session(m_Computer, app, m_Preferences);
                        emit q->sessionCreated(app.name, session);
                    } else {
                        emit q->appQuitRequired(getCurrentAppName());
                    }
                }
            }
            break;
        // Occurs when there was another app running on computer and user accepted quit
        // confirmation dialog
        case Event::AppQuitRequested:
            if (m_State == StateSeekApp) {
                m_ComputerManager->quitRunningApp(m_Computer);
            }
            break;
        // Occurs when the previous app quit has been completed, handles quitting errors if any
        // happened. ComputerUpdated event's handler handles session start when previous app has
        // quit.
        case Event::AppQuitCompleted:
            if (m_State == StateSeekApp && !event.errorMessage.isEmpty()) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Quitting app failed, reason: %1").arg(event.errorMessage));
            }
            break;
        // Occurs when computer or app search timed out
        case Event::Timedout:
            if (m_State == StateSeekComputer) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to connect to %1").arg(m_ComputerName));
            }
            if (m_State == StateSeekApp) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to find application %1").arg(m_AppName));
            }
            break;
        }
    }

    int getAppIndex() const
    {
        for (int i = 0; i < m_Computer->appList.length(); i++) {
            if (m_Computer->appList[i].name.toLower() == m_AppName.toLower()) {
                return i;
            }
        }
        return -1;
    }

    bool isNotStreaming() const
    {
        return m_Computer->currentGameId == 0;
    }

    bool isStreamingApp(NvApp app) const
    {
        return m_Computer->currentGameId == app.id;
    }

    QString getCurrentAppName() const
    {
        for (const NvApp& app : m_Computer->appList) {
            if (m_Computer->currentGameId == app.id) {
                return app.name;
            }
        }
        return "<UNKNOWN>";
    }

    Launcher *q_ptr;
    QString m_ComputerName;
    QString m_AppName;
    QString m_PredefinedPin;
    StreamingPreferences *m_Preferences;
    ComputerManager *m_ComputerManager;
    ComputerSeeker *m_ComputerSeeker;
    NvComputer *m_Computer;
    State m_State;
    QTimer *m_TimeoutTimer;
};

Launcher::Launcher(QString computer, QString app,
                   StreamingPreferences* preferences, QObject *parent)
    : QObject(parent),
      m_DPtr(new LauncherPrivate(this))
{
    Q_D(Launcher);
    d->m_ComputerName = computer;
    d->m_AppName = app;
    d->m_Preferences = preferences;
    d->m_State = StateInit;
    d->m_TimeoutTimer = new QTimer(this);
    d->m_TimeoutTimer->setSingleShot(true);
    connect(d->m_TimeoutTimer, &QTimer::timeout,
            this, &Launcher::onTimeout);
}

Launcher::~Launcher()
{
}

void Launcher::execute(ComputerManager *manager)
{
    Q_D(Launcher);
    Event event(Event::Executed);
    event.computerManager = manager;
    d->handleEvent(event);
}

void Launcher::quitRunningApp()
{
    Q_D(Launcher);
    Event event(Event::AppQuitRequested);
    d->handleEvent(event);
}

bool Launcher::isExecuted() const
{
    Q_D(const Launcher);
    return d->m_State != StateInit;
}

void Launcher::onComputerFound(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerFound);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onComputerUpdated(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerUpdated);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onTimeout()
{
    Q_D(Launcher);
    Event event(Event::Timedout);
    d->handleEvent(event);
}

void Launcher::onQuitAppCompleted(QVariant error)
{
    Q_D(Launcher);
    Event event(Event::AppQuitCompleted);
    event.errorMessage = error.toString();
    d->handleEvent(event);
}

}
