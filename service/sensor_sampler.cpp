#include "sensor_sampler.h"

#include "reference_ownership.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <chrono>
#include <poll.h>
#include <utility>
#include <cstdlib>
#include <unistd.h>

namespace n14::sensor {
namespace {

constexpr int CPU_OFFSET = 0x56;
constexpr int GPU_OFFSET = 0x58;
constexpr int MAX_SNAPSHOT_BYTES = 16384;

void setReason(QString *reason, const QString &value) {
    if (reason) *reason=value;
}

QByteArray readSmallFile(const QString &path, qsizetype maximumBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || maximumBytes < 0) return {};
    // sysfs files commonly report PAGE_SIZE from stat/size() even when their
    // actual textual value is only a few bytes.  Bound the stream read instead
    // of rejecting those files based on the pseudo-file size.
    const QByteArray data=file.read(maximumBytes+1);
    return data.size() > maximumBytes ? QByteArray() : data;
}

bool privateDirectory(const QString &path,quint32 expectedOwner) {
    struct stat info{};
    const QByteArray encoded=QFile::encodeName(path);
    return ::lstat(encoded.constData(),&info)==0 && S_ISDIR(info.st_mode) &&
           !S_ISLNK(info.st_mode) && static_cast<quint32>(info.st_uid)==expectedOwner &&
           (info.st_mode & 07777)==0700;
}

void notifyReady() {
    const QByteArray address=QByteArray(qgetenv("NOTIFY_SOCKET"));
    if (address.isEmpty()) return;
    sockaddr_un socketAddress{};
    socketAddress.sun_family=AF_UNIX;
    QByteArray encoded=address;
    socklen_t length=sizeof(socketAddress.sun_family);
    if (encoded.startsWith('@')) {
        socketAddress.sun_path[0]='\0';
        encoded.remove(0,1);
        if (encoded.size()>=static_cast<int>(sizeof(socketAddress.sun_path))) return;
        std::memcpy(socketAddress.sun_path+1,encoded.constData(),encoded.size());
        length+=static_cast<socklen_t>(1+encoded.size());
    } else {
        if (encoded.size()>=static_cast<int>(sizeof(socketAddress.sun_path))) return;
        std::memcpy(socketAddress.sun_path,encoded.constData(),encoded.size());
        length+=static_cast<socklen_t>(encoded.size()+1);
    }
    const int descriptor=::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if (descriptor<0) return;
    const char ready[]="READY=1";
    ::sendto(descriptor,ready,sizeof(ready)-1,MSG_NOSIGNAL,
             reinterpret_cast<const sockaddr *>(&socketAddress),length);
    ::close(descriptor);
}

double bootTime() {
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME,&value)!=0)
        throw std::runtime_error("CLOCK_BOOTTIME unavailable");
    return static_cast<double>(value.tv_sec)+static_cast<double>(value.tv_nsec)/1e9;
}

} // namespace

