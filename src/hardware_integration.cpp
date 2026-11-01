#include "hardware_integration.h"

#include "hardware.h"
#include "reference_control.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

#include <utility>

namespace n14 {
namespace {

constexpr int FanSettleMilliseconds=5000;
constexpr int MaxTransientRetries=3;
constexpr int TransientRetryMilliseconds=1500;

struct Step {
    QString target;
    int value = 0;
    QString phase;
};

QString targetLabel(const Step &step) {
    return step.phase+QStringLiteral(":")+step.target+QStringLiteral(":")+QString::number(step.value);
}

QJsonObject statusJson(int mode,int cpu,int gpu,int light,int tach) {
    return QJsonObject{
        {"fan_mode",mode},
        {"cpu_target_raw",cpu},
        {"gpu_target_raw",gpu},
        {"keylight_raw",light},
        {"cpu_tach_raw",tach},
        {"cpu_rpm_nominal",tach*100},
        {"gpu_rpm","not_available_from_pinned_transport"}
    };
}

class HardwareIntegrationRunner final : public QObject {
public:
    HardwareIntegrationRunner(QCoreApplication &application,QString reportPath)
        : QObject(&application), application_(application), reportPath_(std::move(reportPath)),
          backend_(false,this) {
        // These are intentionally explicit application-level operations, not
        // direct EC calls. The test exercises the same backend as the GUI.
        steps_={
            {"both",0,"test"}, {"both",40,"test"}, {"both",100,"test"},
            {"cpu",40,"test"}, {"gpu",60,"test"}, {"both",40,"test"},
            {"keylight",0,"test"}, {"keylight",50,"test"}, {"keylight",100,"test"}
        };
        connect(&backend_,&HardwareBackend::changed,this,[this] {
            if (phase_!=Phase::WaitingCapabilities || !backend_.available()) return;
            if (backend_.capabilities()["ec_protocol"].toInt()!=2) {
                return fail("running hardware service is stale; restart n14-hardware.service before writes");
            }
            if (!backend_.ecAvailable()) return fail("EC control capability is unavailable");
            phase_=Phase::WaitingInitialStatus;
            // The first status request includes automatic service-side Polkit.
            armTimeout(75000);
            backend_.refreshEcStatus();
        });
        connect(&backend_,&HardwareBackend::ecStatusRawChanged,this,
                [this](int mode,int cpu,int gpu,int light,int tach) {
            pendingStatus_=statusJson(mode,cpu,gpu,light,tach);
        });
        connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] {
            if (statusDeferred_ || pendingStatus_.isEmpty()) return;
            statusRetryCount_=0;
            const auto status=pendingStatus_;
            pendingStatus_={};
            onStatus(status["fan_mode"].toInt(),status["cpu_target_raw"].toInt(),
                     status["gpu_target_raw"].toInt(),status["keylight_raw"].toInt(),
                     status["cpu_tach_raw"].toInt());
        });
        connect(&backend_,&HardwareBackend::ecStatusFailed,this,[this](const QString &reason) {
            if (!transientSnapshotFailure(reason) || statusRetryCount_>=MaxTransientRetries) {
                fail("EC status observation failed: "+reason);
                return;
            }
            ++statusRetryCount_;
            ++transientRetryTotal_;
            QTimer::singleShot(TransientRetryMilliseconds,this,[this] {
                backend_.refreshEcStatus();
            });
        });
        connect(&backend_,&HardwareBackend::ecFinished,this,
                [this](bool success,const QString &reason) { onOperationFinished(success,reason); });
        connect(&backend_,&HardwareBackend::notice,this,[this](const QString &message,bool error) {
            if (error) notices_.append(message);
            QTextStream(error ? stderr : stdout)<<"hardware-test: "<<message<<'\n';
        });
        timeout_.setSingleShot(true);
        connect(&timeout_,&QTimer::timeout,this,[this] { fail("hardware integration operation timed out"); });
        QTimer::singleShot(0,this,[this] {
            armTimeout(10000);
            backend_.refresh();
        });
    }

private:
    enum class Phase {
        WaitingCapabilities, WaitingInitialStatus, Applying,
        WaitingAppliedStatus, Restoring, WaitingRestoredStatus, Finished
    };

    void armTimeout(int milliseconds) {
        timeout_.start(milliseconds);
    }

    void fail(const QString &reason) {
        if (phase_==Phase::Finished) return;
        errors_.append(reason);
        // Do not leave the machine at the last test duty after a failed
        // readback or physical-response assertion. Restore through the same
        // application path. Process disconnect releases the service-side lease.
        if (!initialStatus_.isEmpty() && !restorationStarted_ &&
            phase_!=Phase::WaitingCapabilities &&
            phase_!=Phase::WaitingInitialStatus) {
            startRestore();
            return;
        }
        phase_=Phase::Finished;
        timeout_.stop();
        finishReport(false);
    }

