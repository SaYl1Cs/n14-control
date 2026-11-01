#include "window.h"
#include "hardware_integration.h"
#include "reference_ownership.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <memory>

namespace {

QJsonObject ownershipFactsJson(const n14::reference::EcOwnershipFacts &facts) {
    return QJsonObject{
        {"runtime_directory", QJsonObject{
            {"is_directory", facts.runtimeDirectoryIsDirectory},
            {"no_symlink", facts.runtimeDirectoryNoSymlink},
            {"query_accessible", facts.runtimeDirectoryQueryAccessible},
            {"owner_uid", static_cast<qint64>(facts.runtimeDirectoryOwnerUid)},
            {"group_gid", static_cast<qint64>(facts.runtimeDirectoryGroupGid)},
            {"mode", QString::number(facts.runtimeDirectoryMode, 8)}}},
        {"lock_file", QJsonObject{
            {"exists", facts.lockFileExists},
            {"is_regular", facts.lockFileIsRegular},
            {"no_symlink", facts.lockFileNoSymlink},
            {"query_accessible", facts.lockFileQueryAccessible},
            {"owner_uid", static_cast<qint64>(facts.lockFileOwnerUid)},
            {"group_gid", static_cast<qint64>(facts.lockFileGroupGid)},
            {"mode", QString::number(facts.lockFileMode, 8)},
            {"link_count", static_cast<qint64>(facts.lockFileLinkCount)},
            {"held", facts.lockHeld}}},
        {"competing_owners", QJsonArray::fromStringList(facts.competingOwners)},
        {"sleep_allowed", facts.sleepAllowed},
        {"acpi_endpoint_present", facts.acpiEndpointPresent},
        {"acpi_endpoint_query_accessible", facts.acpiEndpointQueryAccessible},
        {"read_only_transport_verified", facts.readOnlyTransportVerified},
        {"identity_transport_failure", facts.identityFailureReason},
        {"dsdt_readable", facts.dsdtReadable},
        {"identity", QJsonObject{
            {"vendor", facts.vendor},
            {"model", facts.model},
            {"bios", facts.bios},
            {"dmi_ec_firmware_release", facts.dmiEcVersion},
            {"ec_version", facts.ecVersion},
            {"controller_id", facts.controllerId},
            {"dsdt_sha256", facts.dsdtSha256},
            {"wmi_guid", facts.wmiGuid},
            {"acpi_method", facts.acpiMethod}}}
    };
}

int runOwnershipPreflight() {
    n14::reference::EcReadOnlyProbe probe;
    const auto facts=probe.inspect();
    const auto decision=n14::reference::evaluateEcOwnership(facts);
    const auto &paths=probe.paths();
    const QJsonObject report{
        {"schema", 1},
        {"mode", "read-only"},
        {"hardware_writes", false},
        {"lock_acquired", false},
        {"identity_authority", "pinned_injected_source_required"},
        {"paths", QJsonObject{
            {"sys_root", paths.sysRoot},
            {"proc_root", paths.procRoot},
            {"runtime_directory", paths.runtimeDirectory},
            {"lock_path", paths.lockPath},
            {"acpi_call_path", paths.acpiCallPath}}},
        {"facts", ownershipFactsJson(facts)},
        {"decision", QJsonObject{
            {"control_allowed", decision.allowed},
            {"reason", decision.reason}}},
        {"required_next_gates", QJsonArray{
            "broker-owned lock lease",
            "pinned production EC identity source",
            "sleep lifecycle gate",
            "physical N14 protocol validation"}}
    };
    QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Indented);
    // A denied report is a valid read-only result, but must not be mistaken
    // for permission to start a writer.
    return decision.allowed ? 0 : 2;
}

} // namespace

