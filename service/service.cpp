#include "service.h"
#include "hardware.h"
#include "reference_json.h"

#include <QDateTime>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <time.h>

namespace {

constexpr auto LidControllerMethod = "\\_SB.PC00.LPCB.EC0.PSTA";
constexpr auto LidCacheMethod = "\\LIDS";
constexpr auto LidRefreshMethod = "\\_SB.PC00.LPCB.EC0._Q14";

std::optional<int> fixedAcpiByte(const QString &path,const char *method,QString *reason) {
    const QByteArray request(method);
    QFile writer(path);
    if (!writer.open(QIODevice::WriteOnly) || writer.write(request)!=request.size()) {
        if (reason) *reason="ACPI lid request could not be written";
        return {};
    }
    writer.close();

    QFile reader(path);
    if (!reader.open(QIODevice::ReadOnly)) {
        if (reason) *reason="ACPI lid response could not be read";
        return {};
    }
    QByteArray response=reader.read(256);
    while (!response.isEmpty()) {
        const auto byte=static_cast<unsigned char>(response.back());
        if (byte!='\0' && byte!='\r' && byte!='\n' && byte!=' ' && byte!='\t') break;
        response.chop(1);
    }
    const auto match=QRegularExpression("^0x([0-9a-fA-F]+)$").match(QString::fromLatin1(response));
    bool ok=false;
    const int value=match.hasMatch() ? match.captured(1).toInt(&ok,16) : -1;
    if (!ok || value<0 || value>255) {
        if (reason) *reason="ACPI lid response was not a byte";
        return {};
    }
    return value;
}

bool refreshFixedLid(const QString &path,QString *reason) {
    // Fixture/service tests do not expose the privileged ACPI pseudo-file. On
    // the installed root service the file is present and the fixed sequence
    // below is always attempted; absence is not a reason to affect EC control.
    if (path!="/proc/acpi/call" || !QFileInfo::exists(path)) return true;
    const auto controller=fixedAcpiByte(path,LidControllerMethod,reason);
    const auto cached=fixedAcpiByte(path,LidCacheMethod,reason);
    if (!controller || !cached || (*cached!=0 && *cached!=1)) {
        if (reason && reason->isEmpty()) *reason="ACPI lid state is invalid";
        return false;
    }
    if (((*controller & 0x04)!=0) == (*cached!=0)) return true;
    if (!fixedAcpiByte(path,LidRefreshMethod,reason)) return false;

    const auto controllerAfter=fixedAcpiByte(path,LidControllerMethod,reason);
    const auto cachedAfter=fixedAcpiByte(path,LidCacheMethod,reason);
    if (!controllerAfter || !cachedAfter || (*cachedAfter!=0 && *cachedAfter!=1) ||
        (((*controllerAfter & 0x04)!=0) != (*cachedAfter!=0))) {
        if (reason && reason->isEmpty()) *reason="ACPI lid state did not converge after refresh";
        return false;
    }
    return true;
}

QJsonObject brokerIdentity(const n14::reference::EcBroker &broker) {
    return QJsonObject{
            {"vendor",broker.facts().vendor},
            {"model",broker.facts().model},
            {"bios",broker.facts().bios},
            {"ec_version",broker.facts().ecVersion},
            {"controller_id",broker.facts().controllerId},
            {"dsdt_sha256",broker.facts().dsdtSha256},
            {"wmi_guid",broker.facts().wmiGuid}};
}

quint64 jsonUnsigned(const QJsonObject &object,const QString &key) {
    const auto value=object[key];
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        value.toDouble()<1.0 || value.toDouble()>9007199254740991.0 ||
        std::floor(value.toDouble())!=value.toDouble())
        throw std::runtime_error(("Некорректное целое поле EC: "+key).toStdString());
    return static_cast<quint64>(value.toDouble());
}

qint64 jsonSigned(const QJsonObject &object,const QString &key) {
    const auto value=object[key];
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble())!=value.toDouble() ||
        value.toDouble()<static_cast<double>(std::numeric_limits<qint64>::min()) ||
        value.toDouble()>static_cast<double>(std::numeric_limits<qint64>::max()))
        throw std::runtime_error(("Некорректное время EC: "+key).toStdString());
    return static_cast<qint64>(value.toDouble());
}