    void onStatus(int mode,int cpu,int gpu,int light,int tach) {
        const auto status=statusJson(mode,cpu,gpu,light,tach);
        lastStatus_=status;
        if (phase_==Phase::WaitingInitialStatus) {
            initialStatus_=status;
            if (mode!=2) {
                // Disconnect releases authority, not firmware mode. There is no
                // public automatic-mode command, so this test cannot restore an
                // initially automatic machine after fan commands. Refuse writes
                // instead of reporting a handoff as successful restoration.
                fail("refusing real writes: exact restoration requires initial manual fan mode 2");
                return;
            }
            startNextStep();
            return;
        }
        if (phase_==Phase::WaitingAppliedStatus || phase_==Phase::WaitingRestoredStatus)
            validateCurrentStep(status);
    }

    void startNextStep() {
        ++stepIndex_;
        if (stepIndex_>=steps_.size()) {
            startRestore();
            return;
        }
        current_=steps_.at(stepIndex_);
        operationRetryCount_=0;
        phase_=Phase::Applying;
        armTimeout(7000);
        backend_.applyEcTarget(current_.target,current_.value);
    }

    void startRestore() {
        restorationStarted_=true;
        int cpuDuty=0,gpuDuty=0,light=0;
        try {
            cpuDuty=reference::fanRawToDuty(initialStatus_["cpu_target_raw"].toInt());
            gpuDuty=reference::fanRawToDuty(initialStatus_["gpu_target_raw"].toInt());
            light=reference::keylightRawToPercent(initialStatus_["keylight_raw"].toInt());
        } catch (const std::exception &error) {
            fail(QStringLiteral("cannot calculate restore values: ")+QString::fromUtf8(error.what()));
            return;
        }
        restoreSteps_={
            {"cpu",cpuDuty,"restore"},
            {"gpu",gpuDuty,"restore"},
            {"keylight",light,"restore"}
        };
        restoreIndex_=-1;
        startNextRestoreStep();
    }

    void startNextRestoreStep() {
        ++restoreIndex_;
        if (restoreIndex_>=restoreSteps_.size()) {
            phase_=Phase::Finished;
            timeout_.stop();
            QTimer::singleShot(250,this,[this] { finishReport(errors_.isEmpty()); });
            return;
        }
        current_=restoreSteps_.at(restoreIndex_);
        operationRetryCount_=0;
        phase_=Phase::Restoring;
        armTimeout(7000);
        backend_.applyEcTarget(current_.target,current_.value);
    }

    bool expectedRawMatches(const QJsonObject &status) const {
        try {
            if (current_.target=="keylight")
                return status["keylight_raw"].toInt()==reference::keylightPercentToRaw(current_.value);
            const int expected=reference::fanDutyToRaw(current_.value);
            if (current_.target=="cpu") return status["cpu_target_raw"].toInt()==expected;
            if (current_.target=="gpu") return status["gpu_target_raw"].toInt()==expected;
            return status["cpu_target_raw"].toInt()==expected &&
                   status["gpu_target_raw"].toInt()==expected;
        } catch (const std::exception &) {
            return false;
        }
    }

    void validateCurrentStep(const QJsonObject &status) {
        timeout_.stop();
        const bool matches=expectedRawMatches(status);
        QJsonObject result{{"step",targetLabel(current_)},{"target",current_.target},
                           {"requested_percent",current_.value},{"phase",current_.phase},
                           {"expected_raw",current_.target=="keylight"
                                ? reference::keylightPercentToRaw(current_.value)
                                : reference::fanDutyToRaw(current_.value)},
                           {"readback",status},{"raw_match",matches}};
        results_.append(result);
        if (!matches) {
            fail("raw readback mismatch at "+targetLabel(current_));
            return;
        }
        if (current_.phase=="test" && current_.target=="both" && current_.value==40)
            fan40TachRaw_=status["cpu_tach_raw"].toInt();
        if (current_.phase=="test" && current_.target=="both" && current_.value==100) {
            const int tach=status["cpu_tach_raw"].toInt();
            const bool physicalResponse=fan40TachRaw_>=0 && tach>fan40TachRaw_;
            result["physical_tach_response"]=physicalResponse;
            results_[results_.size()-1]=result;
            if (!physicalResponse)
                errors_.append("CPU tach did not increase between the 40% and 100% fan commands");
        }
        if (phase_==Phase::WaitingAppliedStatus) {
            QTimer::singleShot(250,this,[this] { startNextStep(); });
        } else {
            QTimer::singleShot(250,this,[this] { startNextRestoreStep(); });
        }
    }

    static bool transientSnapshotFailure(const QString &reason) {
        return reason.contains("snapshot",Qt::CaseInsensitive) ||
               reason.contains("thermal",Qt::CaseInsensitive) ||
               reason.contains("GPU power",Qt::CaseInsensitive);
    }

