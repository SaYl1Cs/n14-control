#include "hardware.h"
#include "telemetry.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

using namespace n14;

namespace {
class BoundedCpuLoad {
public:
    BoundedCpuLoad(unsigned workers,int dutyPercent) : workers_(workers),dutyPercent_(dutyPercent) {}
    ~BoundedCpuLoad() { stop(); }
    void start(int seconds) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        for (unsigned worker=0;worker<workers_;++worker) threads_.emplace_back([this,worker,deadline] {
            std::uint64_t value=worker+1;
            while (!stop_.load() && std::chrono::steady_clock::now()<deadline) {
                const auto workTime=std::chrono::microseconds(250*dutyPercent_);
                const auto until=std::min(deadline,std::chrono::steady_clock::now()+workTime);
                do {
                    for (int i=0;i<256;++i) { value^=value<<13; value^=value>>7; value^=value<<17; }
                } while (!stop_.load() && std::chrono::steady_clock::now()<until);
                checksum_.fetch_xor(value); chunks_.fetch_add(1);
                std::this_thread::sleep_until(std::min(deadline,until+std::chrono::microseconds(25000)-workTime));
            }
        });
    }
    void stop() {
        stop_=true;
        for (auto &thread : threads_) if (thread.joinable()) thread.join();
    }
    quint64 chunks() const { return chunks_.load(); }
private:
    unsigned workers_;
    int dutyPercent_;
    std::atomic<bool> stop_{false};
    std::atomic<quint64> chunks_{0},checksum_{0};
    std::vector<std::thread> threads_;
};

QJsonObject controlState(const QJsonObject &status) {
    QJsonObject result;
    for (const auto &key : {"fan_mode","cpu_target_raw","gpu_target_raw","keylight_raw"}) result[key]=status[key];
    return result;
}

class LoadProbe final : public QObject {
public:
    LoadProbe(QCoreApplication &app,QString path,int seconds,double limit,QString sysRoot,
              unsigned workers,int dutyPercent)
        : QObject(&app),app_(app),path_(std::move(path)),seconds_(seconds),limit_(limit),
          reader_(sysRoot),backend_(false,this),load_(workers,dutyPercent),testInputs_(sysRoot!="/sys"),
          workers_(workers),dutyPercent_(dutyPercent) {
        const QDir hwmon(sysRoot+"/class/hwmon");
        for (const auto &entry : hwmon.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
            QFile name(hwmon.filePath(entry+"/name"));
            if (!name.open(QIODevice::ReadOnly)) continue;
            const auto driver=name.readAll().trimmed();
            if (driver!="coretemp" && driver!="k10temp") continue;
            QFile bound(hwmon.filePath(entry+"/temp1_crit"));
            if (bound.open(QIODevice::ReadOnly)) {
                bool ok=false; const double value=bound.readAll().trimmed().toDouble(&ok)/1000;
                if (ok && std::isfinite(value) && value>0) critical_=value;
            }
            break;
        }
        connect(&backend_,&HardwareBackend::changed,this,[this] {
            if (phase_!=Phase::Capabilities || !backend_.available()) return;
            if (!backend_.ecAvailable() || backend_.capabilities()["ec_protocol"].toInt()!=2) { fail("EC protocol 2 required"); return; }
            if (!critical_ || limit_>=*critical_) { fail("test limit must be below a readable hardware CPU critical temperature"); return; }
            if (!checkTelemetry()) return;
            phase_=Phase::Initial; backend_.refreshEcStatus();
        });
        connect(&backend_,&HardwareBackend::ecStatusRawChanged,this,[this](int mode,int cpu,int gpu,int light,int tach) {
            last_={{"fan_mode",mode},{"cpu_target_raw",cpu},{"gpu_target_raw",gpu},{"keylight_raw",light},{"cpu_tach_raw",tach}};
        });
        connect(&backend_,&HardwareBackend::notice,this,[this](const QString &message,bool error) {
            notices_.append(QJsonObject{{"message",message},{"error",error}});
            QTextStream(stderr)<<"load-probe: "<<message<<Qt::endl;
        });
        connect(&backend_,&HardwareBackend::ecStatusFailed,this,[this](const QString &reason) { fail("observation failed; no retry: "+reason); });
        connect(&backend_,&HardwareBackend::ecFinished,this,[this](bool ok,const QString &target) {
            if (phase_!=Phase::Applying) return;
            if (!ok) { fail("Standard/restore operation failed; load stopped without recovery writes"); return; }
            if (!target.isEmpty()) {
                ++confirmedCommands_;
                if (!restoring_) controlMayDiffer_=true;
            }
            phase_=Phase::Observing;
            // A successful Apply already schedules exactly one status request.
            // Only no-demand Standard cycles require an explicit observation.
            if (target.isEmpty()) backend_.refreshEcStatus();
        });
        connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] { statusReady(); });
        connect(&backend_,&HardwareBackend::ecChanged,this,[this] {
            if (loadStarted_ && phase_!=Phase::Finished && !backend_.ecActive()) fail("EC authority lost; test load stopped");
        });
        monitor_.setInterval(250);
        connect(&monitor_,&QTimer::timeout,this,[this] { if (phase_!=Phase::Finished) checkTelemetry(); });
        timeout_.setSingleShot(true);
        connect(&timeout_,&QTimer::timeout,this,[this] { fail("bounded load probe timed out"); });
        timeout_.start((seconds_+100)*1000);
    }
