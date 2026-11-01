#include "hardware.h"
#include "reference_ownership.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace n14 {
namespace {

bool positiveInteger(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           value.toDouble() >= 1.0 && value.toDouble()<=9007199254740991.0 &&
           std::floor(value.toDouble()) == value.toDouble();
}

bool validEcProvenance(const QJsonObject &provenance) {
    const auto identity=provenance["identity"].toObject();
    const auto policy=reference::EcOwnershipPolicy::n14();
    if (identity["vendor"].toString()!=policy.expectedVendor ||
        identity["model"].toString()!=policy.expectedModel ||
        identity["bios"].toString()!=policy.expectedBios ||
        identity["ec_version"].toString()!=policy.expectedEcVersion ||
        identity["controller_id"].toString()!=policy.expectedControllerId ||
        identity["dsdt_sha256"].toString()!=policy.expectedDsdtSha256 ||
        identity["wmi_guid"].toString()!=policy.expectedWmiGuid) return false;
    for (const auto &key : {QStringLiteral("broker_owner_generation"),
                            QStringLiteral("epoch_generation"),QStringLiteral("sample_id"),
                            QStringLiteral("sampled_at_unix_ms"),QStringLiteral("sampler_generation")})
        if (!positiveInteger(provenance[key])) return false;
    for (const auto &key : {QStringLiteral("epoch_session"),QStringLiteral("sampler_session")})
        if (!provenance[key].isString() || provenance[key].toString().isEmpty()) return false;
    const auto boot=provenance["sampled_at_boottime_seconds"];
    if (!boot.isDouble() || !std::isfinite(boot.toDouble()) || boot.toDouble()<0.0) return false;
    if (!provenance["sleep_allowed"].isBool() || !provenance["sleep_allowed"].toBool()) return false;
    const auto state=provenance["gpu_state"].toString();
    if (state!="active" && state!="suspended") return false;
    if (state=="active" && (!provenance["gpu_c"].isDouble() ||
                              !std::isfinite(provenance["gpu_c"].toDouble()))) return false;
    return true;
}

} // namespace

