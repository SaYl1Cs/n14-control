#include "sensor_sampler.h"
#include "reference_runtime.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <thread>
#include <csignal>
#include <pthread.h>

using namespace n14::sensor;

namespace {
void writeText(const QString &path,const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path); QVERIFY2(file.open(QIODevice::WriteOnly),qPrintable(path));
    QVERIFY(file.write(data)==data.size());
}
}

class SensorTests : public QObject {
    Q_OBJECT
private slots:
    void nativeSnapshotIsIsolatedFromLegacyProducer() {
        SamplerPaths defaults;
        QCOMPARE(defaults.snapshotPath,n14::reference::RuntimeSnapshotFilePolicy{}.path);
        QCOMPARE(defaults.runtimeDirectory,QString("/run/n14-control"));
        QCOMPARE(QFileInfo(defaults.snapshotPath).absolutePath(),defaults.runtimeDirectory);
        QCOMPARE(QFileInfo(defaults.sensorLockPath).absolutePath(),defaults.runtimeDirectory);
        QVERIFY(defaults.runtimeDirectory!="/run/colorful-n14-control");
        QTemporaryDir root; QVERIFY(root.isValid());
        SamplerPaths paths; paths.runtimeDirectory=root.path();
        paths.snapshotPath=root.filePath("native-temperatures.json"); paths.requireRootOwnership=false;
        NativeSensorSampler sampler(paths);
        const QJsonObject native{{"sampler_session","native"},{"sample_id",42}};
        QString reason; QVERIFY2(sampler.writeSnapshot(native,&reason),qPrintable(reason));
        const QString legacy=root.filePath("temperatures.json");
        for (int i=0;i<5;++i) writeText(legacy,QJsonDocument(QJsonObject{{"sampler_session","legacy"},{"sample_id",i}}).toJson());
        QFile actual(paths.snapshotPath); QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(actual.readAll()).object(),native);
        QVERIFY(QFile::remove(legacy)); actual.seek(0);
        QCOMPARE(QJsonDocument::fromJson(actual.readAll()).object(),native);
    }
    void intervalWaitHasNoStopFlagPolling() {
        std::atomic_bool stop=false;
        QString reason;
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(waitForSamplerInterval(stop,80,&reason));
        QVERIFY(elapsed.elapsed()>=75); QVERIFY(elapsed.elapsed()<2000);
        QVERIFY(reason.isEmpty());
        stop=true; elapsed.restart();
        QVERIFY(!waitForSamplerInterval(stop,1000,&reason)); QVERIFY(elapsed.elapsed()<100);
        stop=false; QVERIFY(!waitForSamplerInterval(stop,0,&reason)); QVERIFY(!reason.isEmpty());
    }
    void intervalWaitIsInterruptibleAndKeepsDeadline() {
        struct sigaction action{},previous{};
        action.sa_handler=+[](int) {};
        sigemptyset(&action.sa_mask);
        QVERIFY(::sigaction(SIGUSR1,&action,&previous)==0);
        const pthread_t waitingThread=pthread_self();
        std::atomic_bool stop=false;
        std::thread interrupter([&] {
            for (int i=0;i<5;++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                pthread_kill(waitingThread,SIGUSR1);
            }
        });
        QElapsedTimer elapsed; elapsed.start();
        const bool completed=waitForSamplerInterval(stop,80);
        const auto duration=elapsed.elapsed(); interrupter.join();
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            stop=true; pthread_kill(waitingThread,SIGUSR1);
        });
        elapsed.restart(); const bool stopped=waitForSamplerInterval(stop,1000);
        const auto stopDuration=elapsed.elapsed(); stopper.join();
        const int restored=::sigaction(SIGUSR1,&previous,nullptr);
        QVERIFY(completed); QVERIFY(duration>=75); QVERIFY(duration<2000);
        QVERIFY(!stopped); QVERIFY(stopDuration<500); QCOMPARE(restored,0);
    }
    void pairedActiveSampleCarriesOriginalMetadata() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString ec=root.path()+"/ec0/io";
        QFile ecFile(ec); QDir().mkpath(QFileInfo(ec).absolutePath());
        QVERIFY(ecFile.open(QIODevice::ReadWrite)); QVERIFY(ecFile.resize(0x59));
        QVERIFY(ecFile.seek(0x56)); QVERIFY(ecFile.putChar(62));
        QVERIFY(ecFile.seek(0x58)); QVERIFY(ecFile.putChar(71)); ecFile.close();
        const QString gpuTarget=root.path()+"/devices/0000:01:00.0";
        writeText(gpuTarget+"/vendor","0x10de\n");
        writeText(gpuTarget+"/class","0x030000\n");
        writeText(gpuTarget+"/power/runtime_status","active\n");
        writeText(gpuTarget+"/power_state","D0\n");
        const QString gpu=root.path()+"/pci/0000:01:00.0";
        QDir().mkpath(QFileInfo(gpu).absolutePath());
        QVERIFY(QFile::link(gpuTarget,gpu));
        const QString audioTarget=root.path()+"/devices/0000:01:00.1";
        writeText(audioTarget+"/vendor","0x10de\n");
        writeText(audioTarget+"/class","0x040300\n");
        const QString audio=root.path()+"/pci/0000:01:00.1";
        QVERIFY(QFile::link(audioTarget,audio));

        SamplerPaths paths; paths.ecIoPath=ec; paths.pciRoot=root.path()+"/pci";
        NativeSensorSampler sampler(paths,SamplerState("87654321-4321-4432-8234-cba987654321"));
        const auto snapshot=sampler.sample(1001.25,1800000001250LL);
        QCOMPARE(snapshot["schema_version"].toInt(),2);
        QCOMPARE(snapshot["source"].toString(),QString("ec0/io"));
        QCOMPARE(snapshot["cpu_temp"].toInt(),62000);
        QCOMPARE(snapshot["gpu_temp"].toInt(),71000);
        QCOMPARE(snapshot["gpu_state"].toString(),QString("active"));
        QCOMPARE(snapshot["gpu_power_state"].toString(),QString("D0"));
        QCOMPARE(snapshot["sampler_generation"].toInt(),1);
        QCOMPARE(snapshot["sample_id"].toInt(),1);
        QVERIFY(snapshot["cpu_sample"].isObject());
        QCOMPARE(snapshot["cpu_sample"].toObject()["power_state"].toString(),QString("active"));
        QVERIFY(snapshot["gpu_sample"].isObject());
        QVERIFY(snapshot["errors"].toArray().isEmpty());
    }

    void suspendedGpuNeverGetsCpuSubstitutionAndModeAdvances() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString ec=root.path()+"/ec0/io";
        QFile ecFile(ec); QDir().mkpath(QFileInfo(ec).absolutePath());
        QVERIFY(ecFile.open(QIODevice::ReadWrite)); QVERIFY(ecFile.resize(0x59));
        QVERIFY(ecFile.seek(0x56)); QVERIFY(ecFile.putChar(55)); ecFile.close();
        const QString gpu=root.path()+"/pci/0000:01:00.0";
        writeText(gpu+"/vendor","0x10de\n"); writeText(gpu+"/class","0x030000\n");
        writeText(gpu+"/power/runtime_status","suspended\n"); writeText(gpu+"/power_state","D3cold\n");
        SamplerPaths paths; paths.ecIoPath=ec; paths.pciRoot=root.path()+"/pci";
        NativeSensorSampler sampler(paths,SamplerState("87654321-4321-4432-8234-cba987654321"));
        const auto snapshot=sampler.sample(1001.0,1800000001000LL);
        QCOMPARE(snapshot["cpu_temp"].toInt(),55000);
        QVERIFY(snapshot["gpu_temp"].isNull());
        QVERIFY(snapshot["gpu_sample"].isNull());
        QCOMPARE(snapshot["gpu_state"].toString(),QString("suspended"));
        QCOMPARE(snapshot["gpu_power_state"].toString(),QString("D3cold"));
        QCOMPARE(snapshot["sampler_generation"].toInt(),1);
        QVERIFY(snapshot["errors"].toArray().isEmpty());

        writeText(gpu+"/power/runtime_status","active\n"); writeText(gpu+"/power_state","D0\n");
        QFile update(ec); QVERIFY(update.open(QIODevice::ReadWrite)); QVERIFY(update.seek(0x58));
        QVERIFY(update.putChar(65)); update.close();
        const auto active=sampler.sample(1002.0,1800000002000LL);
        QCOMPARE(active["sampler_generation"].toInt(),2);
        QCOMPARE(active["sample_id"].toInt(),2);
        QCOMPARE(active["gpu_temp"].toInt(),65000);
    }

    void invalidGpuObservationPublishesErrorWithoutStaleValues() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString ec=root.path()+"/ec0/io";
        QFile ecFile(ec); QDir().mkpath(QFileInfo(ec).absolutePath());
        QVERIFY(ecFile.open(QIODevice::ReadWrite)); QVERIFY(ecFile.resize(0x57));
        QVERIFY(ecFile.seek(0x56)); QVERIFY(ecFile.putChar(40)); ecFile.close();
        SamplerPaths paths; paths.ecIoPath=ec; paths.pciRoot=root.path()+"/pci";
        NativeSensorSampler sampler(paths);
        const auto snapshot=sampler.sample(1001.0,1800000001000LL);
        QCOMPARE(snapshot["cpu_temp"].toInt(),40000);
        QVERIFY(snapshot["gpu_temp"].isNull());
        QCOMPARE(snapshot["gpu_state"].toString(),QString("error"));
        QVERIFY(!snapshot["errors"].toArray().isEmpty());
        QVERIFY(snapshot["cpu_sample"].toObject()["power_state"].toString()=="error");
    }

    void snapshotPublishIsAtomicAndPrivate() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString runtime=root.path()+"/run/colorful-n14-control";
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));
        SamplerPaths paths; paths.runtimeDirectory=runtime; paths.snapshotPath=runtime+"/temperatures.json";
        paths.sensorLockPath=runtime+"/sensors.lock";
        paths.requireRootOwnership=false;
        NativeSensorSampler sampler(paths);
        const QJsonObject snapshot{{"schema_version",2},{"source","ec0/io"}};
        QString reason;
        QVERIFY2(sampler.writeSnapshot(snapshot,&reason),qPrintable(reason));
        QFile file(paths.snapshotPath); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(),snapshot);
        QVERIFY(!(QFileInfo(file).permissions() & QFileDevice::ReadOther));
    }
};

QTEST_MAIN(SensorTests)
#include "test_sensor.moc"