#include "reference_runtime.h"

#include <QFile>
#include <QFileInfo>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace n14::reference {
namespace {

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

GpuState gpuStateFor(const QString &state) {
    if (state == "active") return GpuState::Active;
    if (state == "suspended") return GpuState::Suspended;
    throw std::runtime_error("unconfirmed GPU state in runtime snapshot");
}

bool hasNonPrivateMode(mode_t mode) {
    return (mode & (S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)) != 0;
}

} // namespace

ValidatedThermalSample RuntimeThermalSampler::accept(const QByteArray &json, qint64 nowUnixMs,
                                                     double nowBoottimeSeconds) {
    return accept(parseReferenceSnapshot(json), nowUnixMs, nowBoottimeSeconds);
}

ValidatedThermalSample RuntimeThermalSampler::accept(const ReferenceSnapshot &snapshot,
                                                     qint64 nowUnixMs,
                                                     double nowBoottimeSeconds) {
    // The producer validator owns schema/source/freshness/paired-sample and
    // active-vs-suspended semantics. Runtime always requires the full contract.
    validateReferenceSnapshot(snapshot, nowBoottimeSeconds, nowUnixMs, true, true);
    require(snapshot.samplerMetadataPresent && snapshot.samplerSession.size() > 0,
            "runtime snapshot lacks sampler identity");
    require(snapshot.sampledAtUnixMs.has_value() && snapshot.sampledAtMonotonicNs.has_value(),
            "runtime snapshot lacks clocks");
    require(snapshot.cpuTempMilliC.has_value(), "runtime CPU temperature unavailable");

    const QString &session = snapshot.samplerSession;
    const quint64 generation = snapshot.samplerGeneration;
    const quint64 sampleId = snapshot.sampleId;
    if (session_ && session == *session_) {
        require(generation_ && generation >= *generation_,
                "runtime sampler generation moved backwards");
        // Polling may observe the same fresh producer snapshot more than once.
        // Reject only a sample-id rollback; a command/calculation replay is
        // checked at the write boundary, not by making the thermal sample a
        // one-shot token.
        if (lastSampleId_ && sampleId < *lastSampleId_)
            throw SensorUnavailable("runtime thermal sample moved backwards");
    }

    ValidatedThermalSample result;
    result.cpuC = *snapshot.cpuTempMilliC / 1000.0;
    result.gpuState = gpuStateFor(snapshot.gpuState);
    if (result.gpuState == GpuState::Active) {
        require(snapshot.gpuTempMilliC.has_value(), "runtime active GPU temperature unavailable");
        result.gpuC = *snapshot.gpuTempMilliC / 1000.0;
    }
    require(std::isfinite(result.cpuC) && result.cpuC > 0.0 && result.cpuC < 120.0,
            "runtime CPU temperature outside range");
    if (result.gpuC)
        require(std::isfinite(*result.gpuC) && *result.gpuC > 0.0 && *result.gpuC < 120.0,
                "runtime GPU temperature outside range");
    result.epoch = referenceEpoch(snapshot);
    result.sampleId = sampleId;
    result.sampledAtUnixMs = *snapshot.sampledAtUnixMs;
    result.sampledAtBoottimeSeconds = *snapshot.sampledAt;

    // Commit ordering state only after every validation above succeeds. A bad
    // snapshot therefore cannot advance the consumer's provenance fence.
    session_ = session;
    generation_ = generation;
    epoch_ = result.epoch;
    lastSampleId_ = sampleId;
    return result;
}

void RuntimeThermalSampler::reset() {
    session_.reset();
    generation_.reset();
    epoch_.reset();
    lastSampleId_.reset();
}

RuntimeSnapshotProvider::RuntimeSnapshotProvider(RuntimeSnapshotFilePolicy policy)
    : policy_(std::move(policy)) {
    const QStringList components=policy_.path.split('/',Qt::SkipEmptyParts);
    const bool containsTraversal=std::any_of(components.cbegin(),components.cend(),
                                             [](const QString &component) {
                                                 return component == "." || component == "..";
                                             });
    require(QFileInfo(policy_.path).isAbsolute() && !components.isEmpty() &&
                !containsTraversal && !policy_.path.contains(QChar(0)) &&
                policy_.maximumBytes > 0,
            "Некорректная runtime snapshot file policy");
}