QString dbusErrorName(QDBusError::ErrorType type) {
    return QDBusError(type,QString{}).name();
}

n14::reference::EcLogicalCommand parseEcCommand(const QString &json) {
    if (json.toUtf8().size()>8192) throw std::runtime_error("Слишком большой EC запрос");
    if (n14::reference::hasDuplicateJsonMembers(json.toUtf8()))
        throw std::runtime_error("EC JSON содержит повторяющиеся поля");
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(json.toUtf8(),&error);
    if (error.error!=QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Некорректный EC JSON");
    const auto root=document.object();
    if (root.keys()!=QStringList{"provenance","target","value"})
        throw std::runtime_error("Неизвестные или отсутствующие поля EC запроса");

    const auto targetValue=root["target"];
    if (!targetValue.isString()) throw std::runtime_error("Цель EC запроса должна быть строкой");
    n14::reference::EcCommandTarget target;
    const auto targetName=targetValue.toString();
    if (targetName=="cpu") target=n14::reference::EcCommandTarget::CpuFan;
    else if (targetName=="gpu") target=n14::reference::EcCommandTarget::GpuFan;
    else if (targetName=="both") target=n14::reference::EcCommandTarget::BothFans;
    else if (targetName=="keylight") target=n14::reference::EcCommandTarget::Keylight;
    else throw std::runtime_error("Неизвестная цель EC запроса");

    const auto value=root["value"];
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble())!=value.toDouble() || value.toInt()<0 || value.toInt()>100)
        throw std::runtime_error("Значение EC должно быть целым числом 0..100");

    const auto provenanceValue=root["provenance"];
    if (!provenanceValue.isObject()) throw std::runtime_error("Provenance EC команды должна быть объектом");
    const auto provenance=provenanceValue.toObject();
    const QStringList expected{
        "broker_owner_generation","calculation_epoch","epoch_generation","epoch_session",
        "sample_id","sampled_at_boottime_seconds","sampled_at_unix_ms","sampler_generation",
        "sampler_session","sleep_allowed"};
    if (provenance.keys()!=expected) throw std::runtime_error("Неполная provenance EC команды");

    n14::reference::EcCommandProvenance p;
    if (!provenance["sampler_session"].isString() || !provenance["epoch_session"].isString())
        throw std::runtime_error("Некорректная session provenance EC команды");
    p.samplerSession=provenance["sampler_session"].toString();
    p.samplerGeneration=jsonUnsigned(provenance,"sampler_generation");
    p.sampleId=jsonUnsigned(provenance,"sample_id");
    p.epochSession=provenance["epoch_session"].toString();
    p.epochGeneration=jsonUnsigned(provenance,"epoch_generation");
    p.sampledAtUnixMs=jsonSigned(provenance,"sampled_at_unix_ms");
    const auto boot=provenance["sampled_at_boottime_seconds"];
    if (!boot.isDouble() || !std::isfinite(boot.toDouble()) || boot.toDouble()<0.0)
        throw std::runtime_error("Некорректные monotonic часы EC команды");
    p.sampledAtBoottimeSeconds=boot.toDouble();
    p.calculationEpoch=jsonUnsigned(provenance,"calculation_epoch");
    p.brokerOwnerGeneration=jsonUnsigned(provenance,"broker_owner_generation");
    if (!provenance["sleep_allowed"].isBool())
        throw std::runtime_error("Некорректный sleep gate EC команды");
    p.sleepAllowed=provenance["sleep_allowed"].toBool();
    return {target,value.toInt(),p};
}

n14::reference::EcTransactionClock transactionClock() {
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME,&value)!=0)
        throw std::runtime_error("CLOCK_BOOTTIME недоступен");
    return {QDateTime::currentMSecsSinceEpoch(),
            static_cast<double>(value.tv_sec)+static_cast<double>(value.tv_nsec)/1e9};
}

} // namespace

HardwareService::HardwareService(n14::CpuController cpu,QObject *parent)
    : HardwareService(std::move(cpu),parent,Options{}) {}

