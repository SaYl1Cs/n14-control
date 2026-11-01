#include "hardware.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

#include <exception>
#include <stdexcept>
#include <utility>
#include <time.h>

using namespace n14;

namespace {

bool saveJson(const QString &path,const QJsonObject &object) {
    QSaveFile file(path);
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();
}

QJsonObject controlState(const QJsonObject &status) {
    QJsonObject result;
    for (const auto &key : {"fan_mode","cpu_target_raw","gpu_target_raw","keylight_raw"})
        result[key]=status[key];
    return result;
}
double suspendOffsetSeconds() {
    timespec boot{},monotonic{};
    if (::clock_gettime(CLOCK_BOOTTIME,&boot)!=0 || ::clock_gettime(CLOCK_MONOTONIC,&monotonic)!=0)
        throw std::runtime_error("suspend clocks unavailable");
    return double(boot.tv_sec-monotonic.tv_sec)+double(boot.tv_nsec-monotonic.tv_nsec)/1e9;
}

class LifecycleProbe final : public QObject {
    Q_OBJECT
public:
    LifecycleProbe(QCoreApplication &app,QString reportPath,bool sleepTest)
        : QObject(&app),app_(app),path_(std::move(reportPath)),backend_(false,this),
          watcher_(HardwareService,QDBusConnection::systemBus(),
                   QDBusServiceWatcher::WatchForOwnerChange,this),sleepTest_(sleepTest) {
        if (sleepTest_) QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1",
            "org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(prepareForSleep(bool)));
        connect(&backend_,&HardwareBackend::changed,this,[this] { capabilitiesReady(); });
        connect(&backend_,&HardwareBackend::notice,this,[this](const QString &message,bool error) {
            notices_.append(QJsonObject{{"message",message},{"error",error}});
            QTextStream(stderr)<<"lifecycle-probe: "<<message<<Qt::endl;
        });
        connect(&backend_,&HardwareBackend::ecStatusRawChanged,this,
                [this](int mode,int cpu,int gpu,int light,int tach) {
            last_={{"fan_mode",mode},{"cpu_target_raw",cpu},{"gpu_target_raw",gpu},
                   {"keylight_raw",light},{"cpu_tach_raw",tach}};
        });
        connect(&backend_,&HardwareBackend::ecStatusFailed,this,[this](const QString &reason) {
            fail("status observation failed (no retry): "+reason);
        });
        connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] { statusReady(); });
        connect(&backend_,&HardwareBackend::ecFinished,this,[this](bool ok,const QString &) {
            ++commandsFinished_;
            if (phase_!=Phase::Priming || !ok || commandsFinished_!=1) {
                fail("unexpected or unsuccessful EC command completion"); return;
            }
            phase_=Phase::PrimedObservation;
            // HardwareBackend already schedules its normal post-command status.
        });
        connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,
                [this](const QString &,const QString &oldOwner,const QString &newOwner) {
            ownerChanged(oldOwner,newOwner);
        });
        timeout_.setSingleShot(true);
        connect(&timeout_,&QTimer::timeout,this,[this] { fail("lifecycle probe timed out"); });
        timeout_.start(180000);
    }

private slots:
    void prepareForSleep(bool sleeping) {
        if (!sleepTest_ || phase_==Phase::Finished) return;
        if (sleeping) {
            if (phase_!=Phase::WaitingRestart) { fail("unexpected sleep preparation"); return; }
            try { suspendOffsetBefore_=suspendOffsetSeconds(); }
            catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
            sawSleep_=true; phase_=Phase::Sleeping;
            QTimer::singleShot(0,this,[this] {
                if (phase_==Phase::Finished) return;
                inactiveOnOwnerChange_=!backend_.ecActive();
                if (!inactiveOnOwnerChange_) fail("backend retained EC authority during sleep");
            });
        } else {
            if (phase_!=Phase::Sleeping || !sawSleep_) { fail("resume without sleep preparation"); return; }
            sawResume_=true;
            try { suspendedSeconds_=suspendOffsetSeconds()-suspendOffsetBefore_; }
            catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
            if (suspendedSeconds_<1.0) { fail("no physical suspend interval confirmed by BOOTTIME/MONOTONIC"); return; }
            afterOwner_=currentOwner();
            if (afterOwner_!=beforeOwner_) { fail("service owner changed during sleep"); return; }
            phase_=Phase::RestartCapabilities; backend_.refresh();
        }
    }
