#pragma once

#include "reference_control.h"
#include "reference_thermal.h"

#include <QObject>
#include <QTimer>

#include <functional>
#include <optional>

namespace n14::reference {

// A snapshot after both source validation and consumer-side ordering checks.
// It contains no command, writer handle, retry state or hardware ownership.
struct ValidatedThermalSample {
    double cpuC = 0.0;
    std::optional<double> gpuC;
    GpuState gpuState = GpuState::Unknown;
    SnapshotEpoch epoch;
    quint64 sampleId = 0;
    qint64 sampledAtUnixMs = 0;
    double sampledAtBoottimeSeconds = 0.0;

    ThermalInput controlInput() const {
        return {cpuC, gpuC, gpuState};
    }
};

// Read-only consumer-side gate for the producer snapshot contract. The caller
// supplies the current CLOCK_BOOTTIME/Unix values; this class never reads files,
// clocks, EC, D-Bus or CoolerControl itself.
class RuntimeThermalSampler {
public:
    ValidatedThermalSample accept(const QByteArray &json, qint64 nowUnixMs,
                                  double nowBoottimeSeconds);
    ValidatedThermalSample accept(const ReferenceSnapshot &snapshot, qint64 nowUnixMs,
                                  double nowBoottimeSeconds);

    void reset();
    bool hasAcceptedSample() const { return lastSampleId_.has_value(); }
    std::optional<SnapshotEpoch> epoch() const { return epoch_; }
    std::optional<quint64> sampleId() const { return lastSampleId_; }

private:
    std::optional<QString> session_;
    std::optional<quint64> generation_;
    std::optional<SnapshotEpoch> epoch_;
    std::optional<quint64> lastSampleId_;
};

struct RuntimeSnapshotFilePolicy {
    QString path = "/run/n14-control/temperatures.json";
    quint32 requiredOwnerUid = 0;
    quint32 requiredGroupGid = 0;
    bool requireOwner = true;
    bool requireGroup = true;
    bool requirePrivatePermissions = true; // no group/other access or special bits
    qsizetype maximumBytes = 16384;
};

// Read-only bridge from the producer's atomically replaced file to the sampler
// above. It never creates, truncates, renames, locks or writes the file.
class RuntimeSnapshotProvider {
public:
    explicit RuntimeSnapshotProvider(RuntimeSnapshotFilePolicy policy = {});

    QByteArray read() const;
    ValidatedThermalSample readAndAccept(RuntimeThermalSampler &sampler,
                                         qint64 nowUnixMs,
                                         double nowBoottimeSeconds) const;
    const RuntimeSnapshotFilePolicy &policy() const { return policy_; }

private:
    RuntimeSnapshotFilePolicy policy_;
};

struct RuntimeSamplerClock {
    std::function<qint64()> unixMs;
    std::function<double()> boottimeSeconds;

    static RuntimeSamplerClock system();
};

// A dry-run, read-only polling loop. It owns no hardware handle and produces
// only validated samples; a stale/missing provider never becomes a control
// command. The clock is injectable so timeout and restart behavior remain
// deterministic in tests.
class RuntimeThermalSamplerLoop : public QObject {
    Q_OBJECT
public:
    explicit RuntimeThermalSamplerLoop(RuntimeSnapshotProvider provider,
                                       int intervalMs = 1000,
                                       int staleTimeoutMs = 5000,
                                       RuntimeSamplerClock clock = RuntimeSamplerClock::system(),
                                       QObject *parent = nullptr);

    void start();
    void stop();
    bool pollOnce();
    bool isRunning() const { return running_; }
    bool hasFreshSample() const { return fresh_; }
    std::optional<ValidatedThermalSample> latestSample() const { return latest_; }
    QString lastError() const { return lastError_; }
    quint64 acceptedSampleCount() const { return acceptedSampleCount_; }

signals:
    void sampleAccepted(const n14::reference::ValidatedThermalSample &sample);
    void sampleUnavailable(const QString &message);
    void freshnessChanged(bool fresh);

private slots:
    void timerPoll();

private:
    void expireIfNeeded(double nowBoottimeSeconds);
    RuntimeSnapshotProvider provider_;
    RuntimeThermalSampler sampler_;
    RuntimeSamplerClock clock_;
    QTimer timer_;
    int intervalMs_;
    int staleTimeoutMs_;
    bool running_ = false;
    bool fresh_ = false;
    std::optional<double> lastAcceptedBoottimeSeconds_;
    std::optional<ValidatedThermalSample> latest_;
    QString lastError_;
    quint64 acceptedSampleCount_ = 0;
};

} // namespace n14::reference