private:
    enum class Phase { Capabilities, Initial, Applying, Observing, Waiting, Finished };
    bool checkTelemetry() {
        telemetry_=reader_.read();
        if (!telemetry_.cpuC || !std::isfinite(*telemetry_.cpuC) || *telemetry_.cpuC>=limit_) {
            fail("CPU test temperature limit reached or temperature unavailable"); return false;
        }
        if (!telemetry_.externalPower || !*telemetry_.externalPower) {
            fail("confirmed external power required for test load"); return false;
        }
        return true;
    }
    void statusReady() {
        if (phase_==Phase::Initial || phase_==Phase::Observing) statusFresh_=true;
        if (phase_==Phase::Initial) {
            initial_=last_;
            if (initial_.isEmpty() || initial_["fan_mode"].toInt()!=2) { fail("initial manual mode 2 required"); return; }
            for (const auto &key : {"cpu_target_raw","gpu_target_raw"}) {
                try {
                    const int raw=initial_[key].toInt();
                    if (reference::fanDutyToRaw(reference::fanRawToDuty(raw))!=raw) { fail("initial target is not exactly restorable"); return; }
                } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
            }
            monitor_.start(); runCycle(); return;
        }
        if (phase_!=Phase::Observing) return;
        if (!checkTelemetry()) return;
        if (restoring_) {
            const QString key=restoreIndex_==0 ? "cpu_target_raw" : "gpu_target_raw";
            if (last_[key]!=initial_[key]) { fail("restore readback mismatch"); return; }
            if (++restoreIndex_==2) {
                if (controlState(last_)!=controlState(initial_)) { fail("final control state differs from initial"); return; }
                controlMayDiffer_=false;
                finish(true); return;
            }
            restoreNext(); return;
        }
        if (!loadStarted_) {
            loadStarted_=true; elapsed_.start();
            try { load_.start(seconds_); }
            catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
        }
        samples_.append(QJsonObject{{"elapsed_ms",elapsed_.elapsed()},{"telemetry",telemetry_.toJson()},{"ec_status",last_}});
        QTextStream(stdout)<<"load-probe sample: "<<QJsonDocument(samples_.last().toObject()).toJson(QJsonDocument::Compact)<<Qt::endl;
        phase_=Phase::Waiting;
        QTimer::singleShot(1000,this,[this] { if (phase_==Phase::Waiting) runCycle(); });
    }
    void runCycle() {
        if (!checkTelemetry()) return;
        if (loadStarted_ && elapsed_.elapsed()>=seconds_*1000) {
            loadActiveMs_=elapsed_.elapsed(); load_.stop(); restoring_=true; restoreNext(); return;
        }
        phase_=Phase::Applying; statusFresh_=false;
        ++standardCycles_;
        backend_.applyEcCurves(reference::defaultCoolerControlGraphs(),reference::defaultCpuStandardFunction(),reference::defaultGpuStandardFunction(),limit_);
    }
    void restoreNext() {
        phase_=Phase::Applying; statusFresh_=false;
        const QString target=restoreIndex_==0 ? "cpu" : "gpu";
        const QString key=restoreIndex_==0 ? "cpu_target_raw" : "gpu_target_raw";
        backend_.applyEcTarget(target,reference::fanRawToDuty(initial_[key].toInt()));
    }
    void fail(const QString &reason) {
        if (phase_==Phase::Finished) return;
        // No compensation after observation/temperature/authority/transport
        // failure. A separately invoked restore is the only post-failure path.
        statusFresh_=false;
        errors_.append(reason); finish(false);
    }
    void finish(bool ok) {
        if (loadStarted_ && loadActiveMs_<0) loadActiveMs_=elapsed_.elapsed();
        phase_=Phase::Finished; monitor_.stop(); timeout_.stop(); load_.stop();
        const QJsonObject report{{"schema",1},{"test","bounded native Standard CPU-load integration"},
            {"ec_protocol",backend_.capabilities()["ec_protocol"]},{"ok",ok && errors_.isEmpty()},
            {"client_bus_name",QDBusConnection::systemBus().baseService()},{"test_inputs",testInputs_},
            {"errors",errors_},{"notices",notices_},{"samples",samples_},
            {"requested_seconds",seconds_},{"load_elapsed_ms",loadStarted_ ? loadActiveMs_ : 0},
            {"test_stop_cpu_c",limit_},{"hardware_critical_cpu_c",critical_ ? QJsonValue(*critical_) : QJsonValue()},
            {"workers",static_cast<int>(workers_)},{"worker_duty_percent",dutyPercent_},{"load_chunks",static_cast<qint64>(load_.chunks())},
            {"load_stopped",true},{"load_started",loadStarted_},{"initial_status",initial_},{"final_status",last_},
            {"standard_cycles",standardCycles_},{"confirmed_ec_commands",confirmedCommands_},{"planned_restore_commands",restoreIndex_},
            {"final_status_is_fresh",statusFresh_},{"worker_deadline_seconds",seconds_},
            {"control_state_restored",statusFresh_ && !initial_.isEmpty() && controlState(initial_)==controlState(last_)},
            {"restoration_attempted",restoring_},{"restoration_succeeded",statusFresh_ && !controlMayDiffer_ &&
                !initial_.isEmpty() && controlState(initial_)==controlState(last_)},
            {"automatic_retries",0},{"recovery_writes",0},{"gpu_load",false}};
        QSaveFile file(path_); const auto json=QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(json)!=json.size() || !file.commit()) {
            QTextStream(stderr)<<"load-probe: cannot write report"<<Qt::endl; app_.exit(1); return;
        }
        app_.exit(ok && errors_.isEmpty() ? 0 : 1);
    }
    QCoreApplication &app_;
    QString path_;
    int seconds_;
    double limit_;
    std::optional<double> critical_;
    TelemetryReader reader_;
    Snapshot telemetry_;
    HardwareBackend backend_;
    BoundedCpuLoad load_;
    QTimer monitor_,timeout_;
    QElapsedTimer elapsed_;
    QJsonObject initial_,last_;
    QJsonArray samples_,errors_,notices_;
    Phase phase_=Phase::Capabilities;
    bool loadStarted_=false,restoring_=false;
    bool statusFresh_=false,testInputs_=false,controlMayDiffer_=false;
    qint64 loadActiveMs_=-1;
    int restoreIndex_=0;
    int standardCycles_=0,confirmedCommands_=0;
    unsigned workers_=2;
    int dutyPercent_=50;
};

