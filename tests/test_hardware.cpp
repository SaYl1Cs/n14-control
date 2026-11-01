#include "hardware.h"
#include "fan_controller.h"
#include "fan_client.h"
#include <limits>
#include "reference_ownership.h"

#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

using namespace n14;

// Contract fixture only: no EC transport, root handles or filesystem writes.
class FakeBackendService final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.n14control.Hardware1")
public:
    int protocol=2;
    int observations=0;
    int generation=1;
    bool rejectObservation=false;
    bool rejectApply=false;
    bool wrongIdentity=false;
    bool wrongRawReadback=false;
    bool pendingObservation=false,pendingApply=false;
    int observationDelayMs=0,applyDelayMs=0;
    int rejectObservationAfter=0,rejectApplyAt=0;
    int cpu=42,gpu=42,light=0,mode=2;
    double cpuTemperature=60;
    QVector<QJsonObject> commands;
public slots:
    QString Capabilities() {
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            {"protocol",1},{"ec_protocol",protocol},{"ec_writes",true},
            {"fan_control",true},{"keyboard_lighting",true},{"cpu",QJsonObject{}}}
        ).toJson(QJsonDocument::Compact));
    }
    QString GetEcProvenance() {
        ++observations;
        if (pendingObservation) { sendErrorReply(EcSamplePending,"GPU transition; no write"); return {}; }
        if (rejectObservation || (rejectObservationAfter>0 && observations>rejectObservationAfter)) {
            sendErrorReply(QDBusError::LimitsExceeded,"runtime snapshot unavailable"); return {};
        }
        const auto policy=reference::EcOwnershipPolicy::n14();
        const auto response=QString::fromUtf8(QJsonDocument(QJsonObject{
            {"identity",QJsonObject{{"vendor",policy.expectedVendor},
                {"model",wrongIdentity ? "unsupported" : policy.expectedModel},
                {"bios",policy.expectedBios},{"ec_version",policy.expectedEcVersion},
                {"controller_id",policy.expectedControllerId},{"dsdt_sha256",policy.expectedDsdtSha256},
                {"wmi_guid",policy.expectedWmiGuid}}},
            {"broker_owner_generation",generation},{"calculation_epoch",0},
            {"epoch_generation",1},{"epoch_session","session"},{"sample_id",observations},
            {"sampled_at_boottime_seconds",123.0},{"sampled_at_unix_ms",123000},
            {"sampler_generation",1},{"sampler_session","session"},{"sleep_allowed",true},
            {"cpu_c",cpuTemperature},{"gpu_state","suspended"},
            {"fan_mode",mode},{"cpu_target_raw",cpu},{"gpu_target_raw",gpu},
            {"keylight_raw",light},{"cpu_tach_raw",35}}
        ).toJson(QJsonDocument::Compact));
        if (observationDelayMs>0) {
            setDelayedReply(true);
            const auto request=message(); const auto bus=connection();
            QTimer::singleShot(observationDelayMs,this,[request,bus,response] { bus.send(request.createReply(response)); });
            return {};
        }
        return response;
    }
    QString ApplyEc(const QString &json) {
        if (pendingApply) { sendErrorReply(EcSamplePending,"GPU changed before write; no write submitted"); return {}; }
        const auto command=QJsonDocument::fromJson(json.toUtf8()).object();
        commands.append(command);
        if (rejectApply || (rejectApplyAt>0 && commands.size()==rejectApplyAt)) {
            sendErrorReply(QDBusError::Failed,"injected partial write failure"); return {};
        }
        const auto target=command["target"].toString();
        const int raw=target=="keylight" ? reference::keylightPercentToRaw(command["value"].toInt())
            : reference::fanDutyToRaw(command["value"].toInt());
        if (target=="both" || target=="cpu") cpu=raw;
        if (target=="both" || target=="gpu") gpu=raw;
        if (target=="keylight") light=raw;
        const auto response=QString::fromUtf8(QJsonDocument(QJsonObject{{"ok",true},{"raw_readback",wrongRawReadback ? raw+1 : raw}}).toJson(QJsonDocument::Compact));
        if (applyDelayMs>0) {
            setDelayedReply(true);
            const auto request=message(); const auto bus=connection();
            QTimer::singleShot(applyDelayMs,this,[request,bus,response] { bus.send(request.createReply(response)); });
            return {};
        }
        return response;
    }
};

class HardwareTests final : public QObject {
    Q_OBJECT
    QDBusConnection bus_=QDBusConnection::systemBus();
    std::unique_ptr<FakeBackendService> service_;
    bool useRemoteService() {
        // A separate connection is necessary for real delayed D-Bus replies;
        // Qt's same-connection local shortcut explicitly forbids them.
        auto peer=QDBusConnection::connectToBus(QDBusConnection::SystemBus,"lifecycle-replacement");
        bus_.unregisterObject(HardwarePath);
        return peer.isConnected() && bus_.unregisterService(HardwareService) &&
            peer.registerObject(HardwarePath,service_.get(),QDBusConnection::ExportAllSlots) &&
            peer.registerService(HardwareService);
    }
    bool sendSleep(bool sleeping) {
        auto signal=QDBusMessage::createSignal("/org/freedesktop/login1","org.freedesktop.login1.Manager","PrepareForSleep");
        signal.setArguments({sleeping}); return bus_.send(signal);
    }
    bool makeLoadSysfs(const QString &root,int temperature) {
        const QMap<QString,QByteArray> files{{"class/hwmon/hwmon0/name","coretemp\n"},
            {"class/hwmon/hwmon0/temp1_input",QByteArray::number(temperature)},
            {"class/hwmon/hwmon0/temp1_crit","100000\n"},
            {"class/power_supply/AC/type","Mains\n"},{"class/power_supply/AC/online","1\n"}};
        for (auto it=files.begin();it!=files.end();++it) {
            const QString path=root+"/"+it.key();
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(it.value())!=it.value().size()) return false;
        }
        return true;
    }
    QString loadProbeProgram() const {
        return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../load-probe/n14-hardware-load-probe");
    }
    QJsonObject restoreSource() const {
        return {{"schema",1},{"ec_protocol",2},{"initial_status",QJsonObject{
            {"fan_mode",2},{"cpu_target_raw",42},{"gpu_target_raw",42},{"keylight_raw",0}}}};
    }