private:
    enum class Phase { Capabilities, InitialObservation, Priming, PrimedObservation,
                       WaitingRestart, WaitingRegistration, RestartCapabilities,
                       QuietAfterRestart, FinalObservation, Sleeping, Finished };

    QString currentOwner() const {
        const auto *interface=QDBusConnection::systemBus().interface();
        if (!interface) return {};
        const QDBusReply<QString> owner=interface->serviceOwner(HardwareService);
        return owner.isValid() ? owner.value() : QString{};
    }

    void capabilitiesReady() {
        if (phase_!=Phase::Capabilities && phase_!=Phase::RestartCapabilities) return;
        if (!backend_.available()) return;
        if (!backend_.ecAvailable() || backend_.capabilities()["ec_protocol"].toInt()!=2) {
            fail("EC protocol 2 is required"); return;
        }
        if (phase_==Phase::Capabilities) {
            beforeOwner_=currentOwner();
            if (beforeOwner_.isEmpty()) { fail("service owner is unavailable"); return; }
            phase_=Phase::InitialObservation;
            backend_.refreshEcStatus();
        } else {
            phase_=Phase::QuietAfterRestart;
            // No provenance or command is requested during this observation window.
            QTimer::singleShot(1500,this,[this] {
                if (phase_!=Phase::QuietAfterRestart) return;
                if (backend_.ecActive() || commandsFinished_!=1) {
                    fail("EC authority or command was automatically replayed after restart"); return;
                }
                quietWithoutAuthority_=true;
                phase_=Phase::FinalObservation;
                backend_.refreshEcStatus();
            });
        }
    }

    void statusReady() {
        if (phase_==Phase::InitialObservation) {
            initial_=last_;
            if (initial_.isEmpty() || initial_["fan_mode"].toInt()!=2) {
                fail("refusing writes: initial manual mode 2 is required"); return;
            }
            try {
                const int raw=initial_["cpu_target_raw"].toInt();
                const int duty=reference::fanRawToDuty(raw);
                if (reference::fanDutyToRaw(duty)!=raw) {
                    fail("initial CPU target cannot be represented exactly"); return;
                }
                phase_=Phase::Priming;
                backend_.applyEcTarget("cpu",duty);
            } catch (const std::exception &error) {
                fail(QString::fromUtf8(error.what()));
            }
        } else if (phase_==Phase::PrimedObservation) {
            if (!backend_.ecActive() || controlState(last_)!=controlState(initial_)) {
                fail("priming command changed control state or lost authority"); return;
            }
            phase_=Phase::WaitingRestart;
            QJsonObject ready{{"ready_for_restart",true},{"action",sleepTest_ ? "sleep" : "restart"},
                              {"client_bus_name",QDBusConnection::systemBus().baseService()},
                              {"service_owner",beforeOwner_},{"pid",QCoreApplication::applicationPid()},
                              {"initial_status",initial_}};
            if (!saveJson(path_+".ready",ready)) { fail("cannot save restart-ready marker"); return; }
            QTextStream(stdout)<<"LIFECYCLE_READY_FOR_RESTART"<<Qt::endl;
        } else if (phase_==Phase::FinalObservation) {
            if (!backend_.ecActive() || controlState(last_)!=controlState(initial_)) {
                fail("post-restart observation differs from the initial control state"); return;
            }
            finish(true);
        }
    }

    void ownerChanged(const QString &oldOwner,const QString &newOwner) {
        if (phase_==Phase::Finished) return;
        if (sleepTest_) { fail("service owner changed during sleep validation"); return; }
        events_.append(QJsonObject{{"at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                  {"old_owner",oldOwner},{"new_owner",newOwner}});
        if (phase_==Phase::WaitingRestart && oldOwner==beforeOwner_ && newOwner!=beforeOwner_) {
            sawLoss_=true;
            phase_=Phase::WaitingRegistration;
            QTimer::singleShot(0,this,[this] {
                if (phase_==Phase::Finished) return;
                inactiveOnOwnerChange_=!backend_.ecActive();
                if (!inactiveOnOwnerChange_) fail("backend retained EC authority after service-owner loss");
            });
        } else if (phase_!=Phase::WaitingRegistration) {
            fail("service owner changed outside the authorized restart phase"); return;
        }
        if (!newOwner.isEmpty()) {
            afterOwner_=newOwner;
            if (!sawLoss_ || afterOwner_==beforeOwner_) {
                fail("replacement must have a new unique D-Bus owner"); return;
            }
            phase_=Phase::RestartCapabilities;
            backend_.refresh();
        }
    }

    void fail(const QString &reason) {
        if (phase_==Phase::Finished) return;
        errors_.append(reason);
        QTextStream(stderr)<<"lifecycle-probe: "<<reason<<Qt::endl;
        // Only the current target is ever reasserted. No compensating/recovery
        // write is attempted after an unknown transport/readback failure.
        finish(false);
    }

    void finish(bool ok) {
        phase_=Phase::Finished;
        timeout_.stop();
        const bool unchanged=!initial_.isEmpty() && controlState(initial_)==controlState(last_);
        const QJsonObject report{
            {"schema",1},{"test",sleepTest_ ? "N14 physical sleep/resume with an active EC owner" : "N14 service restart with an active application-backend EC owner"},
            {"action",sleepTest_ ? "sleep" : "restart"},{"sleep_preparation_observed",sawSleep_},
            {"resume_observed",sawResume_},{"physical_suspend_seconds",suspendedSeconds_},
            {"ec_protocol",backend_.capabilities()["ec_protocol"]},
            {"ok",ok && errors_.isEmpty()},{"errors",errors_},{"notices",notices_},
            {"client_bus_name",QDBusConnection::systemBus().baseService()},
            {"service_owner_before",beforeOwner_},{"service_owner_after",afterOwner_},
            {"owner_events",events_},{"owner_loss_observed",sawLoss_},
            {"backend_inactive_on_owner_change",inactiveOnOwnerChange_},
            {"quiet_window_ms",1500},{"quiet_without_ec_authority",quietWithoutAuthority_},
            {"explicit_command_completions",commandsFinished_},
            {"initial_status",initial_},{"final_status",last_},{"control_state_unchanged",unchanged},
            {"no_automatic_command_observed_by_probe",commandsFinished_==1},
            {"dbus_audit_required",true},{"automatic_retries",0},{"recovery_writes",0}
        };
        if (!saveJson(path_,report)) {
            QTextStream(stderr)<<"lifecycle-probe: cannot save report"<<Qt::endl;
            app_.exit(1); return;
        }
        app_.exit(ok && errors_.isEmpty() ? 0 : 1);
    }

    QCoreApplication &app_;
    QString path_;
    HardwareBackend backend_;
    QDBusServiceWatcher watcher_;
    QTimer timeout_;
    Phase phase_=Phase::Capabilities;
    QJsonObject initial_,last_;
    QJsonArray errors_,notices_,events_;
    QString beforeOwner_,afterOwner_;
    int commandsFinished_=0;
    bool sawLoss_=false,inactiveOnOwnerChange_=false,quietWithoutAuthority_=false;
    bool sleepTest_=false,sawSleep_=false,sawResume_=false;
    double suspendOffsetBefore_=0,suspendedSeconds_=0;
};

} // namespace

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const bool sleepTest=app.arguments().size()==3 && app.arguments()[1]=="--sleep";
    if (app.arguments().size()!=2 && !sleepTest) {
        QTextStream(stderr)<<"Usage: n14-hardware-lifecycle-probe [--sleep] ABSOLUTE_NEW_REPORT_PATH"<<Qt::endl;
        return 2;
    }
    const QString path=app.arguments().last();
    const QFileInfo file(path);
    if (!file.isAbsolute() || !file.dir().exists() || file.exists() || QFileInfo::exists(path+".ready")) {
        QTextStream(stderr)<<"Refusing an invalid or already existing receipt path"<<Qt::endl;
        return 2;
    }
    LifecycleProbe probe(app,path,sleepTest);
    return app.exec();
}

#include "hardware_lifecycle_probe.moc"