class RestoreProbe final : public QObject {
public:
    RestoreProbe(QCoreApplication &app,QString sourcePath,QString reportPath)
        : QObject(&app),app_(app),sourcePath_(std::move(sourcePath)),reportPath_(std::move(reportPath)),backend_(false,this) {
        connect(&backend_,&HardwareBackend::changed,this,[this] {
            if (phase_!=Phase::Capabilities || !backend_.available()) return;
            if (!backend_.ecAvailable() || backend_.capabilities()["ec_protocol"].toInt()!=2) { fail("EC protocol 2 required"); return; }
            if (!readSource()) return;
            phase_=Phase::Initial; backend_.refreshEcStatus();
        });
        connect(&backend_,&HardwareBackend::ecStatusRawChanged,this,[this](int mode,int cpu,int gpu,int light,int tach) {
            status_={{"fan_mode",mode},{"cpu_target_raw",cpu},{"gpu_target_raw",gpu},{"keylight_raw",light},{"cpu_tach_raw",tach}};
        });
        connect(&backend_,&HardwareBackend::ecStatusFailed,this,[this](const QString &reason) { fail("restore observation failed; no retry: "+reason); });
        connect(&backend_,&HardwareBackend::ecFinished,this,[this](bool ok,const QString &target) {
            if (phase_!=Phase::Applying) return;
            if (!ok) { fail("explicit restore operation failed; no retry: "+target); return; }
            ++commands_;
            phase_=Phase::FinalObservation;
            // HardwareBackend schedules one read-only post-command observation.
        });
        connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] {
            if (phase_==Phase::Initial) {
                before_=status_;
                if (before_.isEmpty() || before_["fan_mode"]!=initial_["fan_mode"] ||
                    before_["keylight_raw"]!=initial_["keylight_raw"]) {
                    fail("current mode/keylight differs from source receipt; no writes"); return;
                }
                restoreNext();
            } else if (phase_==Phase::FinalObservation) {
                after_=status_;
                const QString key=restoreIndex_==0 ? "cpu_target_raw" : "gpu_target_raw";
                if (after_[key]!=initial_[key]) { fail("explicit restore readback mismatch; no retry"); return; }
                if (restoreIndex_==0) { ++restoreIndex_; restoreNext(); return; }
                const bool restored=controlState(after_)==controlState(initial_);
                finish(restored,restored ? QString{} : "final control state differs from source receipt");
            }
        });
        timeout_.setSingleShot(true); connect(&timeout_,&QTimer::timeout,this,[this] { fail("restore probe timed out"); });
        timeout_.start(90000);
    }