HardwareService::HardwareService(n14::CpuController cpu,QObject *parent,Options options)
    : QObject(parent),cpu_(std::move(cpu)),options_(std::move(options)),
      runtimeProvider_(options_.snapshotPolicy) {
    QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(prepareForSleep(bool)));
    QDBusConnection::systemBus().connect("org.freedesktop.DBus","/org/freedesktop/DBus",
        "org.freedesktop.DBus","NameOwnerChanged",this,SLOT(nameOwnerChanged(QString,QString,QString)));
    timer_.setSingleShot(true);
    timer_.setInterval(60000);
    connect(&authorizer_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),
            this,&HardwareService::finishAuthorization);
    connect(&authorizer_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if (error==QProcess::FailedToStart) fail("Не удалось запустить /usr/bin/pkcheck");
    });
    connect(&timer_,&QTimer::timeout,this,[this]{
        authorizer_.kill();
        fail("Авторизация не завершена за 60 секунд; настройки не изменены");
    });
}

QString HardwareService::Capabilities() {
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {"protocol",1}, {"cpu",cpu_.inspect()},
        {"ec_writes",true}, {"fan_control",true}, {"keyboard_lighting",true},
        {"ec_protocol",2},
        {"mux",false}}
    ).toJson(QJsonDocument::Compact));
}

bool HardwareService::startEcBroker(const QString &caller,QString *reason) {
    try {
        auto paths=options_.ecPaths;
        paths.sleepAllowed=!sleeping_;
        const auto identity=options_.ecIdentitySource
            ? options_.ecIdentitySource
            : std::make_shared<n14::reference::ProductionEcIdentitySource>();
        const n14::reference::EcReadOnlyProbe probe(paths,options_.ecFileSystem,
                                                    options_.ecProcessSource,identity);
        auto facts=probe.inspect(); facts.sleepAllowed=!sleeping_;
        const auto policy=options_.ecPolicy;
        QString failure;
        if (!n14::reference::EcBroker::ensureLockFile(paths,policy,&failure))
            throw std::runtime_error(failure.toStdString());
        // Re-read all facts after provisioning the fixed lock placeholder.
        facts=probe.inspect(); facts.sleepAllowed=!sleeping_;
        auto transport=options_.ecTransportFactory
            ? options_.ecTransportFactory()
            : std::make_unique<n14::reference::PosixEcRegisterTransport>();
        if (!transport) throw std::runtime_error("EC broker transport factory returned null");
        auto broker=std::make_unique<n14::reference::EcBroker>(
            paths,policy,options_.ecTransactionPolicy,std::move(transport),options_.ecFileSystem);
        if (!broker->start(facts,&failure)) throw std::runtime_error(failure.toStdString());
        ecOwnerBus_=caller;
        ecBroker_=std::move(broker);
        runtimeSampler_.reset(); latestEcSample_.reset(); lastEcObservation_.reset();
        return true;
    } catch (const std::exception &error) {
        if (reason) *reason=QString::fromUtf8(error.what());
        return false;
    }
}

