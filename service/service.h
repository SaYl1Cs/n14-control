#pragma once
#include "cpu.h"
#include "reference_broker.h"
#include "reference_runtime.h"
#include "reference_power.h"
#include <QObject>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QProcess>
#include <QTimer>
#include <functional>

class HardwareService : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.n14control.Hardware1")
public:
    struct Options {
        n14::reference::EcReadOnlyProbePaths ecPaths;
        n14::reference::EcOwnershipPolicy ecPolicy=n14::reference::EcOwnershipPolicy::n14();
        n14::reference::EcTransactionPolicy ecTransactionPolicy;
        n14::reference::RuntimeSnapshotFilePolicy snapshotPolicy;
        std::shared_ptr<const n14::reference::EcReadOnlyFileSystem> ecFileSystem;
        std::shared_ptr<const n14::reference::EcReadOnlyProcessSource> ecProcessSource;
        std::shared_ptr<const n14::reference::EcReadOnlyIdentitySource> ecIdentitySource;
        std::function<std::unique_ptr<n14::reference::EcRegisterTransport>()> ecTransportFactory;
        std::function<std::optional<QPair<QString,QString>>()> ecLiveGpuPowerState;
    };

    explicit HardwareService(n14::CpuController cpu=n14::CpuController(),
                             QObject *parent=nullptr);
    HardwareService(n14::CpuController cpu,QObject *parent,Options options);
public slots:
    QString Capabilities();
    QString ApplyCpu(const QString &json,bool interactive);
    QString GetEcProvenance();
    QString ApplyEc(const QString &json);
private slots:
    void prepareForSleep(bool sleeping);
    void nameOwnerChanged(const QString &name,const QString &oldOwner,const QString &newOwner);
private:
    enum class PendingAction { None, Cpu, EcProvenance, EcApply };
    struct EcReply {
        bool ok=false;
        QString value;
        QDBusError::ErrorType error=QDBusError::InvalidArgs;
        QString message;
        QString errorName;
    };
    void finishAuthorization(int code,QProcess::ExitStatus status);
    bool startEcBroker(const QString &caller,QString *reason);
    bool ensureEcBroker(const QString &caller,QString *reason);
    bool existingEcOwnerDuringCpuAuthorization(const QString &caller) const;
    bool beginEcAuthorization(PendingAction action,const QString &ecJson = {});
    EcReply readEcProvenance();
    EcReply deferEcObservation(const QString &reason,const n14::reference::EcTransactionClock &clock);
    EcReply applyEcCommand(const QString &json);
    void sendPendingEcReply(const EcReply &reply);
    void revokeEc();
    void fail(const QString &message);
    n14::CpuController cpu_;
    Options options_;
    QProcess authorizer_;
    QTimer timer_;
    QDBusMessage pending_;
    QDBusConnection pendingBus_=QDBusConnection::systemBus();
    n14::CpuRequest request_;
    QString pendingEcJson_;
    PendingAction pendingAction_=PendingAction::None;
    std::unique_ptr<n14::reference::EcBroker> ecBroker_;
    n14::reference::RuntimeSnapshotProvider runtimeProvider_;
    n14::reference::RuntimeThermalSampler runtimeSampler_;
    std::optional<n14::reference::ValidatedThermalSample> latestEcSample_;
    std::optional<n14::reference::ValidatedThermalSample> lastEcObservation_;
    QString ecOwnerBus_;
    // A successful Polkit decision belongs to the live D-Bus client session,
    // not to an individual fan/keylight command. A fresh request from that
    // session can start a new broker after revoke, without replaying old writes.
    QString ecAuthorizedBus_;
    bool busy_=false;
    bool sleeping_=false;
};