private:
    enum class Phase { Capabilities, Initial, Applying, FinalObservation, Finished };
    bool readSource() {
        QFile source(sourcePath_);
        if (!source.open(QIODevice::ReadOnly) || source.size()>4*1024*1024) { fail("cannot read bounded source receipt"); return false; }
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(source.readAll(),&error);
        source_=document.object(); initial_=source_["initial_status"].toObject();
        if (error.error!=QJsonParseError::NoError || !document.isObject() ||
            source_["schema"]!=QJsonValue(1) || source_["ec_protocol"]!=QJsonValue(2) ||
            initial_["fan_mode"]!=QJsonValue(2)) {
            fail("invalid protocol-2 source receipt or initial manual mode"); return false;
        }
        try {
            for (const auto &key : {"cpu_target_raw","gpu_target_raw","keylight_raw"}) {
                const auto value=initial_[key];
                if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble()!=value.toInt(-1))
                    throw std::runtime_error("source raw target must be an integer");
                const int raw=value.toInt();
                if (QString::fromLatin1(key)=="keylight_raw") { (void)reference::keylightRawToPercent(raw); continue; }
                if (reference::fanDutyToRaw(reference::fanRawToDuty(raw))!=raw)
                    throw std::runtime_error("source target is not exactly representable");
            }
        } catch (const std::exception &exception) { fail(QString::fromUtf8(exception.what())); return false; }
        return true;
    }
    void restoreNext() {
        phase_=Phase::Applying;
        const QString key=restoreIndex_==0 ? "cpu_target_raw" : "gpu_target_raw";
        const QString target=restoreIndex_==0 ? "cpu" : "gpu";
        backend_.applyEcTarget(target,reference::fanRawToDuty(initial_[key].toInt()));
    }
    void fail(const QString &reason) { if (phase_==Phase::Finished) return; errors_.append(reason); finish(false,reason); }
    void finish(bool ok,const QString &reason) {
        if (phase_==Phase::Finished) return;
        phase_=Phase::Finished; timeout_.stop();
        const QJsonObject report{{"schema",1},{"test","explicit restoration from bounded-load receipt"},
            {"ok",ok && errors_.isEmpty()},{"errors",errors_},{"source_receipt",sourcePath_},
            {"source_initial_status",initial_},{"status_before",before_},{"status_after",after_},
            {"explicit_command_completions",commands_},{"automatic_retries",0},{"recovery_writes",0},
            {"client_bus_name",QDBusConnection::systemBus().baseService()},
            {"control_state_restored",ok && controlState(after_)==controlState(initial_)},{"reason",reason}};
        QSaveFile file(reportPath_); const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) { app_.exit(1); return; }
        app_.exit(ok && errors_.isEmpty() ? 0 : 1);
    }
    QCoreApplication &app_;
    QString sourcePath_,reportPath_;
    QJsonObject source_,initial_,before_,after_,status_;
    QJsonArray errors_;
    HardwareBackend backend_;
    QTimer timeout_;
    Phase phase_=Phase::Capabilities;
    int restoreIndex_=0,commands_=0;
};
} // namespace

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QCommandLineParser parser;
    parser.addHelpOption(); parser.addPositionalArgument("report","New absolute report path");
    parser.addOption({"seconds","Bounded load duration (1..600 seconds)","seconds"});
    parser.addOption({"max-cpu-c","Test-only workload stop temperature, below hardware critical","celsius"});
    parser.addOption({"workers","Bounded worker count (1..16)","count","2"});
    parser.addOption({"duty-percent","Per-worker duty cycle (1..100)","percent","50"});
    parser.addOption({"physical","Required for a real /sys run"});
    parser.addOption({"restore-report","Explicitly restore CPU/GPU targets from an existing receipt","path"});
    parser.addOption({"test-sys-root","Fake sysfs, permitted only with the system bus redirected to a private session bus","path","/sys"});
    parser.process(app);
    bool timeOk=false,limitOk=false;
    const int seconds=parser.value("seconds").toInt(&timeOk);
    const double limit=parser.value("max-cpu-c").toDouble(&limitOk);
    bool workersOk=false,dutyOk=false;
    const int workers=parser.value("workers").toInt(&workersOk);
    const int dutyPercent=parser.value("duty-percent").toInt(&dutyOk);
    const auto args=parser.positionalArguments(); const auto sys=parser.value("test-sys-root");
    const bool privateBus=!qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty() && qgetenv("DBUS_SYSTEM_BUS_ADDRESS")==qgetenv("DBUS_SESSION_BUS_ADDRESS");
    const auto restoreSource=parser.value("restore-report");
    if (parser.isSet("restore-report")) {
        if (args.size()!=1 || !parser.isSet("physical") || !QFileInfo(restoreSource).isAbsolute() ||
            !QFileInfo(restoreSource).isFile() || !QFileInfo(args.value(0)).isAbsolute() || QFileInfo::exists(args.value(0)) ||
            !QFileInfo(args.value(0)).dir().exists()) {
            QTextStream(stderr)<<"Invalid explicit restore parameters"<<Qt::endl; return 2;
        }
        RestoreProbe probe(app,restoreSource,args.first());
        return app.exec();
    }
    if (args.size()!=1 || !timeOk || seconds<1 || seconds>600 || !limitOk || !std::isfinite(limit) || limit<=0 || limit>100 ||
        !workersOk || workers<1 || workers>16 || !dutyOk || dutyPercent<1 || dutyPercent>100 ||
        (sys=="/sys" && !parser.isSet("physical")) || (sys!="/sys" && !privateBus) ||
        !QFileInfo(args.value(0)).isAbsolute() || QFileInfo::exists(args.value(0)) || !QFileInfo(args.value(0)).dir().exists()) {
        QTextStream(stderr)<<"Invalid parameters, fake sysfs on a real bus, or existing report"<<Qt::endl; return 2;
    }
    LoadProbe probe(app,args.first(),seconds,limit,sys,static_cast<unsigned>(workers),dutyPercent);
    return app.exec();
}