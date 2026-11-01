#pragma once
#include <QJsonObject>
#include <QString>
#include <QObject>
#include <QThread>
#include <functional>
#include <optional>

namespace n14 {
struct Snapshot {
    qint64 timestampMs = 0;
    bool batteryPresent = false;
    std::optional<bool> externalPower;
    QString batteryStatus;
    int percent = -1;
    std::optional<double> powerW;
    std::optional<double> energyWh;
    std::optional<double> cpuC;
    QString gpuRuntime;
    QString gpuPower;
    std::optional<double> gpuSuspendedSeconds;
    QString rcuLazy;
    QString gpuControl;
    QString d3coldAllowed;
    QString kernel;
    QJsonObject toJson() const;
};
class TelemetryReader {
public:
    explicit TelemetryReader(QString sysRoot = "/sys");
    Snapshot read() const;
private:
    QString sys_;
    QString gpu_;
    QString cpuTemperature_;
};

// A single lazy worker keeps potentially slow sysfs/ACPI reads off the UI
// thread. Requests are coalesced while a read or its completion is pending.
class TelemetryPoller final : public QObject {
public:
    using ReadFunction = std::function<Snapshot()>;
    using Completion = std::function<void(Snapshot,QString)>;
    explicit TelemetryPoller(QObject *parent = nullptr, ReadFunction read = {});
    ~TelemetryPoller() override;
    bool request(Completion completion);
    bool pending() const { return pending_; }
private:
    ReadFunction read_;
    QThread worker_;
    QObject *workerContext_ = nullptr;
    bool pending_ = false;
};
} // namespace n14