SamplerState::SamplerState(QString session)
    : session_(std::move(session)) {
    if (session_.isEmpty()) session_=QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void SamplerState::begin(const QString &mode) {
    if (mode != mode_) {
        ++generation_;
        mode_=mode;
    }
    ++sampleId_;
}

NativeSensorSampler::NativeSensorSampler(SamplerPaths paths,SamplerState state)
    : paths_(std::move(paths)),state_(std::move(state)) {}

int NativeSensorSampler::readEcByte(int offset) const {
    if (offset!=CPU_OFFSET && offset!=GPU_OFFSET)
        throw std::runtime_error("EC temperature offset is not whitelisted");
    const int descriptor=::open(QFile::encodeName(paths_.ecIoPath).constData(),
                                O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (descriptor<0) throw std::runtime_error("EC temperature interface cannot be opened");
    unsigned char value=0;
    const ssize_t count=::pread(descriptor,&value,1,offset);
    const int error=errno;
    ::close(descriptor);
    if (count!=1)
        throw std::runtime_error(QString("EC temperature read failed: %1")
                                 .arg(QString::fromLocal8Bit(std::strerror(error))).toStdString());
    if (value==0 || value>=120)
        throw std::runtime_error("EC temperature byte is outside 1..119 C");
    return static_cast<int>(value)*1000;
}

QString NativeSensorSampler::discoverGpu() const {
    const QDir root(paths_.pciRoot);
    QStringList devices;
    // /sys/bus/pci/devices entries are symlinks into /sys/devices.  Use all
    // entries here rather than QDir::Dirs, which can omit those links.
    for (const auto &name : root.entryList(QDir::AllEntries|QDir::NoDotAndDotDot)) {
        const QString path=root.filePath(name);
        const auto vendor=QString::fromUtf8(readSmallFile(QDir(path).filePath("vendor"),64)).trimmed();
        const auto classValue=QString::fromUtf8(readSmallFile(QDir(path).filePath("class"),64)).trimmed();
        bool ok=false;
        QString classDigits=classValue.trimmed();
        if (classDigits.startsWith("0x",Qt::CaseInsensitive)) classDigits=classDigits.mid(2);
        const int deviceClass=classDigits.toInt(&ok,16);
        // Keep only the VGA/3D display function. The HDMI audio function of
        // the same NVIDIA device is class 0x040300 and must not make GPU
        // discovery ambiguous.
        if (vendor=="0x10de" && ok && (deviceClass & 0xff0000)==0x030000)
            devices.append(path);
    }
    if (devices.size()!=1)
        throw std::runtime_error(QString("expected exactly one NVIDIA display PCI device, found %1")
                                 .arg(devices.size()).toStdString());
    return devices.first();
}

QPair<QString,QString> NativeSensorSampler::readGpuPowerState(const QString &device) const {
    const auto read=[&] {
        return qMakePair(QString::fromUtf8(readSmallFile(QDir(device).filePath("power/runtime_status"),64)).trimmed(),
                         QString::fromUtf8(readSmallFile(QDir(device).filePath("power_state"),64)).trimmed());
    };
    const auto first=read();
    if (first.first.isEmpty() || first.second.isEmpty() || read()!=first)
        throw std::runtime_error("GPU power state changed or is unavailable during observation");
    if (first!=qMakePair(QStringLiteral("suspended"),QStringLiteral("D3cold")) &&
        first!=qMakePair(QStringLiteral("active"),QStringLiteral("D0")))
        throw std::runtime_error("GPU power state is unconfirmed");
    return first;
}

std::optional<int> NativeSensorSampler::readGpuTemperature(
    const QString &device,double sampledAt,QString *state,QString *powerState,
    std::optional<double> *sampledAtOut) const {
    const auto before=readGpuPowerState(device);
    *state=before.first;
    *powerState=before.second;
    if (before==qMakePair(QStringLiteral("active"),QStringLiteral("D0"))) {
        const int value=readEcByte(GPU_OFFSET);
        if (readGpuPowerState(device)!=before)
            throw std::runtime_error("GPU power state changed during EC sample");
        *sampledAtOut=sampledAt;
        return value;
    }
    return {};
}

QJsonObject NativeSensorSampler::sample(double sampledAtBoottimeSeconds,qint64 sampledAtUnixMs) {
    std::optional<int> cpuTemperature;
    QStringList errors;
    try {
        cpuTemperature=readEcByte(CPU_OFFSET);
    } catch (const std::exception &error) {
        errors.append(QStringLiteral("CPU EC read: ")+QString::fromUtf8(error.what()));
    }

    QString gpuState="error", gpuPowerState;
    std::optional<int> gpuTemperature;
    std::optional<double> gpuSampledAt;
    try {
        const QString device=discoverGpu();
        gpuTemperature=readGpuTemperature(device,sampledAtBoottimeSeconds,
                                           &gpuState,&gpuPowerState,&gpuSampledAt);
    } catch (const std::exception &error) {
        gpuState="error";
        gpuPowerState.clear();
        gpuTemperature.reset();
        gpuSampledAt.reset();
        errors.append(QStringLiteral("GPU EC/PM read: ")+QString::fromUtf8(error.what()));
    }

    const QString mode=errors.isEmpty() && (gpuState=="active" || gpuState=="suspended")
        ? gpuState : QStringLiteral("error");
    state_.begin(mode);
    const QJsonObject common{
        {"session",state_.session()},{"generation",static_cast<qint64>(state_.generation())},
        {"sample_id",static_cast<qint64>(state_.sampleId())},
        {"sampled_at_unix_ms",sampledAtUnixMs},
        {"sampled_at_monotonic_ns",static_cast<qint64>(sampledAtBoottimeSeconds*1e9)}};
    QJsonValue cpuSample=QJsonValue(QJsonValue::Null);
    if (cpuTemperature) {
        auto value=common;
        value["sensor"]="cpu"; value["power_state"]=mode;
        cpuSample=value;
    }
    QJsonValue gpuSample=QJsonValue(QJsonValue::Null);
    if (gpuTemperature) {
        auto value=common;
        value["sensor"]="gpu"; value["power_state"]=mode;
        gpuSample=value;
    }
    QJsonObject result{
        {"schema_version",2},{"source","ec0/io"},{"sampled_at",sampledAtBoottimeSeconds},
        {"sampled_at_unix_ms",sampledAtUnixMs},
        {"sampled_at_monotonic_ns",static_cast<qint64>(sampledAtBoottimeSeconds*1e9)},
        {"sampler_session",state_.session()},
        {"sampler_generation",static_cast<qint64>(state_.generation())},
        {"sample_id",static_cast<qint64>(state_.sampleId())},
        {"gpu_state",gpuState},
        {"gpu_power_state",gpuPowerState.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(gpuPowerState)},
        {"cpu_temp",cpuTemperature ? QJsonValue(*cpuTemperature) : QJsonValue(QJsonValue::Null)},
        {"gpu_temp",gpuTemperature ? QJsonValue(*gpuTemperature) : QJsonValue(QJsonValue::Null)},
        {"gpu_sampled_at",gpuSampledAt ? QJsonValue(*gpuSampledAt) : QJsonValue(QJsonValue::Null)},
        {"cpu_sample",cpuSample},{"gpu_sample",gpuSample},
        {"errors",QJsonArray::fromStringList(errors)}};
    return result;
}

QJsonObject NativeSensorSampler::sampleNow() {
    return sample(bootTime(),QDateTime::currentMSecsSinceEpoch());
}

bool NativeSensorSampler::verifyPlatform(QString *reason) const {
    const auto policy=n14::reference::EcOwnershipPolicy::n14();
    const QDir sys(paths_.sysRoot);
    for (const auto &entry : {qMakePair(QStringLiteral("sys_vendor"),policy.expectedVendor),
                              qMakePair(QStringLiteral("product_name"),policy.expectedModel),
                              qMakePair(QStringLiteral("bios_version"),policy.expectedBios)}) {
        const auto actual=QString::fromUtf8(readSmallFile(sys.filePath("class/dmi/id/"+entry.first),4096)).trimmed();
        if (actual!=entry.second) { setReason(reason,"N14 DMI identity mismatch: "+entry.first); return false; }
    }
    QFile smbios(sys.filePath("firmware/dmi/entries/0-0/raw"));
    if (!smbios.open(QIODevice::ReadOnly) ||
        n14::reference::ProductionEcIdentitySource::parseSmbiosEcVersion(smbios.readAll()).value_or(QString())
            != policy.expectedEcVersion) {
        setReason(reason,"SMBIOS EC firmware version is not pinned to 2.0"); return false;
    }
    const QByteArray dsdt=readSmallFile(sys.filePath("firmware/acpi/tables/DSDT"),4*1024*1024);
    if (dsdt.isEmpty() || QString::fromLatin1(QCryptographicHash::hash(dsdt,QCryptographicHash::Sha256).toHex())
            != policy.expectedDsdtSha256) {
        setReason(reason,"DSDT hash mismatch"); return false;
    }
    const QRegularExpression pattern("^"+QRegularExpression::escape(policy.expectedWmiGuid)+"(?:-|:)[0-9]+$",
                                     QRegularExpression::CaseInsensitiveOption);
    bool wmi=false;
    for (const auto &entry : QDir(sys.filePath("bus/wmi/devices")).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
        if (pattern.match(entry).hasMatch()) { wmi=true; break; }
    if (!wmi) { setReason(reason,"expected WMI interface is absent"); return false; }
    return true;
}

bool NativeSensorSampler::ensureRuntimeDirectory(QString *reason) const {
    const quint32 expectedOwner=paths_.requireRootOwnership ? 0 : static_cast<quint32>(::geteuid());
    if (!QDir().mkpath(paths_.runtimeDirectory) ||
        ::chmod(QFile::encodeName(paths_.runtimeDirectory).constData(),0700)!=0 ||
        !privateDirectory(paths_.runtimeDirectory,expectedOwner)) {
        setReason(reason,"sensor runtime directory is not root-private"); return false;
    }
    return true;
}

bool NativeSensorSampler::writeSnapshot(const QJsonObject &snapshot,QString *reason) const {
    const QByteArray data=QJsonDocument(snapshot).toJson(QJsonDocument::Compact)+"\n";
    if (data.size()>MAX_SNAPSHOT_BYTES) { setReason(reason,"sensor snapshot is oversized"); return false; }
    if (!ensureRuntimeDirectory(reason)) return false;
    QByteArray templatePath=QFile::encodeName(paths_.runtimeDirectory+"/.temperatures.XXXXXX");
    int descriptor=::mkstemp(templatePath.data());
    if (descriptor<0) { setReason(reason,"sensor snapshot temporary file cannot be created"); return false; }
    bool ok=true;
    // mkstemp creates the file under the already-validated private directory;
    // when run as the production root service its owner is therefore root.
    if (::fchmod(descriptor,0600)!=0) ok=false;
    qsizetype offset=0;
    while (ok && offset<data.size()) {
        const ssize_t count=::write(descriptor,data.constData()+offset,static_cast<size_t>(data.size()-offset));
        if (count<=0) ok=false; else offset+=count;
    }
    if (ok && ::fsync(descriptor)!=0) ok=false;
    ::close(descriptor);
    const QString temporary=QString::fromLocal8Bit(templatePath);
    if (!ok || ::rename(templatePath.constData(),QFile::encodeName(paths_.snapshotPath).constData())!=0) {
        QFile::remove(temporary); setReason(reason,"sensor snapshot atomic publish failed"); return false;
    }
    return true;
}

int NativeSensorSampler::openSensorLock(QString *reason) const {
    const int descriptor=::open(QFile::encodeName(paths_.sensorLockPath).constData(),
                                O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if (descriptor<0) { setReason(reason,"sensor sampler lock cannot be opened"); return -1; }
    struct stat info{};
    if (::fstat(descriptor,&info)!=0 || !S_ISREG(info.st_mode) || info.st_uid!=0 ||
        (info.st_mode&07777)!=0600 || info.st_nlink!=1 ||
        ::flock(descriptor,LOCK_EX|LOCK_NB)!=0) {
        ::close(descriptor); setReason(reason,"sensor sampler lock is not trusted or is contended"); return -1;
    }
    return descriptor;
}

bool waitForSamplerInterval(const std::atomic_bool &stop,int intervalMs,QString *reason) {
    if (intervalMs<1 || intervalMs>60000) {
        setReason(reason,"invalid sensor sampling interval"); return false;
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(intervalMs);
    while (!stop.load()) {
        const auto remaining=deadline-std::chrono::steady_clock::now();
        if (remaining<=std::chrono::steady_clock::duration::zero()) return true;
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        const timespec timeout{static_cast<time_t>(ns/1000000000),static_cast<long>(ns%1000000000)};
        const int result=::ppoll(nullptr,0,&timeout,nullptr);
        if (result==0) return !stop.load();
        if (result<0 && errno==EINTR) continue;
        setReason(reason,"sensor sampling wait failed"); return false;
    }
    return false;
}

int NativeSensorSampler::run(std::atomic_bool &stop,int intervalMs,QString *reason) {
    if (::geteuid()!=0) { setReason(reason,"native sensor sampler requires root"); return 1; }
    if (!verifyPlatform(reason) || !ensureRuntimeDirectory(reason)) return 1;
    const int lock=openSensorLock(reason);
    if (lock<0) return 1;
    int result=0;
    bool ready=false;
    QString lastErrors;
    while (!stop.load()) {
        try {
            const auto snapshot=sampleNow();
            if (!writeSnapshot(snapshot,reason)) { result=1; break; }
            if (!ready) { notifyReady(); ready=true; }
            const auto errors=snapshot[QStringLiteral("errors")].toArray();
            QStringList messages;
            for (const auto &error : errors)
                if (error.isString()) messages.append(error.toString());
            const auto currentErrors=messages.join("; ");
            if (currentErrors!=lastErrors) {
                if (!currentErrors.isEmpty()) QTextStream(stderr)<<currentErrors<<'\n';
                lastErrors=currentErrors;
            }
        } catch (const std::exception &error) {
            setReason(reason,QString::fromUtf8(error.what())); result=1; break;
        }
        if (!waitForSamplerInterval(stop,intervalMs,reason) && !stop.load()) { result=1; break; }
    }
    ::close(lock);
    return result;
}

} // namespace n14::sensor