QString HardwareService::ApplyCpu(const QString &json,bool interactive) {
    if (!calledFromDBus()) return {};
    if (sleeping_) { sendErrorReply(QDBusError::AccessDenied,"Подготовка ко сну; настройки CPU не изменены"); return {}; }
    if (busy_ || authorizer_.state()!=QProcess::NotRunning) { sendErrorReply(QDBusError::LimitsExceeded,"Аппаратный сервис занят"); return {}; }
    try {
        if (json.toUtf8().size()>1024) throw std::runtime_error("Слишком большой запрос");
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(json.toUtf8(),&error);
        if (error.error!=QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("Некорректный JSON");
        request_=n14::CpuRequest::fromJson(document.object());
    } catch (const std::exception &error) {
        sendErrorReply(QDBusError::InvalidArgs,QString::fromUtf8(error.what())); return {};
    }
    QStringList args{"--action-id","org.n14control.modify-cpu",
                     "--system-bus-name",message().service()};
    if (interactive) args.append("--allow-user-interaction");
    pending_=message(); pendingBus_=connection(); pendingAction_=PendingAction::Cpu;
    busy_=true; setDelayedReply(true);
    authorizer_.setProgram("/usr/bin/pkcheck"); authorizer_.setArguments(args);
    timer_.start(); authorizer_.start(); return {};
}

bool HardwareService::ensureEcBroker(const QString &caller,QString *reason) {
    if (caller.isEmpty()) {
        if (reason) *reason="Неизвестный D-Bus caller";
        return false;
    }
    if (ecBroker_) {
        if (ecBroker_->active() && ecOwnerBus_==caller) return true;
        if (ecBroker_->active()) {
            if (reason) *reason="EC broker уже занят другим владельцем";
            return false;
        }
        ecBroker_.reset(); ecOwnerBus_.clear();
        runtimeSampler_.reset(); latestEcSample_.reset(); lastEcObservation_.reset();
    }
    if (ecAuthorizedBus_!=caller) return false;
    return startEcBroker(caller,reason);
}

bool HardwareService::existingEcOwnerDuringCpuAuthorization(const QString &caller) const {
    // Waiting for a CPU password does no EC I/O. Do not block the already
    // authorized cooling owner. No new owner/acquire/auth bypass is permitted.
    // Both CPU completion and EC transactions still execute serially here.
    return pendingAction_==PendingAction::Cpu && !caller.isEmpty() &&
        ecBroker_ && ecBroker_->active() && ecOwnerBus_==caller;
}

bool HardwareService::beginEcAuthorization(PendingAction action,const QString &ecJson) {
    if (message().service().isEmpty()) {
        sendErrorReply(QDBusError::AccessDenied,"Неизвестный D-Bus caller"); return false;
    }
    if (busy_ || authorizer_.state()!=QProcess::NotRunning) {
        sendErrorReply(QDBusError::LimitsExceeded,"Аппаратный сервис занят"); return false;
    }
    pending_=message(); pendingBus_=connection(); pendingAction_=action; pendingEcJson_=ecJson;
    busy_=true; setDelayedReply(true);
    authorizer_.setProgram("/usr/bin/pkcheck");
    authorizer_.setArguments({"--action-id","org.n14control.modify-ec",
                              "--system-bus-name",message().service()});
    timer_.start(); authorizer_.start(); return true;
}

HardwareService::EcReply HardwareService::deferEcObservation(
        const QString &reason,const n14::reference::EcTransactionClock &clock) {
    EcReply result;
    // Keep the source's strict ORIGINAL total age <5 s. Unlike its cooling
    // hold policy, this typed observation-only wait has no temperature cutoff:
    // user explicitly requested continued GPU-transition handling when hot.
    // It grants NO write authority and never renews either original clock.
    latestEcSample_.reset(); // No old ApplyEc token is usable while waiting.
    if (lastEcObservation_) {
        const auto &last=*lastEcObservation_;
        const auto wallAge=clock.unixMs-last.sampledAtUnixMs;
        const auto bootAge=clock.boottimeSeconds-last.sampledAtBoottimeSeconds;
        if (wallAge>=0 && wallAge<5000 && bootAge>=0 && bootAge<5.0) {
            result.errorName=n14::EcSamplePending; result.message=reason; return result;
        }
    }
    revokeEc(); result.error=QDBusError::Failed;
    result.message="GPU observation unavailable beyond original-age grace; explicit restart required: "+reason;
    return result;
}

HardwareService::EcReply HardwareService::readEcProvenance() {
    EcReply result;
    if (sleeping_) { result.error=QDBusError::AccessDenied; result.message="Подготовка ко сну; EC provenance недоступна"; return result; }
    if (!ecBroker_ || !ecBroker_->active()) {
        result.error=QDBusError::AccessDenied; result.message="EC broker ownership не подтверждено"; return result;
    }
    try {
        const auto clock=transactionClock();
        const auto snapshot=n14::reference::parseReferenceSnapshot(runtimeProvider_.read());
        auto nextSampler=runtimeSampler_;
        n14::reference::ValidatedThermalSample sample;
        try { sample=nextSampler.accept(snapshot,clock.unixMs,clock.boottimeSeconds); }
        catch (const n14::reference::SensorUnavailable &error) {
            // Only known native GPU observation gaps, never transport failures.
            // An invalid temperature byte is not a usable measurement; the
            // producer deliberately publishes the same error-mode envelope.
            // All schema, permission, CPU, transport and stale-clock errors
            // retain their existing failure path; no broad LimitsExceeded retry.
            const QString text=QString::fromUtf8(error.what());
            const QStringList knownGpuErrors{
                "GPU EC/PM read: GPU power state is unconfirmed",
                "GPU EC/PM read: GPU power state changed during EC sample",
                "GPU EC/PM read: GPU power state changed or is unavailable during observation",
                "GPU EC/PM read: EC temperature byte is outside 1..119 C"};
            if (text=="GPU power state unknown/transitional/error" && snapshot.errors.size()==1 &&
                !snapshot.gpuTempMilliC && !snapshot.gpuSampledAt && !snapshot.gpuSample &&
                knownGpuErrors.contains(snapshot.errors.first()))
                return deferEcObservation(snapshot.errors.first(),clock);
            throw;
        }
        if (options_.ecLiveGpuPowerState) {
            const auto live=options_.ecLiveGpuPowerState();
            const auto expected=sample.gpuState==n14::reference::GpuState::Active
                ? qMakePair(QStringLiteral("active"),QStringLiteral("D0"))
                : qMakePair(QStringLiteral("suspended"),QStringLiteral("D3cold"));
            if (!live || *live!=expected)
                return deferEcObservation("GPU power mode differs from validated snapshot",clock);
        }
        const auto previousEpoch=runtimeSampler_.epoch();
        if (previousEpoch && *previousEpoch!=sample.epoch) {
            // A clean accepted producer epoch needs a NEW broker generation,
            // BEFORE any demand is calculated. This is not failure recovery:
            // no failed command is retried and no firmware write is made here.
            const auto caller=ecOwnerBus_;
            revokeEc(); QString reason;
            if (!startEcBroker(caller,&reason)) throw std::runtime_error(reason.toStdString());
        }
        runtimeSampler_=std::move(nextSampler); latestEcSample_=sample;
        QJsonObject response{
            {"identity",brokerIdentity(*ecBroker_)},
            {"broker_owner_generation",static_cast<qint64>(ecBroker_->ownerGeneration())},
            {"calculation_epoch",0},
            {"epoch_generation",static_cast<qint64>(sample.epoch.samplerGeneration)},
            {"epoch_session",sample.epoch.samplerSession},
            {"sample_id",static_cast<qint64>(sample.sampleId)},
            {"sampled_at_boottime_seconds",sample.sampledAtBoottimeSeconds},
            {"sampled_at_unix_ms",sample.sampledAtUnixMs},
            {"sampler_generation",static_cast<qint64>(sample.epoch.samplerGeneration)},
            {"sampler_session",sample.epoch.samplerSession},
            {"sleep_allowed",true},
            {"cpu_c",sample.cpuC},
            {"gpu_state",sample.gpuState==n14::reference::GpuState::Active ? "active" : "suspended"}};
        if (sample.gpuC) response["gpu_c"]=*sample.gpuC;
        n14::reference::EcStatus status;
        try { status=ecBroker_->readStatus(); }
        catch (const std::exception &error) {
            // A transport failure is not a transient thermal observation.
            revokeEc();
            result.error=QDBusError::Failed;
            result.message=QString::fromUtf8(error.what());
            return result;
        }
        response["fan_mode"]=status.mode;
        response["cpu_target_raw"]=status.cpuTargetRaw;
        response["gpu_target_raw"]=status.gpuTargetRaw;
        response["keylight_raw"]=status.keylightRaw;
        response["cpu_tach_raw"]=status.cpuTachRaw;
        lastEcObservation_=sample;
        result.ok=true;
        result.value=QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
        return result;
    } catch (const std::exception &error) {
        latestEcSample_.reset();
        result.error=QDBusError::LimitsExceeded;
        result.message=QString::fromUtf8(error.what());
        return result;
    }
}

HardwareService::EcReply HardwareService::applyEcCommand(const QString &json) {
    EcReply result;
    if (sleeping_) { result.error=QDBusError::AccessDenied; result.message="Подготовка ко сну; EC command отклонён"; return result; }
    if (!ecBroker_ || !ecBroker_->active()) {
        result.error=QDBusError::AccessDenied; result.message="EC broker ownership не подтверждено"; return result;
    }
    try {
        const auto command=parseEcCommand(json);
        const auto clock=transactionClock();
        if (!latestEcSample_) throw std::runtime_error("Перед EC command нужен validated EC provenance");
        const auto &sample=*latestEcSample_;
        const auto &p=command.provenance;
        if (p.samplerSession!=sample.epoch.samplerSession ||
            p.samplerGeneration!=sample.epoch.samplerGeneration ||
            p.sampleId!=sample.sampleId ||
            p.epochSession!=sample.epoch.samplerSession ||
            p.epochGeneration!=sample.epoch.samplerGeneration ||
            p.sampledAtUnixMs!=sample.sampledAtUnixMs ||
            std::abs(p.sampledAtBoottimeSeconds-sample.sampledAtBoottimeSeconds)>0.000001 ||
            !p.sleepAllowed)
            throw std::runtime_error("EC provenance не совпадает с текущим validated thermal snapshot");
        if (options_.ecLiveGpuPowerState) {
            const auto live=options_.ecLiveGpuPowerState();
            const auto expected=sample.gpuState==n14::reference::GpuState::Active
                ? qMakePair(QStringLiteral("active"),QStringLiteral("D0"))
                : qMakePair(QStringLiteral("suspended"),QStringLiteral("D3cold"));
            if (!live || *live!=expected)
                return deferEcObservation("GPU power mode changed before EC write; no write submitted",clock);
        }
        n14::reference::FakeEcTransactionQueue::KeylightContinuationGuard keylightGuard;
        if (command.target==n14::reference::EcCommandTarget::Keylight) {
            // Copy the original Apply provenance, never refresh its clocks from
            // a newer snapshot. The executor does not retain this callback.
            keylightGuard=[this,p] {
                const auto checkOriginalAge=[&] {
                    const auto now=transactionClock();
                    const auto wallAge=now.unixMs-p.sampledAtUnixMs;
                    const auto bootAge=now.boottimeSeconds-p.sampledAtBoottimeSeconds;
                    if (sleeping_ || wallAge<0 || bootAge<0 ||
                        wallAge>=options_.ecTransactionPolicy.maximumSampleAgeMs ||
                        bootAge>=options_.ecTransactionPolicy.maximumSampleAgeSeconds)
                        throw std::runtime_error("keylight ORIGINAL Apply sample expired; no further EC write");
                    return now;
                };
                const auto now=checkOriginalAge();
                auto probe=runtimeSampler_;
                const auto fresh=probe.accept(n14::reference::parseReferenceSnapshot(runtimeProvider_.read()),
                                              now.unixMs,now.boottimeSeconds);
                if (fresh.epoch.samplerSession!=p.samplerSession ||
                    fresh.epoch.samplerSession!=p.epochSession ||
                    fresh.epoch.samplerGeneration!=p.samplerGeneration ||
                    fresh.epoch.samplerGeneration!=p.epochGeneration || !latestEcSample_ ||
                    fresh.gpuState!=latestEcSample_->gpuState)
                    throw std::runtime_error("keylight sampler/GPU epoch changed; no further EC write");
                if (options_.ecLiveGpuPowerState) {
                    const auto expected=fresh.gpuState==n14::reference::GpuState::Active
                        ? qMakePair(QStringLiteral("active"),QStringLiteral("D0"))
                        : qMakePair(QStringLiteral("suspended"),QStringLiteral("D3cold"));
                    const auto live=options_.ecLiveGpuPowerState();
                    if (!live || *live!=expected)
                        throw std::runtime_error("keylight live GPU changed; no further EC write");
                }
                // Filesystem/sysfs reads can themselves stall under memory/IO
                // pressure. Do not write using an age checked before that stall.
                checkOriginalAge();
            };
        }
        const auto transactionResult=ecBroker_->submit(command,clock,keylightGuard);
        if (!transactionResult.committed) {
            ecBroker_.reset(); ecOwnerBus_.clear();
            runtimeSampler_.reset(); latestEcSample_.reset(); lastEcObservation_.reset();
            result.error=QDBusError::Failed;
            auto diagnostics=n14::reference::ecTransactionDiagnostics(transactionResult);
            diagnostics["broker_owner_generation"]=static_cast<qint64>(p.brokerOwnerGeneration);
            diagnostics["calculation_epoch"]=static_cast<qint64>(p.calculationEpoch);
            diagnostics["sample_id"]=static_cast<qint64>(p.sampleId);
            diagnostics["sample_age_ms"]=clock.unixMs-p.sampledAtUnixMs;
            diagnostics["sample_age_seconds"]=clock.boottimeSeconds-p.sampledAtBoottimeSeconds;
            const auto details=QString::fromUtf8(QJsonDocument(diagnostics).toJson(QJsonDocument::Compact));
            qWarning().noquote()<<"N14_EC_TRANSACTION_REJECTED"<<details;
            result.message="EC transaction отклонена; authority отозвана; diagnostics="+details;
            return result;
        }
        if (command.target==n14::reference::EcCommandTarget::Keylight) {
            const auto timing=QJsonObject{{"percent",command.value},{"raw_readback",transactionResult.rawReadback},
                {"configured_prewrite_sleep_ms",20},{"prewrite_us",transactionResult.keylightPrewriteUs},
                {"sleep_us",transactionResult.keylightSleepUs},
                {"preloaded_before_auto",transactionResult.writeAttempts==6},
                {"transaction_write_attempts",transactionResult.writeAttempts}};
            qInfo().noquote()<<"N14_EC_KEYLIGHT_TIMING"<<QString::fromUtf8(QJsonDocument(timing).toJson(QJsonDocument::Compact));
        }
        result.ok=true;
        result.value=QString::fromUtf8(QJsonDocument(QJsonObject{
            {"ok",true},{"raw_readback",transactionResult.rawReadback}}
        ).toJson(QJsonDocument::Compact));
        return result;
    } catch (const std::exception &error) {
        revokeEc();
        result.error=QDBusError::InvalidArgs;
        result.message=QString::fromUtf8(error.what());
        return result;
    }
}

void HardwareService::sendPendingEcReply(const EcReply &reply) {
    if (reply.ok) pendingBus_.send(pending_.createReply(reply.value));
    else pendingBus_.send(pending_.createErrorReply(reply.errorName.isEmpty() ? dbusErrorName(reply.error) : reply.errorName,reply.message));
}

QString HardwareService::GetEcProvenance() {
    if (!calledFromDBus()) return {};
    if (sleeping_) { sendErrorReply(QDBusError::AccessDenied,"Подготовка ко сну; EC provenance недоступна"); return {}; }
    if ((busy_ || authorizer_.state()!=QProcess::NotRunning) &&
        !existingEcOwnerDuringCpuAuthorization(message().service())) {
        sendErrorReply(QDBusError::LimitsExceeded,"Аппаратный сервис занят"); return {};
    }
    QString reason;
    if (!ensureEcBroker(message().service(),&reason)) {
        if (!reason.isEmpty()) sendErrorReply(QDBusError::LimitsExceeded,reason);
        else beginEcAuthorization(PendingAction::EcProvenance);
        return {};
    }
    const auto reply=readEcProvenance();
    if (!reply.ok) {
        if (reply.errorName.isEmpty()) sendErrorReply(reply.error,reply.message);
        else sendErrorReply(reply.errorName,reply.message);
    }
    return reply.ok ? reply.value : QString{};
}

QString HardwareService::ApplyEc(const QString &json) {
    if (!calledFromDBus()) return {};
    if (sleeping_) { sendErrorReply(QDBusError::AccessDenied,"Подготовка ко сну; EC command отклонён"); return {}; }
    if ((busy_ || authorizer_.state()!=QProcess::NotRunning) &&
        !existingEcOwnerDuringCpuAuthorization(message().service())) {
        sendErrorReply(QDBusError::LimitsExceeded,"Аппаратный сервис занят"); return {};
    }
    try { (void)parseEcCommand(json); }
    catch (const std::exception &error) {
        if (ecOwnerBus_==message().service()) revokeEc();
        sendErrorReply(QDBusError::InvalidArgs,QString::fromUtf8(error.what())); return {};
    }
    QString reason;
    if (!ensureEcBroker(message().service(),&reason)) {
        if (!reason.isEmpty()) sendErrorReply(QDBusError::LimitsExceeded,reason);
        else beginEcAuthorization(PendingAction::EcApply,json);
        return {};
    }
    const auto reply=applyEcCommand(json);
    if (!reply.ok) {
        if (reply.errorName.isEmpty()) sendErrorReply(reply.error,reply.message);
        else sendErrorReply(reply.errorName,reply.message);
    }
    return reply.ok ? reply.value : QString{};
}

void HardwareService::revokeEc() {
    ecBroker_.reset();
    ecOwnerBus_.clear();
    runtimeSampler_.reset();
    latestEcSample_.reset();
    lastEcObservation_.reset();
}

void HardwareService::fail(const QString &message) {
    if (!busy_) return;
    timer_.stop(); busy_=false; pendingAction_=PendingAction::None;
    pendingEcJson_.clear();
    pendingBus_.send(pending_.createErrorReply("org.n14control.Hardware1.Error",message));
}

void HardwareService::finishAuthorization(int code,QProcess::ExitStatus status) {
    if (!busy_) return;
    timer_.stop(); authorizer_.readAllStandardOutput(); authorizer_.readAllStandardError();
    if (code!=0 || status!=QProcess::NormalExit) {
        fail("Авторизация отклонена или недоступна; настройки не изменены"); return;
    }
    if (sleeping_) { fail("Подготовка ко сну; настройки не изменены"); return; }
    if (!pendingBus_.interface()) { fail("D-Bus connection отключилась; настройки не изменены"); return; }
    const auto caller=pendingBus_.interface()->isServiceRegistered(pending_.service());
    if (!caller.isValid() || !caller.value()) { fail("Клиент отключился; настройки не изменены"); return; }

    if (pendingAction_==PendingAction::Cpu) {
        try {
            const auto state=cpu_.apply(request_);
            pendingBus_.send(pending_.createReply(QString::fromUtf8(QJsonDocument(
                QJsonObject{{"ok",true},{"cpu",state}}).toJson(QJsonDocument::Compact))));
            busy_=false; pendingAction_=PendingAction::None;
        } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }
        return;
    }
    if (pendingAction_!=PendingAction::EcProvenance && pendingAction_!=PendingAction::EcApply) {
        fail("Неизвестная аппаратная операция"); return;
    }
    try {
        QString reason;
        if (!startEcBroker(pending_.service(),&reason))
            throw std::runtime_error(reason.toStdString());
        ecAuthorizedBus_=pending_.service();
        const auto result=pendingAction_==PendingAction::EcProvenance
            ? readEcProvenance() : applyEcCommand(pendingEcJson_);
        sendPendingEcReply(result);
        pendingEcJson_.clear();
        busy_=false; pendingAction_=PendingAction::None;
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void HardwareService::prepareForSleep(bool sleeping) {
    // Reconcile the firmware lid cache before allowing the first post-resume
    // provenance request.  This is intentionally limited to the fixed,
    // model-pinned read/refresh/read sequence; it never changes fan targets or
    // firmware cooling mode.  Only a client that previously authorized this
    // broker may trigger it, so an unrelated legacy EC owner is left alone.
    if (!sleeping && !ecAuthorizedBus_.isEmpty()) {
        QString reason;
        if (!refreshFixedLid(options_.ecPaths.acpiCallPath,&reason))
            qWarning().noquote()<<"N14 lid refresh after resume failed:"<<reason;
    }
    sleeping_=sleeping;
    if (sleeping && ecBroker_) revokeEc();
    if (sleeping && busy_) {
        authorizer_.kill();
        fail("Подготовка ко сну: авторизация отменена; настройки не изменены");
    }
}

void HardwareService::nameOwnerChanged(const QString &name,const QString &,const QString &newOwner) {
    if (name==ecAuthorizedBus_ && newOwner.isEmpty())
        ecAuthorizedBus_.clear();
    if (name==ecOwnerBus_ && newOwner.isEmpty()) {
        revokeEc();
    }
}