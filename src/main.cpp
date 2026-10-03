#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QMessageBox>
#include <QStyleFactory>
#include <QScopeGuard>
#include <QSettings>
#include <memory>

int main(int argc,char **argv) {
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(true);
    app.setApplicationName("LianDianQi"); app.setOrganizationName("DianXu"); app.setApplicationVersion("1.0.0");
    const QString testSettings=qEnvironmentVariable("LIANDIANQI_TEST_SETTINGS");
    if(!testSettings.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,testSettings);
    }
    app.setStyle(QStyleFactory::create("Fusion")); qRegisterMetaType<InputKey>();
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("点序 · 轻量 C++ / Qt 连点器")); parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({QCommandLineOption("show","Show the running window (or launch)."),QCommandLineOption("quit","Gracefully close the running instance."),
        QCommandLineOption("toggle","Toggle the current task of an existing instance."),QCommandLineOption("stop-task","Stop the task without closing."),
        QCommandLineOption("status","Exit 0 if the application is running, 1 otherwise."),QCommandLineOption("script","Load a script on launch.","file"),
        QCommandLineOption("run","Run the selected task on launch."),QCommandLineOption("screenshot","Render a preview and exit (no saved settings or shortcuts).","file"),
        QCommandLineOption("flow-preview","Render the script tab with --screenshot.")});
    parser.process(app);
    QString user=qEnvironmentVariable("USERNAME")+qEnvironmentVariable("USERDOMAIN")+testSettings;
    QString name="LianDianQi-"+QString::fromLatin1(QCryptographicHash::hash(user.toUtf8(),QCryptographicHash::Sha256).toHex().left(16));
    QString command=parser.isSet("quit")?"quit":parser.isSet("stop-task")?"stop":parser.isSet("toggle")?"toggle":parser.isSet("status")?"status":"show";
    bool control=parser.isSet("quit") || parser.isSet("toggle") || parser.isSet("stop-task") || parser.isSet("status");
    bool preview=parser.isSet("screenshot");
    HANDLE mutex=nullptr;
    if(!preview) {
        std::wstring mutexName=("Local\\"+name).toStdWString();
        mutex=CreateMutexW(nullptr,FALSE,mutexName.c_str());
        if(!mutex) return 2;
        bool exists=GetLastError()==ERROR_ALREADY_EXISTS;
        if(exists) {
            QLocalSocket socket;
            for(int attempt=0;attempt<10;++attempt) {
                socket.connectToServer(name);
                if(socket.waitForConnected(200)) break;
                socket.abort(); Sleep(100);
            }
            if(socket.state()!=QLocalSocket::ConnectedState) { CloseHandle(mutex); return 2; }
            socket.write(command.toUtf8()+"\n"); socket.flush(); socket.waitForBytesWritten(1000);
            bool answered=socket.bytesAvailable()>0 || socket.waitForReadyRead(2000);
            bool ok=answered && socket.readAll().startsWith("OK"); CloseHandle(mutex); return ok?0:2;
        }
        if(control) { CloseHandle(mutex); return parser.isSet("status")?1:0; }
    }
    auto releaseMutex=qScopeGuard([&] { if(mutex) CloseHandle(mutex); });
    QLocalServer server;
    if(!preview) {
        QLocalServer::removeServer(name); server.setSocketOptions(QLocalServer::UserAccessOption);
        if(!server.listen(name)) { QMessageBox::critical(nullptr,QStringLiteral("启动失败"),QStringLiteral("无法创建本地控制接口：%1").arg(server.errorString())); return 2; }
    }
    Window window(nullptr,preview);
    if(parser.isSet("script") && !window.loadScript(parser.value("script"))) return 3;
    QObject::connect(&server,&QLocalServer::newConnection,&app,[&] {
        while(auto *socket=server.nextPendingConnection()) {
            QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            QTimer::singleShot(2000,socket,[socket] { socket->disconnectFromServer(); });
            QObject::connect(socket,&QLocalSocket::readyRead,&window,[&,socket] {
                if(socket->bytesAvailable()>256) { socket->disconnectFromServer(); return; }
                if(!socket->canReadLine()) return;
                QByteArray request=socket->readLine().trimmed();
                if(request!="show" && request!="quit" && request!="stop" && request!="toggle" && request!="status") { socket->write("ERROR\n"); socket->disconnectFromServer(); return; }
                socket->write("OK\n"); socket->flush(); socket->disconnectFromServer();
                if(request=="show") window.showWindow();
                else if(request=="stop") window.stopTask();
                else if(request=="toggle") window.toggleTask();
                else if(request=="quit") { window.shutdown(); QTimer::singleShot(0,&app,&QApplication::quit); }
            });
        }
    });
    QObject::connect(&app,&QCoreApplication::aboutToQuit,&window,&Window::shutdown);
    window.show();
    if(preview) {
        if(parser.isSet("flow-preview")) window.findChild<QTabWidget *>("tabs")->setCurrentIndex(1);
        QTimer::singleShot(250,&window,[&] { bool saved=window.grab().save(parser.value("screenshot")); app.exit(saved?0:4); });
    } else if(parser.isSet("run")) QTimer::singleShot(0,&window,&Window::toggleTask);
    int code=app.exec(); window.shutdown(); server.close(); return code;
}