    void retryCurrentOperation() {
        ++operationRetryCount_;
        ++transientRetryTotal_;
        QTimer::singleShot(TransientRetryMilliseconds,this,[this] {
            backend_.applyEcTarget(current_.target,current_.value);
        });
    }

    void onOperationFinished(bool success,const QString &reason) {
        if (phase_==Phase::Applying) {
            if (!success) {
                if (transientSnapshotFailure(reason) && operationRetryCount_<MaxTransientRetries) {
                    retryCurrentOperation(); return;
                }
                fail("application fan/keylight operation failed: "+reason); return;
            }
            operationRetryCount_=0;
            phase_=Phase::WaitingAppliedStatus;
            armTimeout(12000);
            statusDeferred_=true;
            QTimer::singleShot(FanSettleMilliseconds,this,[this] {
                statusDeferred_=false;
                backend_.refreshEcStatus();
            });
            return;
        }
        if (phase_==Phase::Restoring) {
            if (!success) {
                if (transientSnapshotFailure(reason) && operationRetryCount_<MaxTransientRetries) {
                    retryCurrentOperation(); return;
                }
                fail("restore fan/keylight operation failed: "+reason); return;
            }
            operationRetryCount_=0;
            phase_=Phase::WaitingRestoredStatus;
            armTimeout(12000);
            statusDeferred_=true;
            QTimer::singleShot(FanSettleMilliseconds,this,[this] {
                statusDeferred_=false;
                backend_.refreshEcStatus();
            });
        }
    }

    void finishReport(bool ok) {
        if (reportWritten_) return;
        reportWritten_=true;
        const bool targetsRestoredToInitial=!initialStatus_.isEmpty() &&
            lastStatus_["cpu_target_raw"]==initialStatus_["cpu_target_raw"] &&
            lastStatus_["gpu_target_raw"]==initialStatus_["gpu_target_raw"] &&
            lastStatus_["keylight_raw"]==initialStatus_["keylight_raw"];
        const bool fanModeRestored=!initialStatus_.isEmpty() &&
            lastStatus_["fan_mode"]==initialStatus_["fan_mode"];
        const bool restoredToInitial=targetsRestoredToInitial && fanModeRestored;
        const bool automaticToManualHandoff=!initialStatus_.isEmpty() &&
            initialStatus_["fan_mode"].toInt()==0 && lastStatus_["fan_mode"].toInt()==2;
        const bool controlStateRestored=restoredToInitial;
        QJsonArray reportErrors=errors_;
        if (!controlStateRestored) reportErrors.append("final EC status differs from initial status");
        QJsonObject report{
            {"schema",1}, {"test","n14-control application hardware integration"},
            {"ec_protocol",backend_.capabilities()["ec_protocol"]},
            {"ok",ok && reportErrors.isEmpty()},
            {"application",QCoreApplication::applicationFilePath()},
            {"settle_ms",FanSettleMilliseconds}, {"initial_status",initialStatus_},
            {"final_status",lastStatus_}, {"restored_to_initial",restoredToInitial},
            {"targets_restored_to_initial",targetsRestoredToInitial},
            {"fan_mode_restored",fanModeRestored},
            {"automatic_to_manual_handoff",automaticToManualHandoff},
            {"control_state_restored",controlStateRestored},
            {"steps",results_}, {"notices",notices_}, {"errors",reportErrors},
            {"rpm_semantics","cpu tachometer raw * 100 nominal; GPU RPM is not available"},
            {"transient_snapshot_retries",transientRetryTotal_}
        };
        const QByteArray json=QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (reportPath_.isEmpty()) QTextStream(stdout)<<json;
        else {
            QSaveFile file(reportPath_);
            if (!file.open(QIODevice::WriteOnly) || file.write(json)!=json.size() || !file.commit()) {
                QTextStream(stderr)<<"hardware-test: cannot write report "<<reportPath_<<'\n';
                application_.exit(1); return;
            }
            QTextStream(stdout)<<"hardware-test report: "<<reportPath_<<'\n';
        }
        application_.exit(ok && reportErrors.isEmpty() ? 0 : 1);
    }

    QCoreApplication &application_;
    QString reportPath_;
    HardwareBackend backend_;
    QVector<Step> steps_,restoreSteps_;
    QJsonArray results_,notices_,errors_;
    QJsonObject initialStatus_,lastStatus_,pendingStatus_;
    Step current_;
    QTimer timeout_;
    Phase phase_=Phase::WaitingCapabilities;
    int stepIndex_=-1;
    int restoreIndex_=-1;
    int fan40TachRaw_=-1;
    int statusRetryCount_=0;
    int operationRetryCount_=0;
    int transientRetryTotal_=0;
    bool restorationStarted_=false;
    bool statusDeferred_=false;
    bool reportWritten_=false;
};

} // namespace

int runHardwareIntegrationTest(QCoreApplication &application,const QString &reportPath) {
    HardwareIntegrationRunner runner(application,reportPath);
    return application.exec();
}

} // namespace n14