#include "service.h"
#include "hardware.h"
#include "fan_controller.h"
#include "fan_client.h"
#include <QtTest>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <time.h>
#include <unistd.h>

struct AuthSubject { QString kind; QVariantMap details; };
struct AuthResult { bool authorized=false,challenge=false; QMap<QString,QString> details; };
Q_DECLARE_METATYPE(AuthSubject)
Q_DECLARE_METATYPE(AuthResult)
QDBusArgument &operator<<(QDBusArgument &a,const AuthSubject &s) { a.beginStructure(); a<<s.kind<<s.details; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a,AuthSubject &s) { a.beginStructure(); a>>s.kind>>s.details; a.endStructure(); return a; }
QDBusArgument &operator<<(QDBusArgument &a,const AuthResult &r) { a.beginStructure(); a<<r.authorized<<r.challenge<<r.details; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a,AuthResult &r) { a.beginStructure(); a>>r.authorized>>r.challenge>>r.details; a.endStructure(); return a; }
using AuthDetails=QMap<QString,QString>;
class FakeAuthority : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.freedesktop.PolicyKit1.Authority")
public slots:
    AuthResult CheckAuthorization(const AuthSubject &subject,const QString &action,const AuthDetails &,uint flags,const QString &) {
        if (action=="org.n14control.modify-ec") {
            ++ecChecks; ecFlags=flags; ecSubject=subject;
        }
        if (action=="org.n14control.modify-cpu") ++cpuChecks;
        const AuthResult result{action=="org.n14control.modify-cpu" ||
            (action=="org.n14control.modify-ec" && allowEc),false,{}};
        if (delayMilliseconds>0) {
            setDelayedReply(true);
            const auto request=message(); const auto bus=connection();
            QTimer::singleShot(delayMilliseconds,this,[request,bus,result] {
                bus.send(request.createReply(QVariant::fromValue(result)));
            });
            return {};
        }
        return result;
    }
public:
    int ecChecks=0;
    int cpuChecks=0;
    uint ecFlags=0;
    AuthSubject ecSubject;
    bool allowEc=true;
    int delayMilliseconds=0;
};

class FixtureFileSystem final : public n14::reference::EcReadOnlyFileSystem {
    n14::reference::PosixEcReadOnlyFileSystem delegate_;
public:
    n14::reference::EcReadOnlyFileStatus status(const QString &path) const override {
        return delegate_.status(path);
    }
    QByteArray readFile(const QString &path,qsizetype maximumBytes,bool *ok) const override {
        return delegate_.readFile(path,maximumBytes,ok);
    }
    QStringList listDirectory(const QString &path) const override {
        return delegate_.listDirectory(path);
    }
};

class EmptyProcessSource final : public n14::reference::EcReadOnlyProcessSource {
public:
    QStringList processNames(const QString &) const override { return {}; }
};

class FixtureIdentitySource final : public n14::reference::EcReadOnlyIdentitySource {
public:
    n14::reference::EcReadOnlyIdentityFacts inspect() const override {
        return {true,{},"2.0","55:71","\\_SB.AMW0.WMBA"};
    }
};

class SharedFakeTransport final : public n14::reference::EcRegisterTransport {
    std::shared_ptr<n14::reference::FakeAcpiEcTransport> transport_;
public:
    explicit SharedFakeTransport(std::shared_ptr<n14::reference::FakeAcpiEcTransport> transport)
        : transport_(std::move(transport)) {}
    bool open(const n14::reference::EcOwnershipDecision &decision) override {
        return transport_->open(decision);
    }
    void close() override { transport_->close(); }
    int read(const QString &registerName) override { return transport_->read(registerName); }
    void write(const QString &registerName,int value) override { transport_->write(registerName,value); }
};

// Test-only IPC bridge to the REAL HardwareService/FakeEcTransactionQueue.
// The fixture uses synthetic DSDT bytes: normalize only its already-validated
// identity receipt to the pinned identity expected by the production client.
// No command/provenance/failure/readback is rewritten. Not compiled into apps.
class FixtureHardwareBridge final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.n14control.Hardware1")
public:
    explicit FixtureHardwareBridge(QDBusConnection client) : client_(std::move(client)) {}
    std::function<void()> afterApply;
public slots:
    QString Capabilities() { return forward("Capabilities"); }
    QString GetEcProvenance() { return forward("GetEcProvenance"); }
    QString ApplyEc(const QString &json) { return forward("ApplyEc",{json}); }
private:
    QString forward(const QString &method,const QVariantList &arguments={}) {
        const auto request=message(); const auto replyBus=connection(); setDelayedReply(true);
        auto call=QDBusMessage::createMethodCall("org.n14control.TestHardware",n14::HardwarePath,n14::HardwareInterface,method);
        call.setArguments(arguments);
        auto *watcher=new QDBusPendingCallWatcher(client_.asyncCall(call,7000),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,request,replyBus,method] {
            const QDBusPendingReply<QString> result=*watcher;
            if (result.isError()) replyBus.send(request.createErrorReply(result.error().name(),result.error().message()));
            else {
                auto object=QJsonDocument::fromJson(result.value().toUtf8()).object();
                if (method=="ApplyEc" && afterApply) afterApply();
                if (method=="GetEcProvenance") {
                    auto identity=object["identity"].toObject();
                    identity["dsdt_sha256"]=n14::reference::EcOwnershipPolicy::n14().expectedDsdtSha256;
                    object["identity"]=identity;
                }
                replyBus.send(request.createReply(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))));
            }
            watcher->deleteLater();
        });
        return {};
    }
    QDBusConnection client_;
};