int main(int argc,char **argv) {
    bool probe=false,desktopProbe=false,ownershipPreflight=false;
    for (int i=1;i<argc;++i) {
        if (QByteArray(argv[i])=="--probe") probe=true;
        if (QByteArray(argv[i])=="--probe-desktop") desktopProbe=true;
        if (QByteArray(argv[i])=="--ownership-preflight") ownershipPreflight=true;
    }
    if (probe || desktopProbe || ownershipPreflight) {
        QCoreApplication app(argc,argv);
        if (ownershipPreflight) return runOwnershipPreflight();
        if (!desktopProbe) {
            QTextStream(stdout)<<QJsonDocument(n14::TelemetryReader().read().toJson()).toJson();
            return 0;
        }
        n14::DesktopBackend backend(false);
        QObject::connect(&backend,&n14::DesktopBackend::notice,&app,[](const QString &m,bool e){
            if (e) QTextStream(stderr)<<m<<'\n';
        });
        QTimer::singleShot(2500,&app,[&]{
            QJsonObject out{{"power_service_available",backend.powerAvailable()},{"active_profile",backend.activeProfile()}};
            if (const auto p=backend.panel()) {
                const auto m=p->current();
                out["panel"]=QJsonObject{{"name",p->name},{"refresh_hz",m ? m->hz : 0},
                    {"has_60hz",p->modeForHz(60).has_value()},{"has_165hz",p->modeForHz(165).has_value()}};
            }
            QTextStream(stdout)<<QJsonDocument(out).toJson();
            app.exit(backend.activeProfile().isEmpty() || !backend.panel() ? 1 : 0);
        });
        return app.exec();
    }
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName("n14-control");
    QCoreApplication::setApplicationVersion("0.2.1");
    QGuiApplication::setDesktopFileName("n14-control");
    QCoreApplication::setOrganizationName("N14");
    QCommandLineParser parser;
    parser.setApplicationDescription("N14 Control — пользовательские профили, питание, экран, CPU и gated N14 EC control.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"demo","Демонстрация без аппаратных изменений и пользовательских настроек"});
    parser.addOption({"background","Запуск только в трее (без трея запуск отклоняется)"});
    parser.addOption({"smoke-test","Проверить все вкладки в демо, сохранить снимки и выйти","png-path"});
    parser.addOption({"probe","JSON пассивной телеметрии; графическая сессия не нужна"});
    parser.addOption({"probe-desktop","Проверить D-Bus и KScreen только чтением, вывести JSON"});
    parser.addOption({"ownership-preflight","Собрать read-only ownership/identity report; lock не приобретается, EC не открывается"});
    parser.addOption({"hardware-test","Реальный fan/keylight test через приложение; выполняет EC writes","json-path"});
    parser.process(app);
    if (parser.isSet("hardware-test"))
        return n14::runHardwareIntegrationTest(app,parser.value("hardware-test"));
    const bool demo=parser.isSet("demo") || parser.isSet("smoke-test");
    QTemporaryDir demoDir;
    if (demo && !demoDir.isValid()) { QTextStream(stderr)<<"Не удалось создать временный каталог\n"; return 1; }
    const QString cache=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    std::unique_ptr<QLockFile> lock;
    QLocalServer server;
    const QString socketPath=cache+"/ui.sock";
    if (!demo) {
        if (!QDir().mkpath(cache)) { QTextStream(stderr)<<"Не удалось создать каталог кэша\n"; return 1; }
        lock=std::make_unique<QLockFile>(cache+"/instance.lock");
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            QLocalSocket client; client.connectToServer(socketPath);
            if (client.waitForConnected(1000)) { client.write("show\n"); client.waitForBytesWritten(1000); return 0; }
            QTextStream(stderr)<<"Другой экземпляр работает, но его окно недоступно\n"; return 1;
        }
        QLocalServer::removeServer(socketPath);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(socketPath)) { QTextStream(stderr)<<server.errorString()<<'\n'; return 1; }
    }
    app.setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window,QColor("#182331")); palette.setColor(QPalette::WindowText,QColor("#e6edf5"));
    palette.setColor(QPalette::Base,QColor("#101a27")); palette.setColor(QPalette::AlternateBase,QColor("#223043"));
    palette.setColor(QPalette::Text,QColor("#e6edf5")); palette.setColor(QPalette::Button,QColor("#29394d"));
    palette.setColor(QPalette::ButtonText,QColor("#e6edf5")); palette.setColor(QPalette::Highlight,QColor("#368d80"));
    palette.setColor(QPalette::HighlightedText,Qt::white); palette.setColor(QPalette::ToolTipBase,QColor("#223043"));
    palette.setColor(QPalette::ToolTipText,QColor("#e6edf5")); app.setPalette(palette);
    app.setStyleSheet("QGroupBox {font-weight:600; margin-top:10px; padding-top:14px;} QGroupBox::title {subcontrol-origin:margin; left:10px;} QPushButton {padding:7px 12px;} QTabWidget::pane {border:1px solid #344354;} QTabBar::tab {padding:10px 18px;} QLabel {padding:2px;}");
    const QString config=demo ? demoDir.path()+"/config.json" : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/config.json";
    n14::Window window(config,demo);
    QObject::connect(&server,&QLocalServer::newConnection,&window,[&]{
        while (server.hasPendingConnections()) {
            QLocalSocket *peer=server.nextPendingConnection();
            QObject::connect(peer,&QLocalSocket::disconnected,peer,&QObject::deleteLater);
            QObject::connect(peer,&QLocalSocket::readyRead,&window,[&,peer]{
                if (peer->read(16)=="show\n") window.showWindow();
                peer->disconnectFromServer();
            });
            QTimer::singleShot(1500,peer,[peer]{ peer->abort(); peer->deleteLater(); });
        }
    });
    if (parser.isSet("background")) {
        if (!window.trayAvailable()) { QTextStream(stderr)<<"Системный трей недоступен; фоновый запуск отменён\n"; return 2; }
    } else window.showWindow();
    app.setQuitOnLastWindowClosed(!window.trayAvailable());
    if (parser.isSet("smoke-test")) {
        const QString path=parser.value("smoke-test");
        const int count=window.findChild<QTabWidget *>()->count();
        for (int i=0;i<=count;++i) QTimer::singleShot(300*(i+1),&window,[&,i,path,count]{
            auto *tabs=window.findChild<QTabWidget *>();
            const int index=i%count; tabs->setCurrentIndex(index);
            const QString output=i==count ? path : QFileInfo(path).absolutePath()+"/smoke-tab-"+QString::number(index)+".png";
            if (!window.grab().save(output)) { QTextStream(stderr)<<"Ошибка снимка: "<<output<<'\n'; app.exit(1); }
            else if (i==count) app.quit();
        });
    }
    return app.exec();
}