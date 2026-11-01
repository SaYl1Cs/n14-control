#include "fan_controller.h"
#include <QDBusConnection>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace n14 {
FanController::FanController(QString configPath,QObject *parent)
    : QObject(parent),configPath_(std::move(configPath)) {
    loadConfiguration(); // Never guess a config or write before validation.
    connect(&backend_,&HardwareBackend::changed,this,[this] {
        if (started_ && !backend_.ecAvailable()) { fail("Hardware service unavailable; explicit curve restart required"); return; }
        if (!started_ && backend_.ecAvailable()) { started_=true; timer_.start(); tick(); }
    });
    connect(&backend_,&HardwareBackend::ecProvenanceObserved,this,[this](const QJsonObject &provenance) {
        status_=provenance;
        if (observingKeylight_) {
            if (provenance["keylight_raw"].toInt(-1)!=reference::keylightPercentToRaw(*observingKeylight_)) {
                fail("Keylight post-command readback mismatch; explicit curve restart required"); return;
            }
            observingKeylight_.reset();
        }
        publish();
    });
    connect(&backend_,&HardwareBackend::ecFinished,this,[this](bool ok,const QString &target) {
        pending_=false;
        if (!ok) { fail("EC operation rejected; no retry/recovery writes; explicitly enable curve to retry"); return; }
        if (target=="keylight") { observingKeylight_=activeKeylight_; activeKeylight_.reset(); }
        if (!target.isEmpty()) ++commands_;
        publish();
        QTimer::singleShot(0,this,&FanController::dispatchKeylight);
    });
    connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] {
        pending_=false; waitingForSample_=false; waitingReason_.clear(); dispatchKeylight(); publish();
    });
    connect(&backend_,&HardwareBackend::ecObservationDeferred,this,[this](const QString &reason) {
        if (failed_ || sleeping_) return;
        if (manual_ && pending_ && !activeKeylight_ && !waitingForSample_) {
            pending_=false; fail("Manual command not submitted during GPU transition; explicit request required"); return;
        }
        pending_=false;
        if (activeKeylight_) {
            if (!queuedKeylight_) queuedKeylight_=activeKeylight_;
            activeKeylight_.reset();
        }
        waitingForSample_=true; waitingReason_=reason;
        if (!manual_) curveDue_=true;
        publish();
    });
    connect(&backend_,&HardwareBackend::ecStatusFailed,this,[this](const QString &reason) {
        fail("EC readback observation failed: "+reason);
    });
    connect(&backend_,&HardwareBackend::notice,this,[this](const QString &message,bool error) {
        if (error) reason_=message;
        QTextStream(error ? stderr : stdout)<<"n14-fan-controller: "<<message<<Qt::endl;
    });
    auto *ownerWatcher=new QDBusServiceWatcher(HardwareService,QDBusConnection::systemBus(),
        QDBusServiceWatcher::WatchForOwnerChange,this);
    connect(ownerWatcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this] {
        if (started_) fail("Hardware service owner changed; explicit curve restart required");
    });
    QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(prepareForSleep(bool)));
    timer_.setInterval(1000); timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_,&QTimer::timeout,this,&FanController::tick);
}
void FanController::loadConfiguration() {
    if (!QFileInfo(configPath_).isFile()) throw std::runtime_error("Fan configuration file is missing");
    const auto settings=Settings::load(configPath_);
    if (settings.mix!="Max") throw std::runtime_error("Live Standard supports only Mix Max; Min/Mean are preview-only");
    auto convert=[](const Curve &curve) {
        reference::SpeedProfile profile;
        for (const auto &point : curve)
            profile.append({point.temperature,std::clamp(static_cast<int>(std::lround(point.duty)),0,100)});
        return profile;
    };
    const auto json=settings.toJson();
    const auto config=QJsonDocument(QJsonObject{{"cpu",json["cpu_curve"]},{"gpu",json["gpu_curve"]},
        {"functions",json["coolercontrol"]}}).toJson(QJsonDocument::Compact);
    if (config==coolingConfig_) return;
    graphs_={convert(settings.cpu),convert(settings.gpu)};
    settings_=settings; coolingConfig_=config;
    backend_.resetEcCurves();
}
QString FanController::Status() const {
    const auto light=queuedKeylight_ ? queuedKeylight_ : activeKeylight_ ? activeKeylight_ : observingKeylight_;
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {"schema",1},{"mode",failed_ ? "failed" : sleeping_ ? "sleeping" : waitingForSample_ ? "waiting" : manual_ ? "manual" : "curve"},
        {"failed",failed_},{"sleeping",sleeping_},{"manual",manual_},{"reason",reason_},
        {"ready",started_},{"busy",pending_ || backend_.ecBusy()},
        {"cycles",static_cast<qint64>(cycles_)},{"confirmed_commands",static_cast<qint64>(commands_)},
        {"automatic_retries",0},{"recovery_writes",0},{"interval_ms",timer_.interval()},
        {"keylight_pending_percent",light ? QJsonValue(*light) : QJsonValue(QJsonValue::Null)},
        {"waiting_for_sample",waitingForSample_},{"waiting_reason",waitingReason_},
        {"status",status_}}).toJson(QJsonDocument::Compact));
}
void FanController::publish() { emit StateChanged(Status()); }
void FanController::fail(const QString &reason) {
    if (failed_) return;
    failed_=true;
    pending_=false;
    queuedKeylight_.reset(); activeKeylight_.reset(); observingKeylight_.reset(); curveDue_=false;
    waitingForSample_=false; waitingReason_.clear();
    if (reason_.isEmpty()) reason_=reason;
    QTextStream(stderr)<<"N14_FAN_CONTROL_STOPPED: "<<reason_<<Qt::endl;
    publish();
}
QString FanController::reject(const QString &reason) const {
    auto status=QJsonDocument::fromJson(Status().toUtf8()).object();
    status["request_error"]=reason;
    return QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact));
}
void FanController::tick() {
    if (failed_ || sleeping_) return;
    if (waitingForSample_) {
        if (!pending_ && !backend_.ecBusy() && backend_.ecAvailable()) { pending_=true; backend_.refreshEcStatus(); }
        return;
    }
    if (manual_) return;
    curveDue_=true;
    if (pending_ || backend_.ecBusy() || !backend_.ecAvailable()) return;
    try { loadConfiguration(); }
    catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
    curveDue_=false; pending_=true; ++cycles_;
    backend_.applyEcCurves(graphs_,settings_.coolerCpuFunction,settings_.coolerGpuFunction);
}
QString FanController::SetMode(bool manual,int duty) {
    if (duty<0 || duty>100) return reject("Fan duty must be an integer 0..100");
    if (sleeping_ || pending_ || backend_.ecBusy() || queuedKeylight_ || observingKeylight_ || !backend_.ecAvailable()) return reject("Fan controller is sleeping, busy or unavailable");
    try { loadConfiguration(); }
    catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return reject(QString::fromUtf8(error.what())); }
    manual_=manual; failed_=false; reason_.clear(); curveDue_=false; waitingForSample_=false; waitingReason_.clear();
    backend_.resetEcCurves();
    if (manual_) { pending_=true; backend_.applyEcTarget("both",duty); }
    else tick(); // Explicit request with fresh provenance, not cached replay.
    publish(); return Status();
}
QString FanController::SetKeylight(int percent) {
    if (percent<0 || percent>100) return reject("Keylight must be an integer 0..100");
    if (failed_ || sleeping_ || !backend_.ecAvailable()) return reject("Fan controller is failed, sleeping or unavailable");
    // One replaceable user intent, not a retry queue. Obtain NEW provenance
    // only when the existing operation and its readback have finished.
    queuedKeylight_=percent; dispatchKeylight(); publish(); return Status();
}
void FanController::dispatchKeylight() {
    if (failed_ || sleeping_ || waitingForSample_ || pending_ || backend_.ecBusy() || observingKeylight_ || !backend_.ecAvailable()) return;
    // Continuous slider input must not starve the ordinary one-second curve.
    if (curveDue_ && !manual_) { tick(); return; }
    if (!queuedKeylight_) return;
    activeKeylight_=queuedKeylight_; queuedKeylight_.reset(); pending_=true;
    backend_.applyEcTarget("keylight",*activeKeylight_);
}
void FanController::prepareForSleep(bool sleeping) {
    sleeping_=sleeping;
    if (sleeping) {
        // Save only the last confirmed firmware readback. A queued or
        // in-flight user intent is deliberately discarded and is never
        // replayed after sleep.
        sleepKeylight_.reset();
        if (status_["keylight_raw"].isDouble()) {
            try { sleepKeylight_=reference::keylightRawToPercent(status_["keylight_raw"].toInt()); }
            catch (const std::exception &) { sleepKeylight_.reset(); }
        }
        queuedKeylight_.reset(); curveDue_=false; waitingForSample_=false; waitingReason_.clear();
    }
    if (sleeping && pending_) fail("Sleep interrupted an EC operation; explicit curve restart required");
    // Backend invalidates old authority. After a clean resume the next ordinary
    // tick calculates a NEW demand. The confirmed pre-sleep keylight is a
    // separate restore intent and is submitted through fresh provenance only
    // after both sleep listeners have processed the resume signal.
    if (!sleeping && sleepKeylight_) {
        queuedKeylight_=sleepKeylight_;
        sleepKeylight_.reset();
        QTimer::singleShot(0,this,&FanController::dispatchKeylight);
    }
    publish();
}
}