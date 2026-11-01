#include "telemetry.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <cmath>
#include <exception>
#include <utility>

namespace n14 {
static QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.read(4096)).trimmed();
}
static std::optional<double> number(const QString &path) {
    bool ok=false;
    const double n=readText(path).toDouble(&ok);
    if (!ok || !std::isfinite(n)) return {};
    return n;
}
static QJsonValue jsonNumber(std::optional<double> n) { return n ? QJsonValue(*n) : QJsonValue(QJsonValue::Null); }
QJsonObject Snapshot::toJson() const {
    return {{"timestamp_ms",timestampMs},{"battery_present",batteryPresent},
            {"external_power",externalPower ? QJsonValue(*externalPower) : QJsonValue(QJsonValue::Null)},
            {"battery_status",batteryStatus},{"capacity_percent",percent>=0 ? QJsonValue(percent) : QJsonValue(QJsonValue::Null)},
            {"battery_power_w",jsonNumber(powerW)},{"energy_wh",jsonNumber(energyWh)},
            {"cpu_temperature_c",jsonNumber(cpuC)},{"gpu_runtime_status",gpuRuntime},
            {"gpu_power_state",gpuPower},{"gpu_suspended_seconds",jsonNumber(gpuSuspendedSeconds)},
            {"rcu_lazy",rcuLazy},{"gpu_runtime_control",gpuControl},{"d3cold_allowed",d3coldAllowed},{"kernel",kernel}};
}
TelemetryReader::TelemetryReader(QString sysRoot) : sys_(std::move(sysRoot)) {
    const QDir pci(sys_+"/bus/pci/devices");
    QStringList gpuCandidates;
    for (const auto &d : pci.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        const QString p=pci.filePath(d);
        if (readText(p+"/vendor")=="0x10de" && readText(p+"/class").startsWith("0x03")) gpuCandidates.append(p);
    }
    if (gpuCandidates.size()==1) gpu_=gpuCandidates.first();
    const QDir hwmon(sys_+"/class/hwmon");
    for (const auto &d : hwmon.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        const QString p=hwmon.filePath(d), name=readText(p+"/name");
        // Deliberately exclude GPU hwmon: reading it can wake a discrete GPU.
        if (name=="coretemp" || name=="k10temp") { cpuTemperature_=p+"/temp1_input"; break; }
    }
}
Snapshot TelemetryReader::read() const {
    Snapshot s; s.timestampMs=QDateTime::currentMSecsSinceEpoch(); s.kernel=QSysInfo::kernelVersion();
    const QDir supply(sys_+"/class/power_supply");
    int count=0, capacityCount=0; double capacities=0, power=0, energy=0;
    bool completePower=true, completeEnergy=true, onlineSeen=false, online=false;
    QStringList statuses;
    for (const auto &d : supply.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        const QString p=supply.filePath(d), type=readText(p+"/type");
        // Mouse/headset batteries and their chargers are not laptop power.
        // Older system supplies without a scope attribute remain supported.
        if (readText(p+"/scope")=="Device") continue;
        if (type!="Battery") {
            const auto on=number(p+"/online");
            if (on && (*on==0 || *on==1)) { onlineSeen=true; online=online || *on==1; }
            continue;
        }
        if (readText(p+"/present")=="0") continue;
        ++count; statuses.append(readText(p+"/status"));
        const auto cap=number(p+"/capacity");
        if (cap && *cap>=0 && *cap<=100) { capacities+=*cap; ++capacityCount; }
        auto pw=number(p+"/power_now");
        if (pw) *pw=std::abs(*pw)/1e6;
        else {
            const auto i=number(p+"/current_now"), v=number(p+"/voltage_now");
            if (i && v && *v>0) pw=std::abs(*i)*(*v)/1e12;
        }
        if (pw) power+=*pw; else completePower=false;
        auto en=number(p+"/energy_now");
        if (en && *en>=0) *en/=1e6;
        else {
            en.reset();
            const auto charge=number(p+"/charge_now"), voltage=number(p+"/voltage_now");
            if (charge && *charge>=0 && voltage && *voltage>0) en=*charge*(*voltage)/1e12;
        }
        if (en) energy+=*en; else completeEnergy=false;
    }
    s.batteryPresent=count>0;
    if (onlineSeen) s.externalPower=online;
    if (count>0) {
        s.batteryStatus=statuses.first();
        for (const auto &v : statuses) if (v!=s.batteryStatus) { s.batteryStatus="Mixed"; break; }
        if (!onlineSeen) {
            if (s.batteryStatus=="Discharging") s.externalPower=false;
            else if (s.batteryStatus=="Charging") s.externalPower=true;
        }
        if (capacityCount==count) s.percent=qRound(capacities/count);
        if (completePower) s.powerW=power;
        if (completeEnergy) s.energyWh=energy;
    }
    if (!cpuTemperature_.isEmpty()) {
        const auto temp=number(cpuTemperature_);
        if (temp && *temp>0 && *temp<130000) s.cpuC=*temp/1000;
    }
    if (!gpu_.isEmpty()) {
        s.gpuRuntime=readText(gpu_+"/power/runtime_status"); s.gpuPower=readText(gpu_+"/power_state");
        // Linux exposes cumulative runtime PM times in milliseconds, not microseconds.
        const auto suspended=number(gpu_+"/power/runtime_suspended_time");
        if (suspended && *suspended>=0) s.gpuSuspendedSeconds=*suspended/1000;
        s.gpuControl=readText(gpu_+"/power/control"); s.d3coldAllowed=readText(gpu_+"/d3cold_allowed");
        if (s.gpuRuntime!=readText(gpu_+"/power/runtime_status") || s.gpuPower!=readText(gpu_+"/power_state")) {
            s.gpuRuntime="transition"; s.gpuPower="unknown";
        }
    }
    s.rcuLazy=readText(sys_+"/module/rcutree/parameters/enable_rcu_lazy");
    return s;
}
TelemetryPoller::TelemetryPoller(QObject *parent,ReadFunction read)
    : QObject(parent),read_(std::move(read)) {
    if (!read_) read_=[reader=std::optional<TelemetryReader>{}]() mutable {
        if (!reader) reader.emplace();
        return reader->read();
    };
    worker_.setObjectName("n14-telemetry");
}

TelemetryPoller::~TelemetryPoller() {
    worker_.quit();
    // At most one read is outstanding; never terminate a thread doing I/O.
    worker_.wait();
}

bool TelemetryPoller::request(Completion completion) {
    Q_ASSERT(QThread::currentThread()==thread());
    if (pending_ || !completion) return false;
    if (!workerContext_) {
        workerContext_=new QObject;
        workerContext_->moveToThread(&worker_);
        connect(&worker_,&QThread::finished,workerContext_,&QObject::deleteLater);
        worker_.start();
    }
    pending_=true;
    const bool queued=QMetaObject::invokeMethod(workerContext_,[this,completion=std::move(completion)]() mutable {
        Snapshot snapshot;
        QString error;
        try { snapshot=read_(); }
        catch (const std::exception &failure) { error=QString::fromUtf8(failure.what()).left(256); }
        catch (...) { error="Telemetry read failed"; }
        QMetaObject::invokeMethod(this,[this,snapshot=std::move(snapshot),error=std::move(error),completion=std::move(completion)]() mutable {
            pending_=false;
            completion(std::move(snapshot),std::move(error));
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
    if (!queued) pending_=false;
    return queued;
}

} // namespace n14