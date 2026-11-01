#pragma once
#include "cpu.h"
#include "reference_control.h"
#include "reference_standard.h"
#include <QObject>
#include <QDBusServiceWatcher>
#include <QTimer>
#include <functional>
#include <memory>

namespace n14 {
inline constexpr auto HardwareService = "org.n14control.Hardware1";
inline constexpr auto HardwarePath = "/org/n14control/Hardware1";
inline constexpr auto HardwareInterface = "org.n14control.Hardware1";
inline constexpr auto EcSamplePending = "org.n14control.Error.EcSamplePending";
class HardwareBackend : public QObject {
    Q_OBJECT
public:
    explicit HardwareBackend(bool demo,QObject *parent=nullptr);
    bool available() const { return available_; }
    bool ecAvailable() const { return ecAvailable_; }
    bool ecActive() const { return ecActive_; }
    bool ecBusy() const { return ecBusy_; }
    void resetEcCurves();
    bool busy() const { return busy_; }
    const QJsonObject &state() const { return state_; }
    const QJsonObject &capabilities() const { return capabilities_; }
    void refresh();
    void applyCpu(const CpuRequest &request,bool interactive);
    void applyEcTarget(const QString &target,int value);
    void applyEcCurves(const reference::CoolerControlGraphs &graphs,
                       const reference::StandardFunction &cpuFunction,
                       const reference::StandardFunction &gpuFunction,
                       std::optional<double> testStopCpuC = {});
    void refreshEcStatus();
signals:
    void changed();
    void notice(const QString &message,bool error);
    void cpuFinished(bool success);
    void ecChanged();
    void ecStatusChanged(int cpuRpm,int cpuDuty,int gpuDuty,int keylight,bool manualMode);
    void ecStatusRawChanged(int fanMode,int cpuTargetRaw,int gpuTargetRaw,
                            int keylightRaw,int cpuTachRaw);
    void ecStatusReady();
    void ecProvenanceObserved(const QJsonObject &provenance);
    void ecStatusFailed(const QString &reason);
    void ecObservationDeferred(const QString &reason);
    void ecFinished(bool success,const QString &message);
private slots:
    void prepareForSleep(bool sleeping);
private:
    void invalidateEcSession();
    void requestEcProvenance(std::function<void(const QJsonObject &)> callback,
                             std::function<void(const QString &)> failure = {});
    void publishEcStatus(const QJsonObject &provenance);
    void sendEcCommand(QJsonObject provenance,const QString &target,int value);
    bool demo_,available_=false,busy_=false,ecAvailable_=false,ecActive_=false,ecBusy_=false,sleeping_=false;
    quint64 ecSessionRevision_=0;
    quint64 ecOwnerGeneration_=0;
    quint64 ecCalculationEpoch_=0;
    std::unique_ptr<reference::StandardGraphProcessor> cpuProcessor_;
    std::unique_ptr<reference::StandardGraphProcessor> gpuProcessor_;
    QJsonObject state_;
    QJsonObject capabilities_;
    QDBusServiceWatcher watcher_;
};
} // namespace n14