QByteArray RuntimeSnapshotProvider::read() const {
    const QStringList components=policy_.path.split('/',Qt::SkipEmptyParts);
    if (components.isEmpty())
        throw SensorUnavailable("runtime snapshot path has no regular file name");

    int parentFd=::open("/",O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parentFd < 0)
        throw SensorUnavailable(QString("runtime snapshot root directory unavailable: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    for (qsizetype index=0; index+1<components.size(); ++index) {
        const QByteArray encodedComponent=QFile::encodeName(components.at(index));
        const int childFd=::openat(parentFd,encodedComponent.constData(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        const int openError=errno;
        ::close(parentFd);
        if (childFd < 0)
            throw SensorUnavailable(QString("runtime snapshot directory unavailable: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(openError))));
        parentFd=childFd;
    }
    struct stat parentStat{};
    if (::fstat(parentFd,&parentStat) != 0) {
        const int error=errno; ::close(parentFd);
        throw SensorUnavailable(QString("runtime snapshot directory metadata unavailable: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(error))));
    }
    if (!S_ISDIR(parentStat.st_mode)) {
        ::close(parentFd); throw SensorUnavailable("runtime snapshot parent is not a directory");
    }
    if (policy_.requireOwner && static_cast<quint32>(parentStat.st_uid) != policy_.requiredOwnerUid) {
        ::close(parentFd); throw SensorUnavailable("runtime snapshot directory owner is not trusted");
    }
    if (policy_.requireGroup && static_cast<quint32>(parentStat.st_gid) != policy_.requiredGroupGid) {
        ::close(parentFd); throw SensorUnavailable("runtime snapshot directory group is not trusted");
    }
    if (policy_.requirePrivatePermissions && hasNonPrivateMode(parentStat.st_mode)) {
        ::close(parentFd); throw SensorUnavailable("runtime snapshot directory permissions are not private");
    }

    const QByteArray encodedName=QFile::encodeName(components.last());
    const int fd=::openat(parentFd,encodedName.constData(),O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int openError=errno;
    ::close(parentFd);
    if (fd < 0)
        throw SensorUnavailable(QString("runtime snapshot unavailable: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(openError))));

    struct stat before{};
    if (::fstat(fd,&before) != 0) {
        const int error=errno; ::close(fd);
        throw SensorUnavailable(QString("runtime snapshot metadata unavailable: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(error))));
    }
    if (!S_ISREG(before.st_mode)) {
        ::close(fd); throw SensorUnavailable("runtime snapshot is not a regular file");
    }
    if (policy_.requireOwner && static_cast<quint32>(before.st_uid) != policy_.requiredOwnerUid) {
        ::close(fd); throw SensorUnavailable("runtime snapshot owner is not trusted");
    }
    if (policy_.requireGroup && static_cast<quint32>(before.st_gid) != policy_.requiredGroupGid) {
        ::close(fd); throw SensorUnavailable("runtime snapshot group is not trusted");
    }
    if (policy_.requirePrivatePermissions && hasNonPrivateMode(before.st_mode)) {
        ::close(fd); throw SensorUnavailable("runtime snapshot permissions are not private");
    }
    if (before.st_size <= 0 || before.st_size > policy_.maximumBytes) {
        ::close(fd); throw SensorUnavailable("runtime snapshot size is outside the limit");
    }

    QByteArray data;
    data.resize(static_cast<qsizetype>(before.st_size));
    qsizetype offset=0;
    while (offset < data.size()) {
        const ssize_t count=::read(fd,data.data()+offset,static_cast<size_t>(data.size()-offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int error=errno; ::close(fd);
            throw SensorUnavailable(QString("runtime snapshot read failed: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(error))));
        }
        offset+=static_cast<qsizetype>(count);
    }
    char extra=0;
    ssize_t extraCount;
    do {
        extraCount=::read(fd,&extra,1);
    } while (extraCount < 0 && errno == EINTR);
    const int extraError=errno;
    // The descriptor was opened before the read. A producer using temp-file
    // + rename therefore cannot expose a partial replacement to this reader.
    // The size check above remains authoritative for the opened inode. A
    // non-atomic in-place writer that grows it is rejected rather than parsed.
    ::close(fd);
    if (extraCount > 0)
        throw SensorUnavailable("runtime snapshot grew beyond the size limit");
    if (extraCount < 0)
        throw SensorUnavailable(QString("runtime snapshot read failed: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(extraError))));
    return data;
}

ValidatedThermalSample RuntimeSnapshotProvider::readAndAccept(RuntimeThermalSampler &sampler,
                                                              qint64 nowUnixMs,
                                                              double nowBoottimeSeconds) const {
    return sampler.accept(read(),nowUnixMs,nowBoottimeSeconds);
}

RuntimeSamplerClock RuntimeSamplerClock::system() {
    return {
        [] { return QDateTime::currentMSecsSinceEpoch(); },
        [] {
            timespec value{};
            if (::clock_gettime(CLOCK_BOOTTIME,&value) != 0)
                throw std::runtime_error(QString("CLOCK_BOOTTIME unavailable: %1")
                                          .arg(QString::fromLocal8Bit(std::strerror(errno))).toStdString());
            return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
        }
    };
}

RuntimeThermalSamplerLoop::RuntimeThermalSamplerLoop(RuntimeSnapshotProvider provider,
                                                     int intervalMs,
                                                     int staleTimeoutMs,
                                                     RuntimeSamplerClock clock,
                                                     QObject *parent)
    : QObject(parent), provider_(std::move(provider)), clock_(std::move(clock)),
      intervalMs_(intervalMs), staleTimeoutMs_(staleTimeoutMs) {
    require(intervalMs_ > 0 && staleTimeoutMs_ > 0 && clock_.unixMs && clock_.boottimeSeconds,
            "Некорректная runtime sampler loop policy");
    timer_.setInterval(intervalMs_);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_,&QTimer::timeout,this,&RuntimeThermalSamplerLoop::timerPoll);
}

void RuntimeThermalSamplerLoop::start() {
    if (running_) return;
    sampler_.reset();
    latest_.reset();
    lastAcceptedBoottimeSeconds_.reset();
    lastError_.clear();
    fresh_=false;
    running_=true;
    timer_.start();
    pollOnce();
}

void RuntimeThermalSamplerLoop::stop() {
    if (!running_) return;
    timer_.stop();
    running_=false;
    sampler_.reset();
    latest_.reset();
    lastAcceptedBoottimeSeconds_.reset();
    lastError_.clear();
    if (fresh_) {
        fresh_=false;
        emit freshnessChanged(false);
    }
}

void RuntimeThermalSamplerLoop::expireIfNeeded(double nowBoottimeSeconds) {
    if (!fresh_ || !lastAcceptedBoottimeSeconds_ ||
        nowBoottimeSeconds < *lastAcceptedBoottimeSeconds_ ||
        (nowBoottimeSeconds - *lastAcceptedBoottimeSeconds_) * 1000.0 <= staleTimeoutMs_)
        return;
    latest_.reset();
    fresh_=false;
    emit freshnessChanged(false);
}

bool RuntimeThermalSamplerLoop::pollOnce() {
    if (!running_) return false;
    try {
        const qint64 nowUnixMs=clock_.unixMs();
        const double nowBoottimeSeconds=clock_.boottimeSeconds();
        expireIfNeeded(nowBoottimeSeconds);
        const auto previousSampleId=sampler_.sampleId();
        const auto sample=provider_.readAndAccept(sampler_,nowUnixMs,nowBoottimeSeconds);
        latest_=sample;
        lastError_.clear();
        const bool newSample=!previousSampleId || sample.sampleId > *previousSampleId;
        if (!newSample) {
            // Re-reading an unchanged snapshot is allowed for ordinary status
            // polling, but it must not extend the loop's freshness deadline.
            // Once that deadline has expired, the duplicate cannot restore a
            // fresh state; the producer must publish a newer sample.
            if (!fresh_) {
                latest_.reset();
                throw SensorUnavailable("runtime thermal sample has not advanced");
            }
            emit sampleAccepted(*latest_);
            return true;
        }
        lastAcceptedBoottimeSeconds_=nowBoottimeSeconds;
        if (!fresh_) {
            fresh_=true;
            emit freshnessChanged(true);
        }
        ++acceptedSampleCount_;
        emit sampleAccepted(*latest_);
        return true;
    } catch (const std::exception &error) {
        lastError_=QString::fromUtf8(error.what());
        emit sampleUnavailable(lastError_);
        try {
            expireIfNeeded(clock_.boottimeSeconds());
        } catch (const std::exception &clockError) {
            lastError_=QString::fromUtf8(clockError.what());
        }
        return false;
    }
}

void RuntimeThermalSamplerLoop::timerPoll() {
    pollOnce();
}

} // namespace n14::reference