class ServiceTests : public QObject {
    Q_OBJECT
    QTemporaryDir root_;
    QDBusConnection bus_=QDBusConnection::systemBus();
    QDBusConnection client_=QDBusConnection("unconnected-test-client");
    QDBusConnection disconnectClient_=QDBusConnection("unconnected-disconnect-client");
    HardwareService *service_=nullptr;
    QString thermalSnapshotPath_;
    QString ecRuntimeDirectory_;
    QString dsdtSha256_;
    quint64 nextSampleId_=0;
    QPair<QString,QString> gpuLive_={"suspended","D3cold"};
    std::function<void()> beforeGpuLiveRead_;
    std::shared_ptr<n14::reference::FakeAcpiEcTransport> ecTransport_;
    static void write(const QString &path,const QByteArray &data) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data),data.size());
    }
    void registerAuthority(FakeAuthority &authority) {
        QVERIFY(bus_.registerService("org.freedesktop.PolicyKit1"));
        QVERIFY(bus_.registerObject("/org/freedesktop/PolicyKit1/Authority",&authority,QDBusConnection::ExportAllSlots));
    }
    void unregisterAuthority() {
        bus_.unregisterObject("/org/freedesktop/PolicyKit1/Authority");
        bus_.unregisterService("org.freedesktop.PolicyKit1");
    }
    void writeCurrentThermalSnapshot(const QString &mode="suspended",int generation=1,int cpu=61000) {
        timespec clock{};
        QVERIFY(::clock_gettime(CLOCK_BOOTTIME,&clock)==0);
        const double sampledAt=static_cast<double>(clock.tv_sec)+static_cast<double>(clock.tv_nsec)/1e9-0.01;
        const qint64 unixMs=QDateTime::currentMSecsSinceEpoch()-10;
        const qint64 monotonicNs=static_cast<qint64>(sampledAt*1e9);
        const quint64 sampleId=++nextSampleId_;
        const QString session=QStringLiteral("12345678-1234-4234-8234-123456789abc");
        const QJsonObject sample{{"session",session},{"generation",generation},{"sample_id",static_cast<qint64>(sampleId)},
                                 {"sampled_at_unix_ms",unixMs},{"sampled_at_monotonic_ns",monotonicNs},
                                 {"sensor","cpu"},{"power_state",mode}};
        QJsonObject snapshot{
            {"schema_version",2},{"source","ec0/io"},{"sampled_at",sampledAt},
            {"sampled_at_unix_ms",unixMs},{"sampled_at_monotonic_ns",monotonicNs},
            {"sampler_session",session},{"sampler_generation",generation},
            {"sample_id",static_cast<qint64>(sampleId)},{"gpu_state",mode},
            {"gpu_power_state",mode=="active" ? "D0" : "D3cold"},{"cpu_temp",cpu},{"gpu_temp",QJsonValue(QJsonValue::Null)},
            {"gpu_sampled_at",QJsonValue(QJsonValue::Null)},{"cpu_sample",sample},
            {"gpu_sample",QJsonValue(QJsonValue::Null)},{"errors",QJsonArray{}}};
        if (mode=="active") {
            auto gpu=sample; gpu["sensor"]="gpu";
            snapshot["gpu_temp"]=50000; snapshot["gpu_sampled_at"]=sampledAt; snapshot["gpu_sample"]=gpu;
        }
        write(thermalSnapshotPath_,QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
        QVERIFY(QFile::setPermissions(thermalSnapshotPath_,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
    }
    QJsonObject readThermalSnapshot() const {
        QFile file(thermalSnapshotPath_);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    void writeGpuErrorSnapshot(const QString &error,int generation=2,int cpu=61000) {
        writeCurrentThermalSnapshot("active",generation,cpu);
        auto snapshot=readThermalSnapshot();
        snapshot["gpu_state"]="error";
        for (const auto &field : {"gpu_power_state","gpu_temp","gpu_sampled_at","gpu_sample"})
            snapshot[field]=QJsonValue(QJsonValue::Null);
        auto sample=snapshot["cpu_sample"].toObject(); sample["power_state"]="error";
        snapshot["cpu_sample"]=sample;
        snapshot["errors"]=QJsonArray{error};
        write(thermalSnapshotPath_,QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
    }
    static void gpuObservationErrors() {
        QTest::addColumn<QString>("error");
        QTest::newRow("unconfirmed-power")<<QString("GPU EC/PM read: GPU power state is unconfirmed");
        QTest::newRow("power-changed-during-temperature")<<QString("GPU EC/PM read: GPU power state changed during EC sample");
        QTest::newRow("power-changed-during-observation")<<QString("GPU EC/PM read: GPU power state changed or is unavailable during observation");
        QTest::newRow("invalid-temperature-byte")<<QString("GPU EC/PM read: EC temperature byte is outside 1..119 C");
    }
    QString ecCommand(const QJsonObject &provenance,int value=42) const {
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            {"provenance",provenance},{"target","both"},{"value",value}}
        ).toJson(QJsonDocument::Compact));
    }
    QJsonObject currentProvenance(const QString &json) const {
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    QJsonObject commandProvenance(const QJsonObject &provenance) const {
        QJsonObject result;
        for (const auto &key : {QStringLiteral("broker_owner_generation"),
                                QStringLiteral("calculation_epoch"),
                                QStringLiteral("epoch_generation"),
                                QStringLiteral("epoch_session"),
                                QStringLiteral("sample_id"),
                                QStringLiteral("sampled_at_boottime_seconds"),
                                QStringLiteral("sampled_at_unix_ms"),
                                QStringLiteral("sampler_generation"),
                                QStringLiteral("sampler_session"),
                                QStringLiteral("sleep_allowed")})
            result[key]=provenance[key];
        return result;
    }
    void prepareEcFixture(HardwareService::Options *options) {
        const QString ecRoot=root_.path()+"/ec-fixture";
        const QString sysRoot=ecRoot+"/sys";
        ecRuntimeDirectory_=ecRoot+"/runtime";
        thermalSnapshotPath_=ecRoot+"/thermal/temperatures.json";
        QDir().mkpath(ecRuntimeDirectory_);
        QVERIFY(QFile::setPermissions(ecRuntimeDirectory_,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        write(sysRoot+"/class/dmi/id/sys_vendor","COLORFUL");
        write(sysRoot+"/class/dmi/id/product_name","N14 24");
        write(sysRoot+"/class/dmi/id/bios_version","2.07");
        write(sysRoot+"/class/dmi/id/ec_firmware_release","diagnostic-only");
        const QByteArray dsdt=QByteArrayLiteral("fixture-dsdt-for-service-test");
        write(sysRoot+"/firmware/acpi/tables/DSDT",dsdt);
        dsdtSha256_=QString::fromLatin1(QCryptographicHash::hash(dsdt,QCryptographicHash::Sha256).toHex());
        QDir().mkpath(sysRoot+"/bus/wmi/devices/3161c7c3-489f-4074-82ae-538101cce1c2-0");
        const QString acpiCall=ecRoot+"/proc/acpi/call";
        write(acpiCall,"fixture endpoint");
        writeCurrentThermalSnapshot();
        QVERIFY(QFile::setPermissions(ecRoot+"/thermal",
                                      QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));

        n14::reference::EcReadOnlyProbePaths paths;
        paths.sysRoot=sysRoot;
        paths.procRoot=ecRoot+"/proc";
        paths.runtimeDirectory=ecRuntimeDirectory_;
        paths.lockPath=ecRuntimeDirectory_+"/ec-owner.lock";
        paths.acpiCallPath=acpiCall;
        options->ecPaths=paths;
        options->ecPolicy.requiredOwnerUid=static_cast<quint32>(::getuid());
        options->ecPolicy.requiredGroupGid=static_cast<quint32>(::getgid());
        options->ecPolicy.expectedDsdtSha256=dsdtSha256_;
        options->ecFileSystem=std::make_shared<FixtureFileSystem>();
        options->ecProcessSource=std::make_shared<EmptyProcessSource>();
        options->ecIdentitySource=std::make_shared<FixtureIdentitySource>();
        n14::reference::RuntimeSnapshotFilePolicy snapshotPolicy;
        snapshotPolicy.path=thermalSnapshotPath_;
        snapshotPolicy.requireOwner=false;
        snapshotPolicy.requireGroup=false;
        options->snapshotPolicy=snapshotPolicy;
        gpuLive_={"suspended","D3cold"};
        options->ecLiveGpuPowerState=[this] {
            if (beforeGpuLiveRead_) beforeGpuLiveRead_();
            return std::optional<QPair<QString,QString>>(gpuLive_);
        };
        ecTransport_=std::make_shared<n14::reference::FakeAcpiEcTransport>();
        ecTransport_->seed("0x50",0);
        ecTransport_->seed("0x54",40);
        ecTransport_->seed("0x55",40);
        ecTransport_->seed("0x5b",0);
        ecTransport_->seed("0x5c",35);
        options->ecTransportFactory=[transport=ecTransport_] {
            return std::make_unique<SharedFakeTransport>(transport);
        };
    }
    QDBusMessage message(const QString &method,const QVariantList &args={}) {
        auto m=QDBusMessage::createMethodCall("org.n14control.TestHardware","/org/n14control/Hardware1","org.n14control.Hardware1",method);
        m.setArguments(args); return m;
    }
    QDBusPendingReply<QString> ecCall(const QString &method,const QVariantList &args={}) {
        QDBusPendingCallWatcher watcher(client_.asyncCall(message(method,args),5000));
        QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
        connect(&watcher,&QDBusPendingCallWatcher::finished,&loop,&QEventLoop::quit);
        connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
        timeout.start(7000); if (!watcher.isFinished()) loop.exec();
        if (!watcher.isFinished()) return QDBusMessage::createError(QDBusError::Timeout,"fixture call timeout");
        return watcher;
    }
private slots:
    void init() {
        beforeGpuLiveRead_={};
        gpuLive_={"suspended","D3cold"}; writeCurrentThermalSnapshot();
        ecTransport_->seed("0x50",0); ecTransport_->seed("0x54",40); ecTransport_->seed("0x55",40);
        ecTransport_->seed("0x5b",0); ecTransport_->seed("0x5c",35); ecTransport_->clearEvents();
    }
    void initTestCase() {
        // Refuse running against the real system bus, even if invoked manually.
        QVERIFY(!qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"),qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(bus_.isConnected()); QVERIFY(root_.isValid());
        const auto polkit=bus_.interface()->isServiceRegistered("org.freedesktop.PolicyKit1");
        QVERIFY(polkit.isValid()); QVERIFY(!polkit.value());
        const QString p=root_.path()+"/devices/system/cpu/cpufreq/policy0/";
        write(p+"scaling_driver","intel_pstate"); write(p+"cpuinfo_min_freq","400000"); write(p+"cpuinfo_max_freq","4700000");
        write(p+"scaling_min_freq","400000"); write(p+"scaling_max_freq","4700000");
        write(root_.path()+"/devices/system/cpu/intel_pstate/no_turbo","0");
        qDBusRegisterMetaType<AuthSubject>(); qDBusRegisterMetaType<AuthResult>();
        qDBusRegisterMetaType<AuthDetails>(); qRegisterMetaType<AuthDetails>("AuthDetails");
        HardwareService::Options options;
        prepareEcFixture(&options);
        service_=new HardwareService(n14::CpuController(root_.path()),this,std::move(options));
        QVERIFY(bus_.registerService("org.n14control.TestHardware"));
        QVERIFY(bus_.registerObject("/org/n14control/Hardware1",service_,QDBusConnection::ExportAllSlots));
        client_=QDBusConnection::connectToBus(QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS")),"n14-test-client");
        QVERIFY(client_.isConnected());
        disconnectClient_=QDBusConnection::connectToBus(QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS")),"n14-disconnect-client");
        QVERIFY(disconnectClient_.isConnected());
    }
    void capabilitiesAreVersionedAndExposeBaseEcControl() {
        QDBusPendingCallWatcher watcher(client_.asyncCall(message("Capabilities"),3000));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        const QDBusPendingReply<QString> reply=watcher; QVERIFY2(!reply.isError(),qPrintable(reply.error().message()));
        const auto j=QJsonDocument::fromJson(reply.value().toUtf8()).object(); QCOMPARE(j["protocol"].toInt(),1);
        QVERIFY(j["cpu"].toObject()["supported"].toBool());
        QVERIFY(j["ec_writes"].toBool()); QVERIFY(j["fan_control"].toBool());
        QVERIFY(j["keyboard_lighting"].toBool());
        QCOMPARE(j["ec_protocol"].toInt(),2);
        QVERIFY(!j["mux"].toBool());
    }
    void invalidRequestsRejectedBeforeAuthorization_data() {
        QTest::addColumn<QString>("json");
        QTest::newRow("broken")<<QString("{");
        QTest::newRow("array")<<QString("[]");
        QTest::newRow("unknown-path")<<QString(R"({"mode":"limit","max_mhz":2400,"turbo":-1,"path":"/etc/passwd"})");
        QTest::newRow("oversized")<<QString(1025,'x');
        QTest::newRow("fraction")<<QString(R"({"mode":"limit","max_mhz":2400.1,"turbo":-1})");
    }
    void invalidRequestsRejectedBeforeAuthorization() {
        QFETCH(QString,json);
        QDBusPendingCallWatcher watcher(client_.asyncCall(message("ApplyCpu",{json,false}),3000));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        const QDBusPendingReply<QString> reply=watcher;
        QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::InvalidArgs);
        QCOMPARE(n14::CpuController(root_.path()).policies().first().currentMax,4700000);
    }
    void missingAuthorizationFailsClosedAndCanRetry() {
        const auto json=QString::fromUtf8(QJsonDocument(n14::CpuRequest{"limit",2400,0}.toJson()).toJson(QJsonDocument::Compact));
        for (int i=0;i<2;++i) {
            QDBusPendingCallWatcher watcher(client_.asyncCall(message("ApplyCpu",{json,false}),5000));
            QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),7000);
            const QDBusPendingReply<QString> reply=watcher; QVERIFY(reply.isError());
            QVERIFY2(reply.error().message().contains("Авторизация"),qPrintable(reply.error().message()));
            QCOMPARE(n14::CpuController(root_.path()).policies().first().currentMax,4700000);
            QVERIFY(n14::CpuController(root_.path()).inspect()["turbo_allowed"].toBool());
        }
    }
    void privateSleepHookIsNotExported() {
        auto m=QDBusMessage::createMethodCall("org.n14control.TestHardware","/org/n14control/Hardware1","org.freedesktop.DBus.Introspectable","Introspect");
        QDBusPendingCallWatcher watcher(client_.asyncCall(m,3000)); QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        const QDBusPendingReply<QString> reply=watcher; QVERIFY(!reply.isError());
        QVERIFY(!reply.value().contains("prepareForSleep"));
        QVERIFY(!reply.value().contains("AcquireEc"));
        QVERIFY(reply.value().contains("GetEcProvenance"));
        QVERIFY(reply.value().contains("ApplyEc"));
        QVERIFY(!reply.value().contains("ReleaseEc"));
        QVERIFY(!reply.value().contains("readEcProvenance"));
        QVERIFY(!reply.value().contains("applyEcCommand"));
    }
    void invalidEcCommandRejectedBeforeAuthorization() {
        FakeAuthority authority;
        registerAuthority(authority);
        const QString command=QStringLiteral("{\"provenance\":{},\"target\":\"both\",\"value\":50}");
        QDBusPendingCallWatcher apply(client_.asyncCall(message("ApplyEc",{command}),3000));
        QTRY_VERIFY_WITH_TIMEOUT(apply.isFinished(),5000);
        const QDBusPendingReply<QString> applyReply=apply;
        QVERIFY(applyReply.isError()); QCOMPARE(applyReply.error().type(),QDBusError::InvalidArgs);
        QCOMPARE(authority.ecChecks,0);
        QVERIFY(!ecTransport_->isOpen());
    }
    void ecObservationWithoutAuthorizationDoesNotOpenTransport() {
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),3000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),5000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY(provenanceReply.isError());
        QVERIFY(provenanceReply.error().message().contains("Авторизация"));
        QVERIFY(!ecTransport_->isOpen());
    }
    void deniedEcAuthorizationCanBeRetriedWithoutWrites() {
        FakeAuthority authority; authority.allowEc=false;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher denied(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(denied.isFinished(),7000);
        const QDBusPendingReply<QString> deniedReply=denied;
        QVERIFY(deniedReply.isError()); QVERIFY(!ecTransport_->isOpen());
        QVERIFY(ecTransport_->events().isEmpty());
        authority.allowEc=true;
        QDBusPendingCallWatcher allowed(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(allowed.isFinished(),7000);
        const QDBusPendingReply<QString> allowedReply=allowed;
        QVERIFY2(!allowedReply.isError(),qPrintable(allowedReply.error().message()));
        QCOMPARE(authority.ecChecks,2);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
    }
    void pendingEcAuthorizationRejectsCompetingRequestAndSleepCancelsIt() {
        FakeAuthority authority; authority.delayMilliseconds=300;
        registerAuthority(authority);
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher pending(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_COMPARE_WITH_TIMEOUT(authority.ecChecks,1,2000);
        QDBusPendingCallWatcher competing(disconnectClient_.asyncCall(message("GetEcProvenance"),3000));
        QTRY_VERIFY_WITH_TIMEOUT(competing.isFinished(),5000);
        const QDBusPendingReply<QString> competingReply=competing;
        QVERIFY(competingReply.isError()); QCOMPARE(competingReply.error().type(),QDBusError::LimitsExceeded);
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        QTRY_VERIFY_WITH_TIMEOUT(pending.isFinished(),5000);
        const QDBusPendingReply<QString> pendingReply=pending;
        QVERIFY(pendingReply.isError());
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        QTest::qWait(400); // A late Polkit grant must not reopen the broker.
        QVERIFY(!ecTransport_->isOpen()); QVERIFY(ecTransport_->events().isEmpty());
        QCOMPARE(authority.ecChecks,1);
    }
    void disconnectDuringEcAuthorizationCannotStartBroker() {
        FakeAuthority authority; authority.delayMilliseconds=300;
        registerAuthority(authority);
        auto caller=QDBusConnection::connectToBus(QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS")),"n14-pending-client");
        QVERIFY(caller.isConnected());
        ecTransport_->clearEvents();
        auto pending=std::make_unique<QDBusPendingCallWatcher>(caller.asyncCall(message("GetEcProvenance"),5000));
        QTRY_COMPARE_WITH_TIMEOUT(authority.ecChecks,1,2000);
        pending.reset();
        QDBusConnection::disconnectFromBus("n14-pending-client");
        caller=QDBusConnection("closed-pending-client");
        QTest::qWait(500);
        QVERIFY(!ecTransport_->isOpen()); QVERIFY(ecTransport_->events().isEmpty());
    }
    void missingSnapshotKeepsAuthorizationButNeverWrites() {
        FakeAuthority authority;
        registerAuthority(authority);
        QFile::remove(thermalSnapshotPath_);
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher missing(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(missing.isFinished(),7000);
        const QDBusPendingReply<QString> missingReply=missing;
        QVERIFY(missingReply.isError()); QCOMPARE(missingReply.error().type(),QDBusError::LimitsExceeded);
        QVERIFY(ecTransport_->isOpen());
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher fresh(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(fresh.isFinished(),7000);
        const QDBusPendingReply<QString> freshReply=fresh;
        QVERIFY2(!freshReply.isError(),qPrintable(freshReply.error().message()));
        QCOMPARE(authority.ecChecks,1);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
    }
    void fanAndKeylightSurviveRepeatedCleanGpuEpochChanges() {
        FakeAuthority authority; registerAuthority(authority);
        quint64 owner=0;
        for (int generation=1;generation<=5;++generation) {
            const QString mode=generation%2 ? "suspended" : "active";
            gpuLive_={mode,mode=="active" ? "D0" : "D3cold"};
            writeCurrentThermalSnapshot(mode,generation);
            const auto observed=ecCall("GetEcProvenance");
            QVERIFY2(!observed.isError(),qPrintable(observed.error().message()));
            const auto object=currentProvenance(observed.value());
            const auto next=object["broker_owner_generation"].toInteger();
            QVERIFY(next>static_cast<qint64>(owner)); owner=next;
            auto provenance=commandProvenance(object); provenance["calculation_epoch"]=1;
            const auto fan=ecCall("ApplyEc",{ecCommand(provenance,60)});
            QVERIFY2(!fan.isError(),qPrintable(fan.error().message()));
            provenance["calculation_epoch"]=2;
            const auto light=QString::fromUtf8(QJsonDocument(QJsonObject{
                {"provenance",provenance},{"target","keylight"},{"value",generation%2 ? 50 : 100}}
            ).toJson(QJsonDocument::Compact));
            const auto lighting=ecCall("ApplyEc",{light});
            QVERIFY2(!lighting.isError(),qPrintable(lighting.error().message()));
            QCOMPARE(ecTransport_->read("0x50"),2);
            QCOMPARE(ecTransport_->read("0x54"),n14::reference::fanDutyToRaw(60));
            QCOMPARE(ecTransport_->read("0x55"),n14::reference::fanDutyToRaw(60));
        }
        QCOMPARE(authority.ecChecks,1);
    }
    void gpuObservationGapReturnsOnlyTypedPendingAndNoEcOperations() {
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        const auto old=currentProvenance(first.value());
        auto p=commandProvenance(old); p["calculation_epoch"]=1;
        QVERIFY(!ecCall("ApplyEc",{ecCommand(p,60)}).isError());
        gpuLive_={"active","D0"}; ecTransport_->clearEvents();
        const auto pending=ecCall("GetEcProvenance");
        QVERIFY(pending.isError()); QCOMPARE(pending.error().name(),QString(n14::EcSamplePending));
        QVERIFY(ecTransport_->events().isEmpty());
        writeCurrentThermalSnapshot("active",2);
        const auto fresh=ecCall("GetEcProvenance"); QVERIFY2(!fresh.isError(),qPrintable(fresh.error().message()));
        const auto current=currentProvenance(fresh.value());
        QVERIFY(current["broker_owner_generation"].toInteger()>old["broker_owner_generation"].toInteger());
        auto next=commandProvenance(current); next["calculation_epoch"]=1;
        QVERIFY(!ecCall("ApplyEc",{ecCommand(next,70)}).isError());
        QCOMPARE(authority.ecChecks,1);
    }
    void nativeGpuObservationErrorsWaitWithoutEcOperations_data() { gpuObservationErrors(); }
    void nativeGpuObservationErrorsWaitWithoutEcOperations() {
        QFETCH(QString,error);
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        const auto old=currentProvenance(first.value());
        auto p=commandProvenance(old); p["calculation_epoch"]=1;
        QVERIFY(!ecCall("ApplyEc",{ecCommand(p,60)}).isError());
        writeGpuErrorSnapshot(error); ecTransport_->clearEvents();
        const auto pending=ecCall("GetEcProvenance");
        QVERIFY(pending.isError()); QCOMPARE(pending.error().name(),QString(n14::EcSamplePending));
        QVERIFY(ecTransport_->events().isEmpty());
        writeCurrentThermalSnapshot("active",3,92000); gpuLive_={"active","D0"};
        const auto fresh=ecCall("GetEcProvenance"); QVERIFY2(!fresh.isError(),qPrintable(fresh.error().message()));
        const auto current=currentProvenance(fresh.value());
        QVERIFY(current["sample_id"].toInteger()>old["sample_id"].toInteger());
        QVERIFY(current["broker_owner_generation"].toInteger()>old["broker_owner_generation"].toInteger());
        auto next=commandProvenance(current); next["calculation_epoch"]=1;
        QVERIFY(!ecCall("ApplyEc",{ecCommand(next,70)}).isError());
        QCOMPARE(authority.ecChecks,1);
    }
    void nativeGpuObservationGapInvalidatesOldProvenance_data() { gpuObservationErrors(); }
    void nativeGpuObservationGapInvalidatesOldProvenance() {
        QFETCH(QString,error);
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        auto p=commandProvenance(currentProvenance(first.value())); p["calculation_epoch"]=1;
        writeGpuErrorSnapshot(error);
        QCOMPARE(ecCall("GetEcProvenance").error().name(),QString(n14::EcSamplePending));
        ecTransport_->clearEvents();
        QVERIFY(ecCall("ApplyEc",{ecCommand(p)}).isError());
        QVERIFY(ecTransport_->events().isEmpty()); QVERIFY(!ecTransport_->isOpen());
    }
    void invalidGpuTemperatureNeverInventsOrRenewsGrace_data() {
        QTest::addColumn<bool>("established");
        QTest::newRow("no-confirmed-sample")<<false;
        QTest::newRow("original-age-expires")<<true;
    }
    void invalidGpuTemperatureNeverInventsOrRenewsGrace() {
        QFETCH(bool,established);
        FakeAuthority authority; registerAuthority(authority);
        const QString error="GPU EC/PM read: EC temperature byte is outside 1..119 C";
        if (established) {
            QVERIFY(!ecCall("GetEcProvenance").isError());
            writeGpuErrorSnapshot(error);
            QCOMPARE(ecCall("GetEcProvenance").error().name(),QString(n14::EcSamplePending));
            QTest::qWait(2600); writeGpuErrorSnapshot(error);
            QCOMPARE(ecCall("GetEcProvenance").error().name(),QString(n14::EcSamplePending));
            QTest::qWait(2600);
        }
        writeGpuErrorSnapshot(error); ecTransport_->clearEvents();
        const auto rejected=ecCall("GetEcProvenance");
        QVERIFY(rejected.isError()); QVERIFY(rejected.error().name()!=n14::EcSamplePending);
        QVERIFY(!ecTransport_->isOpen());
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
    }
    void gpuObservationErrorCannotHideInvalidSnapshots_data() {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"transport-open","transport-read","additional-cpu-error","missing-cpu",
                                  "cpu-pairing","stale-clock","fabricated-gpu","fabricated-gpu-clock"})
            QTest::newRow(fault)<<QString::fromLatin1(fault);
    }
    void gpuObservationErrorCannotHideInvalidSnapshots() {
        QFETCH(QString,fault);
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        auto p=commandProvenance(currentProvenance(first.value())); p["calculation_epoch"]=1;
        writeGpuErrorSnapshot("GPU EC/PM read: EC temperature byte is outside 1..119 C");
        auto snapshot=readThermalSnapshot();
        if (fault=="transport-open") snapshot["errors"]=QJsonArray{"GPU EC/PM read: EC temperature interface cannot be opened"};
        else if (fault=="transport-read") snapshot["errors"]=QJsonArray{"GPU EC/PM read: EC temperature read failed: Input/output error"};
        else if (fault=="additional-cpu-error") {
            auto errors=snapshot["errors"].toArray(); errors.append("CPU EC read: error"); snapshot["errors"]=errors;
        } else if (fault=="missing-cpu") { snapshot["cpu_temp"]=QJsonValue(QJsonValue::Null); snapshot["cpu_sample"]=QJsonValue(QJsonValue::Null); }
        else if (fault=="cpu-pairing") { auto sample=snapshot["cpu_sample"].toObject(); sample["sample_id"]=999; snapshot["cpu_sample"]=sample; }
        else if (fault=="stale-clock") snapshot["sampled_at"]=snapshot["sampled_at"].toDouble()-6.0;
        else if (fault=="fabricated-gpu") {
            snapshot["gpu_temp"]=50000; auto sample=snapshot["cpu_sample"].toObject(); sample["sensor"]="gpu"; snapshot["gpu_sample"]=sample;
        } else if (fault=="fabricated-gpu-clock") snapshot["gpu_sampled_at"]=snapshot["sampled_at"];
        write(thermalSnapshotPath_,QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
        ecTransport_->clearEvents();
        const auto rejected=ecCall("GetEcProvenance");
        QVERIFY(rejected.isError()); QVERIFY(rejected.error().name()!=n14::EcSamplePending);
        QVERIFY(ecTransport_->events().isEmpty());
        QVERIFY(ecCall("ApplyEc",{ecCommand(p)}).isError());
        QVERIFY(ecTransport_->events().isEmpty());
    }
    void postCommandGpuObservationErrorDoesNotDisableCurve_data() { gpuObservationErrors(); }
    void postCommandGpuObservationErrorDoesNotDisableCurve() {
        QFETCH(QString,error);
        FakeAuthority authority; registerAuthority(authority);
        writeCurrentThermalSnapshot("suspended",1,65000);
        auto peer=QDBusConnection::connectToBus(QDBusConnection::SystemBus,"ec-stack-bridge");
        QVERIFY(peer.isConnected()); FixtureHardwareBridge bridge(client_);
        QVERIFY(peer.registerService(n14::HardwareService));
        QVERIFY(peer.registerObject(n14::HardwarePath,&bridge,QDBusConnection::ExportAllSlots));
        bool inject=true;
        bridge.afterApply=[&] { if (inject) { inject=false; writeGpuErrorSnapshot(error); } };
        QTemporaryDir directory; const auto config=directory.path()+"/config.json"; n14::Settings{}.save(config);
        n14::FanController controller(config);
        const auto state=[&] { return currentProvenance(controller.Status()); };
        QTRY_VERIFY_WITH_TIMEOUT(state()["waiting_for_sample"].toBool(),2500);
        QVERIFY(!state()["failed"].toBool()); QCOMPARE(state()["confirmed_commands"].toInt(),1);
        ecTransport_->clearEvents(); QTest::qWait(1100);
        QVERIFY(!state()["failed"].toBool()); QVERIFY(ecTransport_->events().isEmpty());
        gpuLive_={"active","D0"}; writeCurrentThermalSnapshot("active",3,92000);
        QTRY_VERIFY_WITH_TIMEOUT(state()["confirmed_commands"].toInt()>1,2500);
        QTRY_VERIFY(!state()["busy"].toBool());
        QVERIFY(!state()["waiting_for_sample"].toBool()); QVERIFY(!state()["failed"].toBool());
        QCOMPARE(state()["status"].toObject()["sampler_generation"].toInt(),3);
        QCOMPARE(state()["status"].toObject()["cpu_c"].toDouble(),92.0);
        QCOMPARE(state()["automatic_retries"].toInt(),0); QCOMPARE(state()["recovery_writes"].toInt(),0);
        QCOMPARE(authority.ecChecks,1);
    }
    void gpuRaceBeforeApplyDoesNotWriteOrReplayOldCommand() {
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        auto p=commandProvenance(currentProvenance(first.value())); p["calculation_epoch"]=1;
        gpuLive_={"active","D0"}; ecTransport_->clearEvents();
        const auto pending=ecCall("ApplyEc",{ecCommand(p,90)});
        QVERIFY(pending.isError()); QCOMPARE(pending.error().name(),QString(n14::EcSamplePending));
        QVERIFY(ecTransport_->events().isEmpty());
        writeCurrentThermalSnapshot("active",2);
        const auto fresh=ecCall("GetEcProvenance"); QVERIFY(!fresh.isError());
        auto next=commandProvenance(currentProvenance(fresh.value())); next["calculation_epoch"]=1;
        const auto light=QString::fromUtf8(QJsonDocument(QJsonObject{
            {"provenance",next},{"target","keylight"},{"value",50}}
        ).toJson(QJsonDocument::Compact));
        QVERIFY(!ecCall("ApplyEc",{light}).isError());
        QCOMPARE(ecTransport_->read("0x54"),40); // Rejected 90% demand was NOT replayed.
    }
    void oldProvenanceCannotBeUsedDuringObservationGap() {
        FakeAuthority authority; registerAuthority(authority);
        const auto first=ecCall("GetEcProvenance"); QVERIFY(!first.isError());
        auto p=commandProvenance(currentProvenance(first.value())); p["calculation_epoch"]=1;
        gpuLive_={"active","D0"};
        QCOMPARE(ecCall("GetEcProvenance").error().name(),QString(n14::EcSamplePending));
        ecTransport_->clearEvents();
        QVERIFY(ecCall("ApplyEc",{ecCommand(p)}).isError());
        QVERIFY(ecTransport_->events().isEmpty()); QVERIFY(!ecTransport_->isOpen());
    }
    void gpuObservationGraceNeverRenewsAtCoolOrHighTemperature_data() {
        QTest::addColumn<bool>("hot");
        QTest::newRow("total-original-age-expires")<<false;
        QTest::newRow("hot-observation-does-not-stop-cooling")<<true;
    }
    void gpuObservationGraceNeverRenewsAtCoolOrHighTemperature() {
        QFETCH(bool,hot);
        FakeAuthority authority; registerAuthority(authority);
        writeCurrentThermalSnapshot("suspended",1,hot ? 85000 : 61000);
        QVERIFY(!ecCall("GetEcProvenance").isError());
        gpuLive_={"active","D0"};
        QCOMPARE(ecCall("GetEcProvenance").error().name(),QString(n14::EcSamplePending));
        QTest::qWait(5100); writeCurrentThermalSnapshot();
        ecTransport_->clearEvents();
        const auto expired=ecCall("GetEcProvenance");
        QVERIFY(expired.isError()); QVERIFY(expired.error().name()!=n14::EcSamplePending);
        QVERIFY(ecTransport_->events().isEmpty()); QVERIFY(!ecTransport_->isOpen());
    }
    void wholeEcStackSerializesLightingGpuTransitionsSleepAndTerminalFailure() {
        FakeAuthority authority; registerAuthority(authority);
        writeCurrentThermalSnapshot("suspended",1,92000);
        auto peer=QDBusConnection::connectToBus(QDBusConnection::SystemBus,"ec-stack-bridge");
        QVERIFY(peer.isConnected()); FixtureHardwareBridge bridge(client_);
        QVERIFY(peer.registerService(n14::HardwareService));
        QVERIFY(peer.registerObject(n14::HardwarePath,&bridge,QDBusConnection::ExportAllSlots));
        QTemporaryDir directory; const auto config=directory.path()+"/config.json"; n14::Settings{}.save(config);
        n14::FanController controller(config);
        QVERIFY(bus_.registerService(n14::FanService));
        QVERIFY(bus_.registerObject(n14::FanPath,&controller,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));
        n14::FanClient gui(false);
        QTRY_VERIFY_WITH_TIMEOUT(gui.state()["confirmed_commands"].toInt()>=1,3000);
        QTRY_VERIFY(!gui.state()["busy"].toBool());
        auto generation=gui.state()["status"].toObject()["broker_owner_generation"].toInteger();
        for (int epoch=2;epoch<=4;++epoch) {
            const QString mode=epoch%2 ? "suspended" : "active";
            gpuLive_={mode,mode=="active" ? "D0" : "D3cold"};
            // Live PM moves first: a real broker response must defer without
            // touching EC, while GUI changes collapse into one latest intent.
            QTRY_VERIFY_WITH_TIMEOUT(gui.state()["waiting_for_sample"].toBool(),2200);
            ecTransport_->clearEvents();
            gui.setKeylight(20); gui.setKeylight(epoch%2 ? 73 : 50);
            QTest::qWait(200);
            QVERIFY(ecTransport_->events().isEmpty());
            writeCurrentThermalSnapshot(mode,epoch,92000);
            QTRY_VERIFY_WITH_TIMEOUT(gui.state()["status"].toObject()["sampler_generation"].toInt()==epoch,2500);
            QTRY_VERIFY_WITH_TIMEOUT(!gui.keylightPending() && !gui.state()["busy"].toBool(),2500);
            QVERIFY(!gui.state()["failed"].toBool());
            const auto next=gui.state()["status"].toObject()["broker_owner_generation"].toInteger();
            QVERIFY(next>generation); generation=next;
            QCOMPARE(ecTransport_->read("0x5b"),n14::reference::keylightPercentToRaw(epoch%2 ? 73 : 50));
            QCOMPARE(ecTransport_->read("0x50"),2);
        }
        // Clean sleep/resume invalidates all three layers without a replay.
        auto *backend=controller.findChild<n14::HardwareBackend *>(); QVERIFY(backend);
        ecTransport_->clearEvents();
        for (bool sleeping : {true,false}) {
            QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,sleeping)));
            QVERIFY(QMetaObject::invokeMethod(backend,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,sleeping)));
            QVERIFY(QMetaObject::invokeMethod(&controller,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,sleeping)));
        }
        QVERIFY(ecTransport_->events().isEmpty()); writeCurrentThermalSnapshot("active",4,92000);
        QTRY_VERIFY_WITH_TIMEOUT(gui.state()["status"].toObject()["broker_owner_generation"].toInteger()>generation,2500);
        QTRY_VERIFY(!gui.state()["busy"].toBool());
        QVERIFY(!gui.state()["failed"].toBool());
        // A real transport error is terminal for lighting AND the fan loop.
        ecTransport_->failNextWrite("0x50"); gui.setKeylight(40);
        QTRY_VERIFY_WITH_TIMEOUT(gui.state()["failed"].toBool(),2500);
        QVERIFY(!ecTransport_->isOpen()); const auto events=ecTransport_->events().size();
        QTest::qWait(1200); QCOMPARE(ecTransport_->events().size(),events);
        QVERIFY(gui.state()["keylight_pending_percent"].isNull());
    }
    void applyWithoutObservationAuthorizesButCannotInventProvenance() {
        FakeAuthority authority;
        registerAuthority(authority);
        // A correctly shaped envelope is still not a broker-issued observation.
        QJsonObject provenance{{"broker_owner_generation",1},{"calculation_epoch",1},
            {"epoch_generation",1},{"epoch_session","session"},{"sample_id",1},
            {"sampled_at_boottime_seconds",1.0},{"sampled_at_unix_ms",1},
            {"sampler_generation",1},{"sampler_session","session"},{"sleep_allowed",true}};
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher apply(client_.asyncCall(message("ApplyEc",{ecCommand(provenance)}),5000));
        QTRY_VERIFY_WITH_TIMEOUT(apply.isFinished(),7000);
        const QDBusPendingReply<QString> reply=apply;
        QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::InvalidArgs);
        QCOMPARE(authority.ecChecks,1); QVERIFY(!ecTransport_->isOpen());
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher fresh(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(fresh.isFinished(),7000);
        const QDBusPendingReply<QString> freshReply=fresh;
        QVERIFY2(!freshReply.isError(),qPrintable(freshReply.error().message()));
        QCOMPARE(authority.ecChecks,1);
    }
    void removedLifecycleMethodsAreUnknown() {
        for (const auto &method : {QStringLiteral("AcquireEc"),QStringLiteral("ReleaseEc")}) {
            const QVariantList args=method=="AcquireEc" ? QVariantList{false} : QVariantList{};
            QDBusPendingCallWatcher watcher(client_.asyncCall(message(method,args),3000));
            QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
            const QDBusPendingReply<QString> reply=watcher;
            QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::UnknownMethod);
        }
        QVERIFY(!ecTransport_->isOpen());
    }
    void sleepGateRejectsAndResumesWithoutWrites() {
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        const auto json=QString::fromUtf8(QJsonDocument(n14::CpuRequest{"limit",2400,-1}.toJson()).toJson(QJsonDocument::Compact));
        QDBusPendingCallWatcher watcher(client_.asyncCall(message("ApplyCpu",{json,false}),3000));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        const QDBusPendingReply<QString> reply=watcher; QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::AccessDenied);
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        QCOMPARE(n14::CpuController(root_.path()).policies().first().currentMax,4700000);
    }
    void authorizedRequestAppliesOnlyToFakeSysfs() {
        FakeAuthority authority;
        registerAuthority(authority);
        const auto json=QString::fromUtf8(QJsonDocument(n14::CpuRequest{"limit",2400,0}.toJson()).toJson(QJsonDocument::Compact));
        QDBusPendingCallWatcher watcher(client_.asyncCall(message("ApplyCpu",{json,false}),5000));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),7000);
        const QDBusPendingReply<QString> reply=watcher;
        // Unregister before assertions so a failure cannot authorize later tests.
        unregisterAuthority();
        QVERIFY2(!reply.isError(),qPrintable(reply.error().message()));
        const auto result=QJsonDocument::fromJson(reply.value().toUtf8()).object(); QVERIFY(result["ok"].toBool());
        QCOMPARE(n14::CpuController(root_.path()).policies().first().currentMax,2400000);
        QVERIFY(!n14::CpuController(root_.path()).inspect()["turbo_allowed"].toBool());
    }
    void ecProvenanceStartsBrokerAndAppliesEndToEnd() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),7000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY2(!provenanceReply.isError(),qPrintable(provenanceReply.error().message()));
        auto provenanceJson=currentProvenance(provenanceReply.value());
        QVERIFY(ecTransport_->isOpen());
        QCOMPARE(authority.ecChecks,1);
        QCOMPARE(authority.ecFlags,0u);
        QCOMPARE(authority.ecSubject.kind,QString("system-bus-name"));
        QCOMPARE(authority.ecSubject.details["name"].toString(),client_.baseService());
        QCOMPARE(provenanceJson["identity"].toObject()["vendor"].toString(),QString("COLORFUL"));
        QCOMPARE(provenanceJson["identity"].toObject()["model"].toString(),QString("N14 24"));
        QCOMPARE(provenanceJson["identity"].toObject()["dsdt_sha256"].toString(),dsdtSha256_);
        QVERIFY(provenanceJson["broker_owner_generation"].toInteger()>0);
        QVERIFY(provenanceJson["sample_id"].toInteger()>0);
        QVERIFY(provenanceJson["sleep_allowed"].toBool());
        QCOMPARE(provenanceJson["gpu_state"].toString(),QString("suspended"));
        QCOMPARE(provenanceJson["fan_mode"].toInt(),0);
        QCOMPARE(provenanceJson["cpu_target_raw"].toInt(),40);
        QCOMPARE(provenanceJson["gpu_target_raw"].toInt(),40);
        QCOMPARE(provenanceJson["keylight_raw"].toInt(),0);
        QCOMPARE(provenanceJson["cpu_tach_raw"].toInt(),35);
        QDBusPendingCallWatcher repeated(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(repeated.isFinished(),7000);
        const QDBusPendingReply<QString> repeatedReply=repeated;
        QVERIFY2(!repeatedReply.isError(),qPrintable(repeatedReply.error().message()));
        // Ordinary polling may read the same fresh snapshot more than once;
        // the snapshot is not consumed as a one-shot command token.
        provenanceJson=currentProvenance(repeatedReply.value());
        const auto commandProvenanceJson=commandProvenance(provenanceJson);
        auto commandProvenanceWithEpoch=commandProvenanceJson;
        commandProvenanceWithEpoch["calculation_epoch"]=1;

        QDBusPendingCallWatcher apply(client_.asyncCall(message("ApplyEc",{ecCommand(commandProvenanceWithEpoch)}),5000));
        QTRY_VERIFY_WITH_TIMEOUT(apply.isFinished(),7000);
        const QDBusPendingReply<QString> applyReply=apply;
        QVERIFY2(!applyReply.isError(),qPrintable(applyReply.error().message()));
        const auto applied=QJsonDocument::fromJson(applyReply.value().toUtf8()).object();
        QVERIFY(applied["ok"].toBool());
        QCOMPARE(applied["raw_readback"].toInt(),ecTransport_->read("0x54"));
        QVERIFY(std::any_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write" && event.registerName=="0x50" && event.value==2; }));

        const auto firstGeneration=provenanceJson["broker_owner_generation"].toInteger();
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        QVERIFY(!ecTransport_->isOpen());
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher resumed(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(resumed.isFinished(),7000);
        const QDBusPendingReply<QString> resumedReply=resumed;
        QVERIFY2(!resumedReply.isError(),qPrintable(resumedReply.error().message()));
        QVERIFY(currentProvenance(resumedReply.value())["broker_owner_generation"].toInteger()>firstGeneration);
        QCOMPARE(authority.ecChecks,1);
        unregisterAuthority();
    }
    void callerDisconnectRevokesEcOwnership() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher acquire(disconnectClient_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(acquire.isFinished(),7000);
        const QDBusPendingReply<QString> acquireReply=acquire;
        QVERIFY2(!acquireReply.isError(),qPrintable(acquireReply.error().message()));
        QDBusConnection::disconnectFromBus("n14-disconnect-client");
        disconnectClient_=QDBusConnection("n14-disconnect-client-closed");
        QTRY_VERIFY_WITH_TIMEOUT(!ecTransport_->isOpen(),5000);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),3000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),5000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY2(!provenanceReply.isError(),qPrintable(provenanceReply.error().message()));
        QCOMPARE(authority.ecChecks,2);
        unregisterAuthority();
    }
    void anotherCallerCannotReadOrApplyOwnersProvenance() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher owner(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(owner.isFinished(),7000);
        const QDBusPendingReply<QString> ownerReply=owner;
        QVERIFY2(!ownerReply.isError(),qPrintable(ownerReply.error().message()));
        auto provenance=commandProvenance(currentProvenance(ownerReply.value()));
        provenance["calculation_epoch"]=1;
        auto other=QDBusConnection::connectToBus(QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS")),"n14-other-client");
        QVERIFY(other.isConnected());
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher observe(other.asyncCall(message("GetEcProvenance"),3000));
        QDBusPendingCallWatcher apply(other.asyncCall(message("ApplyEc",{ecCommand(provenance)}),3000));
        QTRY_VERIFY_WITH_TIMEOUT(observe.isFinished() && apply.isFinished(),5000);
        const QDBusPendingReply<QString> observeReply=observe,applyReply=apply;
        QVERIFY(observeReply.isError()); QCOMPARE(observeReply.error().type(),QDBusError::LimitsExceeded);
        QVERIFY(applyReply.isError()); QCOMPARE(applyReply.error().type(),QDBusError::LimitsExceeded);
        QCOMPARE(authority.ecChecks,1); QVERIFY(ecTransport_->isOpen());
        QVERIFY(ecTransport_->events().isEmpty());
        QDBusConnection::disconnectFromBus("n14-other-client");
    }
    void sleepRevokesEcOwnership() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher acquire(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(acquire.isFinished(),7000);
        const QDBusPendingReply<QString> acquireReply=acquire;
        QVERIFY2(!acquireReply.isError(),qPrintable(acquireReply.error().message()));
        QVERIFY(ecTransport_->isOpen());
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        QVERIFY(!ecTransport_->isOpen());
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),3000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),5000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY(provenanceReply.isError()); QCOMPARE(provenanceReply.error().type(),QDBusError::AccessDenied);
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        unregisterAuthority();
    }
    void failedEcWriteRevokesAuthorityWithoutRecoveryWrite() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),7000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY2(!provenanceReply.isError(),qPrintable(provenanceReply.error().message()));
        auto provenanceJson=currentProvenance(provenanceReply.value());
        const auto commandProvenanceJson=commandProvenance(provenanceJson);
        auto commandProvenanceWithEpoch=commandProvenanceJson;
        commandProvenanceWithEpoch["calculation_epoch"]=1;
        ecTransport_->clearEvents();
        ecTransport_->failNextWrite("0x54");
        QDBusPendingCallWatcher apply(client_.asyncCall(message("ApplyEc",{ecCommand(commandProvenanceWithEpoch)}),5000));
        QTRY_VERIFY_WITH_TIMEOUT(apply.isFinished(),7000);
        const QDBusPendingReply<QString> applyReply=apply;
        QVERIFY(applyReply.isError()); QCOMPARE(applyReply.error().type(),QDBusError::Failed);
        QVERIFY(!ecTransport_->isOpen());
        QCOMPARE(std::count_if(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                              [](const auto &event) { return event.operation=="write"; }),1);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                             [](const auto &event) { return event.operation=="write" && event.registerName=="0x50" && event.value==0; }));
        ecTransport_->clearEvents();
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher fresh(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(fresh.isFinished(),7000);
        const QDBusPendingReply<QString> freshReply=fresh;
        QVERIFY2(!freshReply.isError(),qPrintable(freshReply.error().message()));
        QVERIFY(currentProvenance(freshReply.value())["broker_owner_generation"].toInteger()>
                provenanceJson["broker_owner_generation"].toInteger());
        QCOMPARE(authority.ecChecks,1);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
        unregisterAuthority();
    }
    void currentEcOwnerContinuesDuringCpuAuthorization_data() {
        QTest::addColumn<bool>("apply");
        QTest::newRow("fresh-observation")<<false;
        QTest::newRow("fresh-fan-command")<<true;
    }
    void currentEcOwnerContinuesDuringCpuAuthorization() {
        QFETCH(bool,apply);
        FakeAuthority authority; registerAuthority(authority); writeCurrentThermalSnapshot();
        const auto initial=ecCall("GetEcProvenance");
        QVERIFY2(!initial.isError(),qPrintable(initial.error().message()));
        auto p=commandProvenance(currentProvenance(initial.value())); p["calculation_epoch"]=1;
        authority.delayMilliseconds=1000;
        const QString cpu=QStringLiteral(R"({"mode":"limit","max_mhz":2300,"turbo":-1})");
        // callerDisconnectRevokesEcOwnership deliberately closes its fixture
        // client earlier in the full suite. Use a separate live CPU client.
        const auto cpuClient=QDBusConnection::connectToBus(
            QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS")),"n14-cpu-auth-client");
        QVERIFY(cpuClient.isConnected());
        QDBusPendingCallWatcher pending(cpuClient.asyncCall(message("ApplyCpu",{cpu,true}),5000));
        QTRY_COMPARE_WITH_TIMEOUT(authority.cpuChecks,1,2000);
        ecTransport_->clearEvents();
        const auto during=apply ? ecCall("ApplyEc",{ecCommand(p)}) : ecCall("GetEcProvenance");
        const bool cpuWasStillPending=!pending.isFinished();
        QDBusPendingCallWatcher outsider(cpuClient.asyncCall(message("GetEcProvenance"),2000));
        QTRY_VERIFY_WITH_TIMEOUT(outsider.isFinished(),2000);
        const QDBusPendingReply<QString> outsiderReply=outsider;
        QTRY_VERIFY_WITH_TIMEOUT(pending.isFinished(),3000);
        const QDBusPendingReply<QString> cpuReply=pending;
        // Reset only synthetic cpufreq, never real sysfs.
        write(root_.path()+"/devices/system/cpu/cpufreq/policy0/scaling_max_freq","4700000");
        QVERIFY2(!cpuReply.isError(),qPrintable(cpuReply.error().message()));
        QVERIFY2(!during.isError(),qPrintable(during.error().message()));
        QVERIFY(cpuWasStillPending); QCOMPARE(authority.ecChecks,1); QVERIFY(ecTransport_->isOpen());
        QVERIFY(outsiderReply.isError()); QCOMPARE(outsiderReply.error().type(),QDBusError::LimitsExceeded);
        QCOMPARE(std::count_if(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
            [](const auto &event) { return event.operation=="write"; }),apply ? 3 : 0);
    }
    void keylightContinuationRejectsChangedStateWithoutFurtherWrites_data() {
        QTest::addColumn<QString>("fault"); QTest::addColumn<int>("failAt");
        QTest::addColumn<int>("attempts");
        QTest::newRow("gpu-before-preload")<<QString("gpu")<<2<<0;
        QTest::newRow("gpu-before-main-write")<<QString("gpu")<<3<<2;
        QTest::newRow("gpu-before-restore")<<QString("gpu")<<4<<3;
        QTest::newRow("epoch-before-main-write")<<QString("epoch")<<2<<2;
        QTest::newRow("original-age-expires-during-check")<<QString("age")<<2<<0;
    }
    void keylightContinuationRejectsChangedStateWithoutFurtherWrites() {
        QFETCH(QString,fault); QFETCH(int,failAt); QFETCH(int,attempts);
        FakeAuthority authority; registerAuthority(authority);
        ecTransport_->seed("0x50",2); writeCurrentThermalSnapshot();
        const auto observed=ecCall("GetEcProvenance");
        QVERIFY2(!observed.isError(),qPrintable(observed.error().message()));
        auto p=commandProvenance(currentProvenance(observed.value())); p["calculation_epoch"]=1;
        int liveReads=0;
        beforeGpuLiveRead_=[&] {
            if (++liveReads!=failAt) return;
            if (fault=="gpu") gpuLive_={"active","D0"};
            if (fault=="epoch") writeCurrentThermalSnapshot("suspended",2);
            if (fault=="age") {
                QTest::qSleep(2100);
                // A newer snapshot cannot rejuvenate the ORIGINAL Apply token.
                writeCurrentThermalSnapshot();
            }
        };
        ecTransport_->clearEvents();
        const auto request=QString::fromUtf8(QJsonDocument(QJsonObject{
            {"provenance",p},{"target","keylight"},{"value",50}}).toJson(QJsonDocument::Compact));
        const auto reply=ecCall("ApplyEc",{request});
        beforeGpuLiveRead_={};
        QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::Failed);
        const auto text=reply.error().message();
        QVERIFY2(text.contains("diagnostics="),qPrintable(text));
        const auto diagnostics=QJsonDocument::fromJson(text.mid(text.indexOf("diagnostics=")+12).toUtf8()).object();
        QCOMPARE(diagnostics["stage"].toString(),QString("observation"));
        QCOMPARE(diagnostics["write_attempts"].toInt(),attempts);
        QVERIFY(!ecTransport_->isOpen());
        QCOMPARE(std::count_if(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
            [](const auto &e) { return e.operation=="write"; }),attempts);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
            [](const auto &e) { return e.operation=="write" && e.registerName=="0x50" && e.value==2; }));
        if (fault=="age") QVERIFY(diagnostics["reason"].toString().contains("ORIGINAL Apply sample expired"));
    }
    void transactionRejectionExposesCause_data() {
        QTest::addColumn<QString>("fault"); QTest::addColumn<QString>("stage");
        QTest::addColumn<int>("attempts");
        QTest::newRow("owner-mismatch")<<QString("owner")<<QString("validation")<<0;
        QTest::newRow("stale-provenance")<<QString("stale")<<QString("validation")<<0;
        QTest::newRow("first-write-failure")<<QString("write")<<QString("write")<<1;
        QTest::newRow("cpu-readback-failure")<<QString("read")<<QString("readback")<<2;
        QTest::newRow("cpu-readback-mismatch")<<QString("mismatch")<<QString("readback")<<2;
    }
    void transactionRejectionExposesCause() {
        QFETCH(QString,fault); QFETCH(QString,stage); QFETCH(int,attempts);
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher provenance(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(provenance.isFinished(),7000);
        const QDBusPendingReply<QString> provenanceReply=provenance;
        QVERIFY2(!provenanceReply.isError(),qPrintable(provenanceReply.error().message()));
        auto p=commandProvenance(currentProvenance(provenanceReply.value()));
        p["calculation_epoch"]=1;
        if (fault=="owner") p["broker_owner_generation"]=p["broker_owner_generation"].toInteger()+1;
        if (fault=="stale") QTest::qWait(2100);
        if (fault=="write") ecTransport_->failNextWrite("0x50");
        if (fault=="read") ecTransport_->failNextRead("0x54");
        if (fault=="mismatch") ecTransport_->overrideRead("0x54",0);
        ecTransport_->clearEvents();
        QDBusPendingCallWatcher apply(client_.asyncCall(message("ApplyEc",{ecCommand(p)}),5000));
        QTRY_VERIFY_WITH_TIMEOUT(apply.isFinished(),7000);
        const QDBusPendingReply<QString> reply=apply;
        QVERIFY(reply.isError()); QCOMPARE(reply.error().type(),QDBusError::Failed);
        const auto text=reply.error().message();
        QVERIFY2(text.contains("diagnostics="),qPrintable(text));
        const auto diagnostics=QJsonDocument::fromJson(text.mid(text.indexOf("diagnostics=")+12).toUtf8()).object();
        QCOMPARE(diagnostics["stage"].toString(),stage);
        QCOMPARE(diagnostics["write_attempts"].toInt(-1),attempts);
        QCOMPARE(diagnostics["hardware_state_uncertain"].toBool(),attempts>0);
        QVERIFY(!diagnostics["reason"].toString().isEmpty());
        QCOMPARE(diagnostics["sample_id"],p["sample_id"]);
        QCOMPARE(diagnostics["calculation_epoch"].toInt(),1);
        QCOMPARE(diagnostics["broker_owner_generation"],p["broker_owner_generation"]);
        if (fault=="owner") QVERIFY(diagnostics["reason"].toString().contains("owner generation mismatch"));
        if (fault=="stale") {
            QVERIFY(diagnostics["reason"].toString().contains("stale"));
            QVERIFY(diagnostics["sample_age_ms"].toInteger()>=2000);
        }
        if (fault=="mismatch") {
            QCOMPARE(diagnostics["actual_raw"].toInt(-1),0);
            QCOMPARE(diagnostics["expected_raw"].toInt(),n14::reference::fanDutyToRaw(42));
        }
        QVERIFY(!ecTransport_->isOpen());
        const int writes=std::count_if(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
            [](const auto &event) { return event.operation=="write"; });
        QCOMPARE(writes,fault=="write" ? 0 : attempts);
        // Do not reacquire: inspect only the terminal event list.
        ecTransport_->clearReadOverride("0x54");
    }
    void statusReadFailureRevokesBrokerWithoutRecoveryWrite() {
        FakeAuthority authority;
        registerAuthority(authority);
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher first(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(first.isFinished(),7000);
        const QDBusPendingReply<QString> firstReply=first;
        QVERIFY2(!firstReply.isError(),qPrintable(firstReply.error().message()));
        const auto firstGeneration=currentProvenance(firstReply.value())["broker_owner_generation"].toInteger();
        ecTransport_->clearEvents(); ecTransport_->failNextRead("0x54");
        QDBusPendingCallWatcher failed(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(failed.isFinished(),7000);
        const QDBusPendingReply<QString> failedReply=failed;
        QVERIFY(failedReply.isError()); QCOMPARE(failedReply.error().type(),QDBusError::Failed);
        QVERIFY(!ecTransport_->isOpen());
        QCOMPARE(ecTransport_->events().size(),1); // mode read, no retry or write
        writeCurrentThermalSnapshot();
        QDBusPendingCallWatcher fresh(client_.asyncCall(message("GetEcProvenance"),5000));
        QTRY_VERIFY_WITH_TIMEOUT(fresh.isFinished(),7000);
        const QDBusPendingReply<QString> freshReply=fresh;
        QVERIFY2(!freshReply.isError(),qPrintable(freshReply.error().message()));
        QVERIFY(currentProvenance(freshReply.value())["broker_owner_generation"].toInteger()>firstGeneration);
        QCOMPARE(authority.ecChecks,1);
        QVERIFY(std::none_of(ecTransport_->events().cbegin(),ecTransport_->events().cend(),
                            [](const auto &event) { return event.operation=="write"; }));
    }
    void cleanup() {
        beforeGpuLiveRead_={};
        QDBusConnection::disconnectFromBus("n14-cpu-auth-client");
        bus_.unregisterObject(n14::FanPath); bus_.unregisterService(n14::FanService);
        auto bridge=QDBusConnection("ec-stack-bridge");
        if (bridge.isConnected()) {
            bridge.unregisterObject(n14::HardwarePath); bridge.unregisterService(n14::HardwareService);
            QDBusConnection::disconnectFromBus("ec-stack-bridge");
        }
        // Each case starts without a broker or cached session authorization.
        // This private lifecycle hook cannot be called over D-Bus.
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        QVERIFY(QMetaObject::invokeMethod(service_,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        for (const auto &caller : {client_.baseService(),disconnectClient_.baseService()}) {
            if (caller.isEmpty()) continue;
            QVERIFY(QMetaObject::invokeMethod(service_,"nameOwnerChanged",Qt::DirectConnection,
                Q_ARG(QString,caller),Q_ARG(QString,caller),Q_ARG(QString,QString{})));
        }
        unregisterAuthority();
    }
    void cleanupTestCase() {
        bus_.unregisterObject("/org/n14control/Hardware1"); bus_.unregisterService("org.n14control.TestHardware");
        QDBusConnection::disconnectFromBus("n14-test-client");
        QDBusConnection::disconnectFromBus("n14-disconnect-client");
    }
};
QTEST_GUILESS_MAIN(ServiceTests)
#include "test_service.moc"