HardwareBackend::HardwareBackend(bool demo,QObject *parent) : QObject(parent),demo_(demo),watcher_(this) {
    if (demo_) {
        available_=true; ecAvailable_=true;
        state_={{"supported",true},{"turbo_supported",true},{"turbo_allowed",true},
                {"policies",QJsonArray{CpuPolicy{"policy0","demo",400000,4700000,400000,4700000}.toJson()}}};
        return;
    }
    watcher_.setConnection(QDBusConnection::systemBus());
    watcher_.setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    watcher_.addWatchedService(HardwareService);
    connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{
        invalidateEcSession();
        emit ecChanged(); refresh();
    });
    QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(prepareForSleep(bool)));
    QTimer::singleShot(0,this,&HardwareBackend::refresh);
}
void HardwareBackend::invalidateEcSession() {
    ++ecSessionRevision_;
    ecActive_=false; ecBusy_=false; ecOwnerGeneration_=0; ecCalculationEpoch_=0;
    cpuProcessor_.reset(); gpuProcessor_.reset();
}
void HardwareBackend::resetEcCurves() {
    cpuProcessor_.reset(); gpuProcessor_.reset();
}
void HardwareBackend::prepareForSleep(bool sleeping) {
    sleeping_=sleeping;
    if (sleeping) invalidateEcSession();
    // Resume permits a fresh explicit request, never cached command replay.
    emit ecChanged();
}
void HardwareBackend::refresh() {
    if (demo_) { emit changed(); return; }
    const auto message=QDBusMessage::createMethodCall(HardwareService,HardwarePath,HardwareInterface,"Capabilities");
    auto *call=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,3000),this);
    const auto revision=ecSessionRevision_;
    connect(call,&QDBusPendingCallWatcher::finished,this,[this,call,revision]{
        if (revision!=ecSessionRevision_) { call->deleteLater(); return; }
        const QDBusPendingReply<QString> reply=*call;
        const bool previousEcAvailable=ecAvailable_;
        const bool previousEcActive=ecActive_;
        available_=false; ecAvailable_=false; state_={}; capabilities_={};
        if (!reply.isError()) {
            QJsonParseError error; const auto doc=QJsonDocument::fromJson(reply.value().toUtf8(),&error);
            if (error.error==QJsonParseError::NoError && doc.isObject() && doc.object()["protocol"].toInt()==1) {
                capabilities_=doc.object(); state_=capabilities_["cpu"].toObject(); available_=true;
                ecAvailable_=capabilities_["ec_protocol"].toInt()==2 &&
                    capabilities_["ec_writes"].toBool() && capabilities_["fan_control"].toBool();
            }
        }
        if (!available_ || !ecAvailable_) {
            invalidateEcSession();
        }
        call->deleteLater(); emit changed();
        if (previousEcAvailable!=ecAvailable_ || previousEcActive!=ecActive_) emit ecChanged();
    });
}
void HardwareBackend::applyCpu(const CpuRequest &input,bool interactive) {
    CpuRequest request;
    try { request=CpuRequest::fromJson(input.toJson()); }
    catch (const std::exception &e) { emit notice(QString::fromUtf8(e.what()),true); emit cpuFinished(false); return; }
    if (!request.changesHardware()) { emit cpuFinished(true); return; }
    if (!available_ || busy_) { emit notice("Аппаратный сервис недоступен или занят",true); emit cpuFinished(false); return; }
    if (demo_) {
        auto policies=state_["policies"].toArray();
        for (qsizetype i=0;i<policies.size();++i) {
            auto p=policies[i].toObject();
            if (request.mode!="keep") p["max_khz"]=request.mode=="hardware" ? p["hardware_max_khz"].toInt() : std::min(request.maxMHz*1000,p["hardware_max_khz"].toInt());
            policies[i]=p;
        }
        state_["policies"]=policies;
        if (request.turbo!=-1) state_["turbo_allowed"]=request.turbo==1;
        emit changed(); emit notice("Демо: параметры CPU применены только в симуляции",false); emit cpuFinished(true); return;
    }
    auto message=QDBusMessage::createMethodCall(HardwareService,HardwarePath,HardwareInterface,"ApplyCpu");
    message.setArguments({QString::fromUtf8(QJsonDocument(request.toJson()).toJson(QJsonDocument::Compact)),interactive});
    busy_=true;
    auto *call=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,70000),this);
    connect(call,&QDBusPendingCallWatcher::finished,this,[this,call]{
        const QDBusPendingReply<QString> reply=*call; busy_=false; bool ok=false;
        if (reply.isError()) emit notice("CPU не подтверждён: "+reply.error().message(),true);
        else {
            QJsonParseError error; const auto doc=QJsonDocument::fromJson(reply.value().toUtf8(),&error);
            ok=error.error==QJsonParseError::NoError && doc.isObject() && doc.object()["ok"].toBool() && doc.object()["cpu"].isObject();
            if (ok) { state_=doc.object()["cpu"].toObject(); emit changed(); emit notice("Параметры CPU применены и прочитаны обратно",false); }
            else emit notice("Некорректный ответ аппаратного сервиса; состояние CPU не подтверждено",true);
        }
        call->deleteLater(); emit cpuFinished(ok);
    });
}
void HardwareBackend::applyEcTarget(const QString &target,int value) {
    if (value<0 || value>100 || !QStringList{"cpu","gpu","both","keylight"}.contains(target)) {
        emit notice("Некорректная EC command",true); emit ecFinished(false,QString()); return;
    }
    if (demo_) { emit notice(QString("Демо: %1 = %2%%").arg(target).arg(value),false); emit ecFinished(true,QString()); return; }
    if (sleeping_) {
        const QString reason="Подготовка ко сну: EC command не отправлена";
        emit notice(reason,true); emit ecFinished(false,reason); return;
    }
    if (ecBusy_) { emit notice("EC broker занят предыдущей командой",true); emit ecFinished(false,QString()); return; }
    if (!ecAvailable_) {
        emit notice("EC broker недоступен или использует старый EC protocol; команда не применена",true);
        emit ecFinished(false,QString());
        return;
    }
    requestEcProvenance([this,target,value](const QJsonObject &provenance){
        sendEcCommand(provenance,target,value);
    },[this](const QString &reason){
        emit notice("EC status недоступен: "+reason,true);
        emit ecFinished(false,reason);
    });
}
void HardwareBackend::applyEcCurves(const reference::CoolerControlGraphs &graphs,
                                    const reference::StandardFunction &cpuFunction,
                                    const reference::StandardFunction &gpuFunction,
                                    std::optional<double> testStopCpuC) {
    if (demo_) { emit notice("Демо: automatic curves не отправляют hardware command",false); return; }
    if (ecBusy_) return;
    if (!ecAvailable_ || sleeping_) return;
    requestEcProvenance([this,graphs,cpuFunction,gpuFunction,testStopCpuC](const QJsonObject &provenance){
        const auto cpu=provenance["cpu_c"];
        const auto state=provenance["gpu_state"].toString();
        if (!cpu.isDouble() || !std::isfinite(cpu.toDouble()) ||
            (state=="active" && (!provenance["gpu_c"].isDouble() ||
                                   !std::isfinite(provenance["gpu_c"].toDouble())))) {
            ecBusy_=false; emit notice("EC sampler provenance lacks valid temperatures",true);
            emit ecFinished(false,QString()); return;
        }
        if (testStopCpuC && (!std::isfinite(*testStopCpuC) || *testStopCpuC<=0 || cpu.toDouble()>=*testStopCpuC)) {
            ecBusy_=false;
            emit notice("Test-only CPU temperature limit reached in validated EC provenance",true);
            emit ecFinished(false,QString()); return;
        }
        try {
            if (!cpuProcessor_) cpuProcessor_=std::make_unique<reference::StandardGraphProcessor>(graphs.cpu,100,cpuFunction);
            else cpuProcessor_->configure(cpuFunction,1.0);
            if (!gpuProcessor_) gpuProcessor_=std::make_unique<reference::StandardGraphProcessor>(graphs.gpu,100,gpuFunction);
            else gpuProcessor_->configure(gpuFunction,1.0);
            const auto cpuResult=cpuProcessor_->update({cpu.toDouble()});
            std::optional<int> demand=cpuResult.duty;
            bool newDemand=cpuResult.duty.has_value();
            if (!demand && cpuProcessor_->state().lastAppliedDuty)
                demand=cpuProcessor_->state().lastAppliedDuty;
            if (state=="active") {
                const auto gpuResult=gpuProcessor_->update({provenance["gpu_c"].toDouble()});
                const auto gpuDuty=gpuResult.duty ? gpuResult.duty : gpuProcessor_->state().lastAppliedDuty;
                newDemand = newDemand || gpuResult.duty.has_value();
                if (!gpuDuty && !demand) { ecBusy_=false; emit ecFinished(true,QString()); return; }
                if (gpuDuty && (!demand || *gpuDuty>*demand)) demand=gpuDuty;
            } else if (state!="suspended") {
                ecBusy_=false; emit notice("EC sampler GPU power state is unconfirmed",true);
                emit ecFinished(false,QString()); return;
            }
            if (!demand || !newDemand) { ecBusy_=false; emit ecFinished(true,QString()); return; }
            sendEcCommand(provenance,"both",*demand);
        } catch (const std::exception &error) {
            ecBusy_=false; emit notice(QString::fromUtf8(error.what()),true);
            emit ecFinished(false,QString());
        }
    },[this](const QString &reason) {
        emit notice("EC status недоступен: "+reason,true);
        emit ecFinished(false,QString());
    });
}
void HardwareBackend::requestEcProvenance(std::function<void(const QJsonObject &)> callback,
                                          std::function<void(const QString &)> failure) {
    if (!ecAvailable_ || ecBusy_ || sleeping_) return;
    auto message=QDBusMessage::createMethodCall(HardwareService,HardwarePath,HardwareInterface,"GetEcProvenance");
    ecBusy_=true;
    // The first observation includes the service's bounded, noninteractive
    // Polkit check. No caller-side acquire/release round trip is required.
    auto *call=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,70000),this);
    const auto revision=ecSessionRevision_;
    connect(call,&QDBusPendingCallWatcher::finished,this,
            [this,call,revision,callback=std::move(callback),failure=std::move(failure)]() mutable {
        if (revision!=ecSessionRevision_) {
            if (failure) failure("EC request invalidated by sleep or service-owner change");
            call->deleteLater(); return;
        }
        const QDBusPendingReply<QString> reply=*call;
        if (reply.isError()) {
            ecBusy_=false;
            if (reply.error().name()==EcSamplePending) {
                resetEcCurves();
                emit ecObservationDeferred(reply.error().message());
                call->deleteLater(); return;
            }
            if (reply.error().type()!=QDBusError::LimitsExceeded) {
                ecActive_=false; ecOwnerGeneration_=0; ecCalculationEpoch_=0;
                cpuProcessor_.reset(); gpuProcessor_.reset(); emit ecChanged();
            }
            if (failure) failure(reply.error().message());
            else emit notice("EC status пока недоступен: "+reply.error().message(),false);
            call->deleteLater(); return;
        }
        QJsonParseError error; const auto document=QJsonDocument::fromJson(reply.value().toUtf8(),&error);
        if (error.error!=QJsonParseError::NoError || !document.isObject() ||
            !validEcProvenance(document.object())) {
            ecBusy_=false;
            ecActive_=false; ecOwnerGeneration_=0; ecCalculationEpoch_=0;
            cpuProcessor_.reset(); gpuProcessor_.reset(); emit ecChanged();
            if (failure) failure("некорректный thermal snapshot");
            else emit notice("EC status пока недоступен: некорректный thermal snapshot",false);
            call->deleteLater(); return;
        }
        const auto generation=document.object()["broker_owner_generation"].toVariant().toULongLong();
        const bool changed=!ecActive_ || generation!=ecOwnerGeneration_;
        if (generation!=ecOwnerGeneration_) {
            ecOwnerGeneration_=generation; ecCalculationEpoch_=0;
            cpuProcessor_.reset(); gpuProcessor_.reset();
        }
        ecActive_=true;
        if (changed) emit ecChanged();
        publishEcStatus(document.object());
        emit ecProvenanceObserved(document.object());
        callback(document.object()); call->deleteLater();
    });
}
void HardwareBackend::publishEcStatus(const QJsonObject &provenance) {
    const auto mode=provenance["fan_mode"];
    const auto cpu=provenance["cpu_target_raw"];
    const auto gpu=provenance["gpu_target_raw"];
    const auto light=provenance["keylight_raw"];
    const auto tach=provenance["cpu_tach_raw"];
    if (!mode.isDouble() || !cpu.isDouble() || !gpu.isDouble() || !light.isDouble() || !tach.isDouble()) return;
    try {
        const int fanMode=mode.toInt();
        const int cpuDuty=reference::fanRawToDuty(cpu.toInt());
        const int gpuDuty=reference::fanRawToDuty(gpu.toInt());
        const int keylight=reference::keylightRawToPercent(light.toInt());
        const int tachRaw=tach.toInt();
        if (tachRaw<0 || tachRaw>70 || (fanMode!=0 && fanMode!=2)) return;
        emit ecStatusRawChanged(fanMode,cpu.toInt(),gpu.toInt(),light.toInt(),tachRaw);
        emit ecStatusChanged(tachRaw*100,cpuDuty,gpuDuty,keylight,fanMode==2);
    } catch (const std::exception &) {
        // Status is diagnostic only; an unknown readback must not turn into a
        // guessed RPM value or weaken the write-side provenance gate.
    }
}
void HardwareBackend::refreshEcStatus() {
    if (demo_) { emit ecStatusChanged(0,0,0,50,false); return; }
    if (sleeping_) { emit ecStatusFailed("Подготовка ко сну: EC observation не отправлено"); return; }
    if (!ecAvailable_ || ecBusy_) return;
    requestEcProvenance([this](const QJsonObject &) {
        ecBusy_=false;
        emit ecStatusReady();
    },[this](const QString &reason) {
        emit notice("EC status пока недоступен: "+reason,false);
        emit ecStatusFailed(reason);
    });
}
void HardwareBackend::sendEcCommand(QJsonObject provenance,const QString &target,int value) {
    if (!ecActive_ || !ecBusy_ || sleeping_) return;
    // Temperature fields are returned by GetEcProvenance for the local
    // Standard calculation, but the write-boundary envelope intentionally has
    // an exact schema and must not carry calculation-only metadata.
    provenance.remove("cpu_c");
    provenance.remove("gpu_c");
    provenance.remove("gpu_state");
    provenance.remove("fan_mode");
    provenance.remove("cpu_target_raw");
    provenance.remove("gpu_target_raw");
    provenance.remove("keylight_raw");
    provenance.remove("cpu_tach_raw");
    provenance.remove("identity");
    provenance["calculation_epoch"]=static_cast<qint64>(++ecCalculationEpoch_);
    provenance["broker_owner_generation"]=static_cast<qint64>(ecOwnerGeneration_);
    QJsonObject command{{"provenance",provenance},{"target",target},{"value",value}};
    auto message=QDBusMessage::createMethodCall(HardwareService,HardwarePath,HardwareInterface,"ApplyEc");
    message.setArguments({QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact))});
    auto *apply=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,5000),this);
    const auto revision=ecSessionRevision_;
    connect(apply,&QDBusPendingCallWatcher::finished,this,[this,apply,target,value,revision]{
        if (revision!=ecSessionRevision_) {
            emit notice("EC reply invalidated by sleep or service-owner change",true);
            emit ecFinished(false,QString()); apply->deleteLater(); return;
        }
        const QDBusPendingReply<QString> result=*apply; ecBusy_=false; bool ok=false; QString detail;
        if (result.isError() && result.error().name()==EcSamplePending) {
            // Service certifies this request did not submit an EC write.
            // Discard its provenance; the ordinary controller tick observes
            // a new sample and calculates a new demand, never replays this one.
            resetEcCurves();
            emit ecObservationDeferred(result.error().message());
            apply->deleteLater(); return;
        }
        int rawReadback=-1;
        if (result.isError()) detail=result.error().message();
        else {
            QJsonParseError error; const auto document=QJsonDocument::fromJson(result.value().toUtf8(),&error);
            const auto object=document.object();
            const int expected=target=="keylight" ? reference::keylightPercentToRaw(value) : reference::fanDutyToRaw(value);
            ok=error.error==QJsonParseError::NoError && object["ok"].toBool()
                && object["raw_readback"].isDouble() && object["raw_readback"].toDouble()==expected;
            if (ok) rawReadback=object["raw_readback"].toInt();
            if (!ok) detail="Некорректный ответ EC transaction";
        }
        if (!ok) {
            ecActive_=false; ecOwnerGeneration_=0; ecCalculationEpoch_=0;
            cpuProcessor_.reset(); gpuProcessor_.reset();
            emit notice("EC command отклонена: "+detail,true);
        } else {
            emit notice(QString("EC command подтверждена: %1=%2%, raw readback=%3")
                        .arg(target).arg(value).arg(rawReadback),false);
            QTimer::singleShot(0,this,[this,revision] {
                if (revision==ecSessionRevision_ && !sleeping_) refreshEcStatus();
            });
        }
        emit ecChanged(); emit ecFinished(ok,ok ? target : QString()); apply->deleteLater();
    });
}
} // namespace n14