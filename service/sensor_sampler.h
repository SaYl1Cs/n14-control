#pragma once

#include <QJsonObject>
#include <QPair>
#include <QString>

#include <atomic>
#include <optional>

namespace n14::sensor {

// One interruptible wait per interval, not a 50 Hz stop-flag polling loop.
bool waitForSamplerInterval(const std::atomic_bool &stop, int intervalMs, QString *reason = nullptr);

struct SamplerPaths {
    QString ecIoPath = "/sys/kernel/debug/ec/ec0/io";
    QString pciRoot = "/sys/bus/pci/devices";
    QString sysRoot = "/sys";
    QString runtimeDirectory = "/run/n14-control";
    QString snapshotPath = "/run/n14-control/temperatures.json";
    QString sensorLockPath = "/run/n14-control/sensors.lock";
    // Tests may use a private QTemporaryDir owned by the test user. The
    // production service leaves this true and additionally requires euid 0.
    bool requireRootOwnership = true;
};

class SamplerState {
public:
    SamplerState(QString session = {});

    quint64 generation() const { return generation_; }
    quint64 sampleId() const { return sampleId_; }
    const QString &session() const { return session_; }

    void begin(const QString &mode);

private:
    QString session_;
    quint64 generation_ = 0;
    quint64 sampleId_ = 0;
    QString mode_;
};

class NativeSensorSampler {
public:
    explicit NativeSensorSampler(SamplerPaths paths = {}, SamplerState state = {});

    // The clock arguments are injectable so the producer contract can be tested
    // without sleeping or touching the system clock.
    QJsonObject sample(double sampledAtBoottimeSeconds, qint64 sampledAtUnixMs);
    QJsonObject sampleNow();

    bool verifyPlatform(QString *reason = nullptr) const;
    bool writeSnapshot(const QJsonObject &snapshot, QString *reason = nullptr) const;

    // Run the root-owned read-only producer until stop becomes true.
    int run(std::atomic_bool &stop, int intervalMs = 1000, QString *reason = nullptr);

    const SamplerPaths &paths() const { return paths_; }
    const SamplerState &state() const { return state_; }

private:
    int readEcByte(int offset) const;
    QString discoverGpu() const;
    QPair<QString,QString> readGpuPowerState(const QString &device) const;
    std::optional<int> readGpuTemperature(const QString &device,
                                          double sampledAt,
                                          QString *state,
                                          QString *powerState,
                                          std::optional<double> *sampledAtOut) const;
    bool ensureRuntimeDirectory(QString *reason) const;
    int openSensorLock(QString *reason) const;

    SamplerPaths paths_;
    SamplerState state_;
};

} // namespace n14::sensor