private slots:
    void initTestCase() {
        // Never register the real hardware service name on the host system bus.
        QVERIFY(!qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"),qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(bus_.isConnected());
        QVERIFY(bus_.registerService("org.freedesktop.login1"));
    }
    void init() {
        service_=std::make_unique<FakeBackendService>();
        QVERIFY(bus_.registerService(n14::HardwareService));
        QVERIFY(bus_.registerObject(HardwarePath,service_.get(),QDBusConnection::ExportAllSlots));
    }
    void firstCommandUsesOnlyProvenanceAndApply() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable()); QVERIFY(!backend.ecActive());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        backend.applyEcTarget("both",40);
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(finished.first()[0].toBool());
        QTRY_COMPARE(ready.count(),1);
        QVERIFY(backend.ecActive()); QCOMPARE(service_->commands.size(),1);
        const auto command=service_->commands.first();
        QCOMPARE(command["target"].toString(),QString("both"));
        QCOMPARE(command["value"].toInt(),40);
        const auto provenance=command["provenance"].toObject();
        QCOMPARE(provenance.size(),10); QVERIFY(!provenance.contains("identity"));
        QVERIFY(!provenance.contains("cpu_c"));
        QCOMPARE(provenance["broker_owner_generation"].toInt(),1);
        QCOMPARE(provenance["calculation_epoch"].toInt(),1);
    }
    void headlessControllerRunsWithoutGuiAndTracksTemperature() {
        QTemporaryDir directory;
        const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->cpuTemperature=65;
        FanController controller(path);
        QTRY_VERIFY(!controller.Status().isEmpty() && service_->commands.size()==1);
        const int first=service_->commands.last()["value"].toInt();
        service_->cpuTemperature=85;
        QTRY_VERIFY_WITH_TIMEOUT(service_->commands.size()>=2,3000);
        QVERIFY(service_->commands.last()["value"].toInt()>first);
        const auto status=QJsonDocument::fromJson(controller.Status().toUtf8()).object();
        QVERIFY(!status["failed"].toBool()); QCOMPARE(status["interval_ms"].toInt(),1000);
    }
    void headlessStableTemperatureDoesNotRewriteEveryTick() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTest::qWait(2200);
        QCOMPARE(service_->commands.size(),1);
    }
    void headlessQueuesOnlyLatestKeylightBehindFanTransaction() {
        QVERIFY(useRemoteService());
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->applyDelayMs=200;
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        for (int percent : {20,40,73}) {
            const auto reply=QJsonDocument::fromJson(controller.SetKeylight(percent).toUtf8()).object();
            QVERIFY(!reply.contains("request_error"));
        }
        QCOMPARE(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].toInt(),73);
        QTRY_COMPARE(service_->commands.size(),2);
        QCOMPARE(service_->commands.last()["target"].toString(),QString("keylight"));
        QCOMPARE(service_->commands.last()["value"].toInt(),73);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
        QCOMPARE(service_->light,reference::keylightPercentToRaw(73));
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),2);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessQueuedKeylightIsDiscardedAfterTransactionFailure() {
        QVERIFY(useRemoteService());
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->applyDelayMs=200; service_->wrongRawReadback=true;
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QVERIFY(!QJsonDocument::fromJson(controller.SetKeylight(73).toUtf8()).object().contains("request_error"));
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),1);
        QVERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
    }
    void headlessQueuedKeylightNeverReplaysAcrossSleep() {
        QVERIFY(useRemoteService());
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->observationDelayMs=200;
        FanController controller(path);
        QTRY_COMPARE(service_->observations,1);
        QVERIFY(!QJsonDocument::fromJson(controller.SetKeylight(73).toUtf8()).object().contains("request_error"));
        QVERIFY(sendSleep(true));
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        QVERIFY(sendSleep(false)); QTest::qWait(1200);
        QVERIFY(service_->commands.isEmpty());
        QVERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
    }
    void headlessKeylightWorksInManualModeWithUnchangedFanTargets() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        controller.SetMode(true,70); QTRY_COMPARE(service_->commands.size(),2);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        const int cpu=service_->cpu,gpu=service_->gpu;
        controller.SetKeylight(0); QTRY_COMPARE(service_->commands.size(),3);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
        QCOMPARE(service_->light,175); QCOMPARE(service_->cpu,cpu); QCOMPARE(service_->gpu,gpu);
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),3);
    }
    void headlessGpuWaitCoalescesLightAndUsesFreshCurveAfterNewGeneration() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        service_->pendingObservation=true;
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["waiting_for_sample"].toBool(),2000);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        controller.SetKeylight(20); controller.SetKeylight(73);
        QCOMPARE(service_->commands.size(),1);
        const int observations=service_->observations; QTest::qWait(300);
        QCOMPARE(service_->observations,observations); // No polling/retry storm.
        service_->pendingObservation=false; ++service_->generation; service_->cpuTemperature=85;
        QTRY_COMPARE_WITH_TIMEOUT(service_->commands.size(),3,2500);
        QCOMPARE(service_->commands[1]["target"].toString(),QString("both"));
        QCOMPARE(service_->commands[2]["target"].toString(),QString("keylight"));
        QCOMPARE(service_->commands[2]["value"].toInt(),73);
        for (int i=1;i<3;++i) QCOMPARE(service_->commands[i]["provenance"].toObject()["broker_owner_generation"].toInt(),2);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessPrewriteGpuRaceDiscardsOldCurveDemand() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        service_->pendingApply=true; service_->cpuTemperature=85;
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["waiting_for_sample"].toBool(),2500);
        QCOMPARE(service_->commands.size(),1);
        service_->cpuTemperature=60; ++service_->generation; service_->pendingApply=false;
        QTRY_COMPARE_WITH_TIMEOUT(service_->commands.size(),2,2500);
        QCOMPARE(service_->commands.last()["provenance"].toObject()["broker_owner_generation"].toInt(),2);
        QVERIFY(service_->commands.last()["value"].toInt()<50);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessPrewriteDeferralResetsUncommittedStandardStateEvenInSameEpoch() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path); QTRY_COMPARE(service_->commands.size(),1);
        service_->pendingApply=true; service_->cpuTemperature=85;
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["waiting_for_sample"].toBool(),2500);
        QCOMPARE(service_->commands.size(),1);
        service_->pendingApply=false; // PM returned before producer epoch changed.
        QTRY_COMPARE_WITH_TIMEOUT(service_->commands.size(),2,2500);
        QVERIFY(service_->commands.last()["value"].toInt()>80);
        QCOMPARE(service_->commands.last()["provenance"].toObject()["broker_owner_generation"].toInt(),1);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessTypedWaitNeverConvertsGenericObservationFailureIntoRetry() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path); QTRY_COMPARE(service_->commands.size(),1);
        service_->pendingObservation=true;
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["waiting_for_sample"].toBool(),2000);
        controller.SetKeylight(73);
        service_->pendingObservation=false; service_->rejectObservation=true;
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool(),2000);
        const int observed=service_->observations; QTest::qWait(1200);
        QCOMPARE(service_->observations,observed); QCOMPARE(service_->commands.size(),1);
        QVERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["keylight_pending_percent"].isNull());
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        service_->rejectObservation=false;
        QVERIFY(!QJsonDocument::fromJson(controller.SetMode(false,0).toUtf8()).object().contains("request_error"));
        QTRY_COMPARE(service_->commands.size(),2); // Only a NEW explicit restart.
    }
    void headlessWrongRawReadbackStopsInsteadOfReportingSuccess() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->wrongRawReadback=true;
        FanController controller(path);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),1);
        QCOMPARE(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["confirmed_commands"].toInt(),0);
    }
    void duplicateHeadlessProcessRefusesBeforeTouchingHardware() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        QVERIFY(bus_.registerService(FanService));
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../fan-controller/n14-fan-controller"));
        process.setArguments({"--config",path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,3000);
        QCOMPARE(process.exitCode(),2); QCOMPARE(service_->observations,0); QVERIFY(service_->commands.isEmpty());
    }
    void headlessFailureIsTerminalUntilExplicitRequest() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->rejectApply=true;
        FanController controller(path);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        const int observations=service_->observations;
        service_->rejectApply=false;
        QTest::qWait(2200);
        QCOMPARE(service_->commands.size(),1); QCOMPARE(service_->observations,observations);
        QVERIFY(!QJsonDocument::fromJson(controller.SetMode(false,0).toUtf8()).object().contains("request_error"));
        QTRY_COMPARE(service_->commands.size(),2);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessMissingObservationNeverStartsRetryLoop() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        service_->rejectObservation=true;
        FanController controller(path);
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
        QTest::qWait(1200);
        QCOMPARE(service_->observations,1); QVERIFY(service_->commands.isEmpty());
    }
    void headlessReloadsSavedGraphInsteadOfKeepingOldProcessor() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings settings; settings.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        settings.cpu={{0,80},{90,100},{100,100}}; settings.save(path);
        QTRY_VERIFY_WITH_TIMEOUT(service_->commands.size()>=2,3000);
        QVERIFY(service_->commands.last()["value"].toInt()>=80);
    }
    void headlessInvalidHotReloadStopsWithoutDefaultCurve() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); file.write("{}"); file.close();
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool(),3000);
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),1);
    }
    void headlessCleanSleepPausesAndResumeUsesNewProvenance() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        QVERIFY(sendSleep(true));
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["sleeping"].toBool());
        const int observations=service_->observations;
        QTest::qWait(1200); QCOMPARE(service_->observations,observations);
        ++service_->generation; service_->cpuTemperature=85;
        QVERIFY(sendSleep(false));
        QTRY_VERIFY_WITH_TIMEOUT(service_->commands.size()>=2,3000);
        QCOMPARE(service_->commands.last()["provenance"].toObject()["broker_owner_generation"].toInt(),2);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void headlessCleanSleepRestoresLastConfirmedKeylight() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        const int expected=reference::keylightRawToPercent(service_->light);
        QVERIFY(sendSleep(true));
        QTRY_VERIFY(QJsonDocument::fromJson(controller.Status().toUtf8()).object()["sleeping"].toBool());
        QVERIFY(sendSleep(false));
        QTRY_COMPARE_WITH_TIMEOUT(service_->commands.size(),2,2000);
        QCOMPARE(service_->commands.last()["target"].toString(),QString("keylight"));
        QCOMPARE(service_->commands.last()["value"].toInt(),expected);
        QVERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["failed"].toBool());
    }
    void guiFacadeOnlyReadsCachedControllerState() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QVERIFY(bus_.registerService(FanService));
        QVERIFY(bus_.registerObject(FanPath,&controller,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));
        FanClient client(false);
        QTRY_VERIFY(!client.state().isEmpty());
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!QJsonDocument::fromJson(controller.Status().toUtf8()).object()["busy"].toBool());
        const int observations=service_->observations;
        client.refresh(); client.refresh(); QTest::qWait(100);
        QCOMPARE(service_->observations,observations); QCOMPARE(service_->commands.size(),1);
        client.setMode(true,70);
        QTRY_COMPARE(service_->commands.size(),2);
        QTRY_VERIFY(client.state()["manual"].toBool());
        QCOMPARE(service_->commands.last()["value"].toInt(),70);
        bus_.unregisterObject(FanPath); bus_.unregisterService(FanService);
    }
    void guiFacadeDebouncesKeylightThroughSingleController() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QVERIFY(bus_.registerService(FanService));
        QVERIFY(bus_.registerObject(FanPath,&controller,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));
        FanClient client(false); QSignalSpy notices(&client,&FanClient::notice);
        QTRY_VERIFY(!client.state().isEmpty());
        QTRY_COMPARE(service_->commands.size(),1);
        client.setKeylight(20); client.setKeylight(40); client.setKeylight(73);
        QVERIFY(client.keylightPending());
        QTRY_COMPARE(service_->commands.size(),2);
        QCOMPARE(service_->commands.last()["target"].toString(),QString("keylight"));
        QCOMPARE(service_->commands.last()["value"].toInt(),73);
        QTRY_VERIFY(!client.keylightPending());
        QCOMPARE(client.state()["status"].toObject()["keylight_raw"].toInt(),reference::keylightPercentToRaw(73));
        QTest::qWait(1200); QCOMPARE(service_->commands.size(),2); QVERIFY(notices.isEmpty());
    }
    void guiFacadeAlternatingLightValuesReachEcWithoutOneCommandLag() {
        QTemporaryDir directory; const auto path=directory.path()+"/config.json"; Settings{}.save(path);
        FanController controller(path);
        QVERIFY(bus_.registerService(FanService));
        QVERIFY(bus_.registerObject(FanPath,&controller,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));
        FanClient client(false); QSignalSpy notices(&client,&FanClient::notice);
        QTRY_COMPARE(service_->commands.size(),1);
        QTRY_VERIFY(!client.state().isEmpty() && !client.state()["busy"].toBool());
        QVector<int> applied;
        for (int percent : {0,100,0,25,75,100,0}) {
            client.setKeylight(percent);
            QTRY_VERIFY(!client.keylightPending());
            applied.clear();
            for (const auto &command : service_->commands)
                if (command["target"]=="keylight") applied.append(command["value"].toInt());
            QCOMPARE(applied.last(),percent);
            QCOMPARE(service_->light,reference::keylightPercentToRaw(percent));
            QCOMPARE(client.state()["status"].toObject()["keylight_raw"].toInt(),reference::keylightPercentToRaw(percent));
        }
        QCOMPARE(applied,QVector<int>({0,100,0,25,75,100,0})); QVERIFY(notices.isEmpty());
    }
    void statusReadyCanImmediatelyStartNextCommand() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        bool requested=false;
        connect(&backend,&HardwareBackend::ecStatusReady,this,[&] {
            if (requested) return;
            requested=true; backend.applyEcTarget("keylight",50);
        });
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.refreshEcStatus();
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(finished.first()[0].toBool()); QCOMPARE(service_->commands.size(),1);
        QCOMPARE(service_->commands.first()["target"].toString(),QString("keylight"));
    }
    void changedGenerationResetsCalculationEpochAndStandardState() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        const auto graphs=reference::defaultCoolerControlGraphs();
        const auto cpu=reference::defaultCpuStandardFunction();
        const auto gpu=reference::defaultGpuStandardFunction();
        backend.applyEcCurves(graphs,cpu,gpu);
        QTRY_COMPARE(finished.count(),1); QTRY_COMPARE(ready.count(),1);
        QVERIFY(finished.first()[0].toBool()); QCOMPARE(service_->commands.size(),1);
        ++service_->generation;
        backend.applyEcCurves(graphs,cpu,gpu);
        QTRY_COMPARE(finished.count(),2); QTRY_COMPARE(ready.count(),2);
        QVERIFY(finished.last()[0].toBool()); QCOMPARE(service_->commands.size(),2);
        const auto provenance=service_->commands.last()["provenance"].toObject();
        QCOMPARE(provenance["broker_owner_generation"].toInt(),2);
        QCOMPARE(provenance["calculation_epoch"].toInt(),1);
    }
    void partialFailureDoesNotReplayCommand() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        service_->rejectApply=true;
        backend.applyEcTarget("both",100);
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!backend.ecActive());
        QTest::qWait(100); QCOMPARE(service_->commands.size(),1);
        service_->rejectApply=false; ++service_->generation;
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        backend.refreshEcStatus();
        QTRY_COMPARE(ready.count(),1);
        QCOMPARE(service_->commands.size(),1); // observation cannot restore an old target
        backend.applyEcTarget("both",40);
        QTRY_COMPARE(finished.count(),2);
        QVERIFY(finished.last()[0].toBool()); QCOMPARE(service_->commands.size(),2);
        QCOMPARE(service_->commands.last()["value"].toInt(),40);
    }
    void serviceOwnerLossResetsStandardWithoutReplay() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        service_->generation=9;
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        const auto graphs=reference::defaultCoolerControlGraphs();
        const auto cpu=reference::defaultCpuStandardFunction();
        const auto gpu=reference::defaultGpuStandardFunction();
        backend.applyEcCurves(graphs,cpu,gpu);
        QTRY_COMPARE(finished.count(),1); QTRY_COMPARE(ready.count(),1);
        QVERIFY(finished.first()[0].toBool()); QVERIFY(backend.ecActive());
        QVERIFY(bus_.unregisterService(n14::HardwareService));
        QTRY_VERIFY(!backend.available()); QVERIFY(!backend.ecActive());
        service_->generation=1; // Broker counters are local to the service process.
        QVERIFY(bus_.registerService(n14::HardwareService));
        QTRY_VERIFY(backend.ecAvailable()); QVERIFY(!backend.ecActive());
        QTest::qWait(100); QCOMPARE(service_->commands.size(),1);
        backend.refreshEcStatus();
        QTRY_COMPARE(ready.count(),2); QVERIFY(backend.ecActive());
        QCOMPARE(service_->commands.size(),1);
        backend.applyEcCurves(graphs,cpu,gpu);
        QTRY_COMPARE(finished.count(),2); QTRY_COMPARE(ready.count(),3);
        QVERIFY(finished.last()[0].toBool()); QCOMPARE(service_->commands.size(),2);
        const auto provenance=service_->commands.last()["provenance"].toObject();
        QCOMPARE(provenance["broker_owner_generation"].toInt(),1);
        QCOMPARE(provenance["calculation_epoch"].toInt(),1);
    }
    void lifecycleProbeSurvivesNewServiceOwnerWithoutReplay() {
        service_->generation=9;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString reportPath=directory.path()+"/restart.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            "../lifecycle-probe/n14-hardware-lifecycle-probe"));
        process.setArguments({reportPath}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(reportPath+".ready") ||
                                process.state()==QProcess::NotRunning,10000);
        QVERIFY2(QFileInfo::exists(reportPath+".ready"),process.readAllStandardError().constData());
        QCOMPARE(service_->commands.size(),1);
        QCOMPARE(service_->commands.first()["target"].toString(),QString("cpu"));
        QCOMPARE(service_->commands.first()["value"].toInt(),59);
        auto replacement=QDBusConnection::connectToBus(QDBusConnection::SystemBus,"lifecycle-replacement");
        QVERIFY(replacement.isConnected());
        QVERIFY(replacement.registerObject(HardwarePath,service_.get(),QDBusConnection::ExportAllSlots));
        QVERIFY(bus_.unregisterService(n14::HardwareService));
        service_->generation=1;
        QVERIFY(replacement.registerService(n14::HardwareService));
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitStatus(),QProcess::NormalExit);
        QVERIFY2(process.exitCode()==0,process.readAllStandardError().constData());
        QFile file(reportPath); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["ok"].toBool()); QVERIFY(report["errors"].toArray().isEmpty());
        QVERIFY(report["owner_loss_observed"].toBool());
        QVERIFY(report["backend_inactive_on_owner_change"].toBool());
        QVERIFY(report["quiet_without_ec_authority"].toBool());
        QVERIFY(report["control_state_unchanged"].toBool());
        QVERIFY(report["service_owner_before"]!=report["service_owner_after"]);
        QCOMPARE(report["explicit_command_completions"].toInt(),1);
        QCOMPARE(report["automatic_retries"].toInt(),0);
        QCOMPARE(service_->commands.size(),1);
    }
    void lifecycleProbeRefusesAutomaticMode() {
        service_->mode=0;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString reportPath=directory.path()+"/automatic.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            "../lifecycle-probe/n14-hardware-lifecycle-probe"));
        process.setArguments({reportPath}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitStatus(),QProcess::NormalExit); QCOMPARE(process.exitCode(),1);
        QFile file(reportPath); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QVERIFY(report["control_state_unchanged"].toBool());
        QVERIFY(report["errors"].toArray().first().toString().contains("initial manual mode 2"));
        QVERIFY(service_->commands.isEmpty()); QVERIFY(!QFileInfo::exists(reportPath+".ready"));
    }
    void lifecycleProbeDoesNotRetryMissingSnapshot() {
        service_->rejectObservation=true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString reportPath=directory.path()+"/snapshot.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            "../lifecycle-probe/n14-hardware-lifecycle-probe"));
        process.setArguments({reportPath}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitStatus(),QProcess::NormalExit); QCOMPARE(process.exitCode(),1);
        QCOMPARE(service_->observations,1); QVERIFY(service_->commands.isEmpty());
        QFile file(reportPath); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QCOMPARE(report["automatic_retries"].toInt(),0);
        QVERIFY(!QFileInfo::exists(reportPath+".ready"));
    }
    void sleepProbeRejectsSignalsWithoutPhysicalSuspend() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path=directory.path()+"/sleep.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            "../lifecycle-probe/n14-hardware-lifecycle-probe"));
        process.setArguments({"--sleep",path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path+".ready") || process.state()==QProcess::NotRunning,10000);
        QVERIFY2(QFileInfo::exists(path+".ready"),process.readAllStandardError().constData());
        QVERIFY(sendSleep(true)); QTest::qWait(50); QVERIFY(sendSleep(false));
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),1);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QVERIFY(report["sleep_preparation_observed"].toBool());
        QVERIFY(report["resume_observed"].toBool());
        QVERIFY(report["errors"].toArray().first().toString().contains("no physical suspend interval"));
        QCOMPARE(service_->commands.size(),1);
    }
    void loadProbeRunsStandardAndRestoresThroughBackend() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/load.json";
        QVERIFY(makeLoadSysfs(sys,70000));
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../load-probe/n14-hardware-load-probe"));
        process.setArguments({"--seconds","1","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QVERIFY2(process.exitCode()==0,process.readAllStandardError().constData());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["ok"].toBool()); QVERIFY(report["load_started"].toBool());
        QVERIFY(report["load_stopped"].toBool()); QVERIFY(report["load_chunks"].toInteger()>0);
        QVERIFY(report["load_elapsed_ms"].toInteger()>=1000);
        QVERIFY(report["control_state_restored"].toBool());
        QCOMPARE(service_->cpu,42); QCOMPARE(service_->gpu,42); QCOMPARE(service_->light,0);
        QVERIFY(service_->commands.size()>=3); QCOMPARE(report["automatic_retries"].toInt(),0);
    }
    void loadProbeRefusesHotCpuBeforeAcquiringEcOrStartingLoad() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/hot.json";
        QVERIFY(makeLoadSysfs(sys,95000));
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../load-probe/n14-hardware-load-probe"));
        process.setArguments({"--seconds","1","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitCode(),1);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QVERIFY(!report["load_started"].toBool());
        QVERIFY(report["load_stopped"].toBool()); QCOMPARE(report["load_chunks"].toInteger(),0);
        QCOMPARE(service_->observations,0); QVERIFY(service_->commands.isEmpty());
    }
    void testThermalGuardRejectsBeforeAnyWrite_data() {
        QTest::addColumn<double>("temperature"); QTest::addColumn<double>("limit");
        QTest::newRow("at-limit")<<80.0<<80.0;
        QTest::newRow("above-limit")<<90.0<<80.0;
        QTest::newRow("nonpositive-limit")<<60.0<<0.0;
        QTest::newRow("nonfinite-limit")<<60.0<<std::numeric_limits<double>::quiet_NaN();
    }
    void testThermalGuardRejectsBeforeAnyWrite() {
        QFETCH(double,temperature); QFETCH(double,limit);
        service_->cpuTemperature=temperature;
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.applyEcCurves(reference::defaultCoolerControlGraphs(),reference::defaultCpuStandardFunction(),
            reference::defaultGpuStandardFunction(),limit);
        QTRY_COMPARE(finished.count(),1); QVERIFY(!finished.first()[0].toBool());
        QTest::qWait(30); QVERIFY(service_->commands.isEmpty());
    }
    void loadProbeRefusesHotEcDespiteCoolSysfs() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/hot-ec.json";
        QVERIFY(makeLoadSysfs(sys,50000)); service_->cpuTemperature=95;
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--seconds","1","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitCode(),1);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QVERIFY(!report["load_started"].toBool());
        QCOMPARE(report["confirmed_ec_commands"].toInt(),0); QVERIFY(service_->commands.isEmpty());
    }
    void loadProbeRequiresPhysicalOptInBeforeAnyDbusCall() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path=directory.path()+"/refused.json";
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--seconds","1","--max-cpu-c","90",path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),2); QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(service_->observations,0); QVERIFY(service_->commands.isEmpty());
    }
    void loadProbeStopsWithoutRestoreAfterObservationFailure() {
        service_->rejectObservationAfter=3;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/failure.json";
        QVERIFY(makeLoadSysfs(sys,70000));
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--seconds","5","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitCode(),1);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["load_started"].toBool()); QVERIFY(report["load_stopped"].toBool());
        QVERIFY(!report["restoration_attempted"].toBool()); QVERIFY(!report["control_state_restored"].toBool());
        QCOMPARE(service_->observations,4); QCOMPARE(service_->commands.size(),1);
        QCOMPARE(report["recovery_writes"].toInt(),0);
    }
    void loadProbeStopsWithoutRestoreWhenTemperatureRises() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/hot-after-start.json";
        QVERIFY(makeLoadSysfs(sys,70000));
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--seconds","5","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output+=process.readAllStandardOutput()).contains("load-probe sample:"),5000);
        QVERIFY(makeLoadSysfs(sys,95000));
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),1);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["load_started"].toBool()); QVERIFY(report["load_stopped"].toBool());
        QVERIFY(!report["restoration_attempted"].toBool()); QCOMPARE(service_->commands.size(),1);
        QVERIFY(report["errors"].toArray().first().toString().contains("temperature limit"));
    }
    void loadProbeNeverContinuesAfterFailedPlannedRestore() {
        service_->rejectApplyAt=2;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString sys=directory.path()+"/sys",path=directory.path()+"/restore-failure.json";
        QVERIFY(makeLoadSysfs(sys,70000));
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--seconds","1","--max-cpu-c","90","--test-sys-root",sys,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitCode(),1); QCOMPARE(service_->commands.size(),2);
        QCOMPARE(service_->commands.last()["target"].toString(),QString("cpu"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["load_stopped"].toBool()); QVERIFY(report["restoration_attempted"].toBool());
        QVERIFY(!report["restoration_succeeded"].toBool()); QVERIFY(!report["final_status_is_fresh"].toBool());
    }
    void explicitRestoreUsesFreshObservationAndExactlyTwoWrites() {
        service_->cpu=25; service_->gpu=25;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString source=directory.path()+"/source.json",path=directory.path()+"/restored.json";
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(QJsonDocument(restoreSource()).toJson()); input.close();
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--physical","--restore-report",source,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitCode(),0); QCOMPARE(service_->commands.size(),2);
        QCOMPARE(service_->commands[0]["target"].toString(),QString("cpu"));
        QCOMPARE(service_->commands[1]["target"].toString(),QString("gpu"));
        QCOMPARE(service_->cpu,42); QCOMPARE(service_->gpu,42); QCOMPARE(service_->mode,2); QCOMPARE(service_->light,0);
        QCOMPARE(service_->observations,5);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(report["ok"].toBool()); QVERIFY(report["control_state_restored"].toBool());
        QCOMPARE(report["explicit_command_completions"].toInt(),2);
    }
    void explicitRestoreRejectsInvalidSourceWithoutWrites_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("malformed")<<QByteArray("{");
        auto source=restoreSource(); auto initial=source["initial_status"].toObject();
        initial.remove("cpu_target_raw"); source["initial_status"]=initial;
        QTest::newRow("missing-target")<<QJsonDocument(source).toJson();
        initial["cpu_target_raw"]=42.5; source["initial_status"]=initial;
        QTest::newRow("fractional-target")<<QJsonDocument(source).toJson();
        source=restoreSource(); source["ec_protocol"]=1;
        QTest::newRow("legacy-protocol")<<QJsonDocument(source).toJson();
    }
    void explicitRestoreRejectsInvalidSourceWithoutWrites() {
        QFETCH(QByteArray,json);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString source=directory.path()+"/source.json",path=directory.path()+"/invalid.json";
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly)); input.write(json); input.close();
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--physical","--restore-report",source,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),1); QVERIFY(QFileInfo::exists(path));
        QVERIFY(service_->commands.isEmpty()); QCOMPARE(service_->observations,0);
    }
    void explicitRestoreNeverContinuesAfterPartialFailure() {
        service_->rejectApplyAt=1;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString source=directory.path()+"/source.json",path=directory.path()+"/failure.json";
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(QJsonDocument(restoreSource()).toJson()); input.close();
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--physical","--restore-report",source,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),1); QCOMPARE(service_->commands.size(),1); QCOMPARE(service_->observations,2);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool()); QCOMPARE(report["automatic_retries"].toInt(),0);
    }
    void explicitRestoreRefusesChangedFirmwareModeWithoutWrites() {
        service_->mode=0;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString source=directory.path()+"/source.json",path=directory.path()+"/mode-changed.json";
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(QJsonDocument(restoreSource()).toJson()); input.close();
        QProcess process; process.setProgram(loadProbeProgram());
        process.setArguments({"--physical","--restore-report",source,path}); process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,5000);
        QCOMPARE(process.exitCode(),1); QVERIFY(service_->commands.isEmpty()); QCOMPARE(service_->observations,1);
    }
    void unavailableSnapshotNeverSendsOrRetriesCommand() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        service_->rejectObservation=true;
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.applyEcTarget("both",40);
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(!finished.first()[0].toBool());
        QTest::qWait(100);
        QCOMPARE(service_->observations,1); QVERIFY(service_->commands.isEmpty());
    }
    void sleepRevokesBackendAndResumeDoesNotReplay() {
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),1); QVERIFY(backend.ecActive());
        QVERIFY(sendSleep(true)); QTRY_VERIFY(!backend.ecActive());
        const int observations=service_->observations;
        backend.applyEcTarget("both",100);
        QCOMPARE(finished.count(),1); QVERIFY(!finished.last()[0].toBool());
        backend.applyEcCurves(reference::defaultCoolerControlGraphs(),reference::defaultCpuStandardFunction(),reference::defaultGpuStandardFunction());
        QTest::qWait(50); QCOMPARE(service_->observations,observations); QVERIFY(service_->commands.isEmpty());
        QVERIFY(sendSleep(false)); QTest::qWait(50);
        QVERIFY(!backend.ecActive()); QCOMPARE(service_->observations,observations);
        ++service_->generation;
        backend.applyEcTarget("cpu",40); QTRY_COMPARE(finished.count(),2);
        QTRY_COMPARE(ready.count(),2); QVERIFY(finished.last()[0].toBool());
        QCOMPARE(service_->commands.size(),1);
        QCOMPARE(service_->commands.last()["provenance"].toObject()["calculation_epoch"].toInt(),1);
    }
    void sleepInvalidatesLateProvenanceWithoutSendingCommand() {
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),1);
        service_->observationDelayMs=300;
        backend.applyEcTarget("both",100); QTRY_COMPARE(service_->observations,2);
        QVERIFY(sendSleep(true)); QTRY_VERIFY(!backend.ecActive());
        QVERIFY(sendSleep(false)); QTest::qWait(50); service_->observationDelayMs=0;
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),2); QVERIFY(backend.ecActive());
        QTRY_COMPARE(finished.count(),1); QVERIFY(!finished.last()[0].toBool());
        QVERIFY(backend.ecActive()); QVERIFY(service_->commands.isEmpty());
    }
    void sleepInvalidatesLateApplyReplyWithoutAutomaticObservation() {
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        service_->applyDelayMs=300;
        backend.applyEcTarget("cpu",40); QTRY_COMPARE(service_->commands.size(),1);
        QVERIFY(sendSleep(true)); QTest::qWait(50);
        QVERIFY(sendSleep(false)); QTest::qWait(50);
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),1); QVERIFY(backend.ecActive());
        QTRY_COMPARE(finished.count(),1); QVERIFY(!finished.last()[0].toBool());
        QVERIFY(backend.ecActive()); QCOMPARE(service_->commands.size(),1);
        QCOMPARE(service_->observations,2); QTest::qWait(50); QCOMPARE(ready.count(),1);
    }
    void sleepInvalidatesAlreadyQueuedPostCommandObservation() {
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        connect(&backend,&HardwareBackend::ecFinished,this,[&backend](bool ok,const QString &) {
            if (!ok) return;
            QVERIFY(QMetaObject::invokeMethod(&backend,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
            QVERIFY(QMetaObject::invokeMethod(&backend,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        });
        backend.applyEcTarget("cpu",40); QTRY_COMPARE(finished.count(),1);
        QVERIFY(finished.last()[0].toBool()); QTest::qWait(100);
        QVERIFY(!backend.ecActive()); QCOMPARE(service_->observations,1); QCOMPARE(service_->commands.size(),1);
    }
    void replacementOwnerInvalidatesInflightReply_data() {
        QTest::addColumn<bool>("delayApply");
        QTest::newRow("provenance-before-command")<<false;
        QTest::newRow("apply-after-possible-write")<<true;
    }
    void replacementOwnerInvalidatesInflightReply() {
        QFETCH(bool,delayApply);
        service_->generation=9;
        HardwareBackend backend(false); QTRY_VERIFY(backend.ecAvailable());
        QSignalSpy ready(&backend,&HardwareBackend::ecStatusReady);
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),1);
        if (delayApply) service_->applyDelayMs=500; else service_->observationDelayMs=500;
        backend.applyEcTarget("cpu",40);
        if (delayApply) { QTRY_COMPARE(service_->commands.size(),1); }
        else { QTRY_COMPARE(service_->observations,2); }
        auto replacement=QDBusConnection::connectToBus(QDBusConnection::SystemBus,"lifecycle-replacement");
        QVERIFY(replacement.isConnected());
        QVERIFY(replacement.registerObject(HardwarePath,service_.get(),QDBusConnection::ExportAllSlots));
        QVERIFY(bus_.unregisterService(n14::HardwareService));
        QTRY_VERIFY(!backend.available()); QVERIFY(!backend.ecActive());
        service_->generation=1; service_->applyDelayMs=0; service_->observationDelayMs=0;
        QVERIFY(replacement.registerService(n14::HardwareService)); QTRY_VERIFY(backend.ecAvailable());
        backend.refreshEcStatus(); QTRY_COMPARE(ready.count(),2); QVERIFY(backend.ecActive());
        QTRY_COMPARE(finished.count(),1); QVERIFY(!finished.last()[0].toBool());
        QVERIFY(backend.ecActive()); QTest::qWait(50);
        QCOMPARE(service_->commands.size(),delayApply ? 1 : 0); QCOMPARE(service_->observations,3);
    }
    void identityMismatchNeverSendsCommand() {
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.ecAvailable());
        service_->wrongIdentity=true;
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.applyEcTarget("both",40);
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!backend.ecActive());
        QVERIFY(service_->commands.isEmpty());
    }
    void legacyServiceCannotReceiveEcRequests() {
        service_->protocol=1;
        HardwareBackend backend(false);
        QTRY_VERIFY(backend.available()); QVERIFY(!backend.ecAvailable());
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        backend.applyEcTarget("both",40);
        QCOMPARE(finished.count(),1); QVERIFY(!finished.first()[0].toBool());
        QCOMPARE(service_->observations,0); QVERIFY(service_->commands.isEmpty());
    }
    void liveHarnessRefusesAutomaticModeWithoutWriting() {
        service_->mode=0;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto reportPath=directory.path()+"/report.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../app/n14-control"));
        process.setArguments({"--hardware-test",reportPath});
        process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitStatus(),QProcess::NormalExit); QCOMPARE(process.exitCode(),1);
        QFile file(reportPath); QVERIFY2(file.open(QIODevice::ReadOnly),process.readAllStandardError().constData());
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(report["ec_protocol"].toInt(),2);
        QVERIFY(!report["ok"].toBool());
        QVERIFY(report["restored_to_initial"].toBool());
        QCOMPARE(report["initial_status"].toObject()["fan_mode"].toInt(),0);
        QVERIFY(report["errors"].toArray().first().toString().contains("initial manual fan mode 2"));
        QVERIFY(report["steps"].toArray().isEmpty()); QVERIFY(service_->commands.isEmpty());
    }
    void liveHarnessRefusesLegacyServiceBeforeObservation() {
        service_->protocol=1;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto reportPath=directory.path()+"/report.json";
        QProcess process;
        process.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../app/n14-control"));
        process.setArguments({"--hardware-test",reportPath});
        process.start();
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QCOMPARE(process.exitStatus(),QProcess::NormalExit); QCOMPARE(process.exitCode(),1);
        QFile file(reportPath); QVERIFY2(file.open(QIODevice::ReadOnly),process.readAllStandardError().constData());
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!report["ok"].toBool());
        QVERIFY(report["errors"].toArray().first().toString().contains("service is stale"));
        QCOMPARE(service_->observations,0); QVERIFY(service_->commands.isEmpty());
    }
    void cleanup() {
        bus_.unregisterObject(FanPath); bus_.unregisterService(FanService);
        auto replacement=QDBusConnection("lifecycle-replacement");
        if (replacement.isConnected()) {
            replacement.unregisterObject(HardwarePath);
            replacement.unregisterService(n14::HardwareService);
            QDBusConnection::disconnectFromBus("lifecycle-replacement");
        }
        bus_.unregisterObject(HardwarePath);
        bus_.unregisterService(n14::HardwareService);
        service_.reset();
    }
};

QTEST_GUILESS_MAIN(HardwareTests)
#include "test_hardware.moc"