#include "reference_ownership.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cerrno>
#include <sys/file.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace n14::reference {
namespace {

EcOwnershipDecision deny(const QString &reason) {
    return {false,reason};
}

void requireOpen(bool open) {
    if (!open) throw std::runtime_error("fake ACPI/EC transport is not open");
}

EcReadOnlyFileStatus inspectPath(const QString &path) {
    EcReadOnlyFileStatus result;
    struct stat info{};
    const QByteArray encoded=QFile::encodeName(path);
    if (::lstat(encoded.constData(),&info) != 0) {
        result.queryAccessible=errno != EACCES && errno != EPERM;
        return result;
    }
    result.exists=true;
    result.symbolicLink=S_ISLNK(info.st_mode);
    result.regularFile=S_ISREG(info.st_mode);
    result.directory=S_ISDIR(info.st_mode);
    result.ownerUid=static_cast<quint32>(info.st_uid);
    result.groupGid=static_cast<quint32>(info.st_gid);
    result.mode=static_cast<quint32>(info.st_mode & 07777);
    result.linkCount=static_cast<quint32>(info.st_nlink);
    return result;
}

QString readSmallText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

QStringList readCommandBasenames(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    const auto arguments=QString::fromUtf8(file.readAll()).split(QChar('\0'),Qt::SkipEmptyParts);
    QStringList result;
    for (qsizetype index=0; index<arguments.size() && index<4; ++index) {
        const QString name=QFileInfo(arguments.at(index)).fileName();
        if (!name.isEmpty() && !result.contains(name)) result.append(name);
    }
    return result;
}

QString dmiPath(const EcReadOnlyProbePaths &paths, const QString &name) {
    return QDir(paths.sysRoot).filePath("class/dmi/id/"+name);
}

QByteArray n14Packet(quint32 port, int index, std::optional<int> value = {}) {
    QByteArray packet(24, '\0');
    packet.replace(0, 4, "BYDL");
    packet[4]=static_cast<char>(port & 0xff);
    packet[5]=static_cast<char>((port >> 8) & 0xff);
    packet[6]=static_cast<char>((port >> 16) & 0xff);
    packet[7]=static_cast<char>((port >> 24) & 0xff);
    packet[12]=1;
    packet[14]=8;
    packet[15]=static_cast<char>(index);
    if (value) {
        packet[13]=1;
        packet[16]=static_cast<char>(*value);
    }
    return packet;
}

std::optional<int> parseAcpiByte(const QByteArray &raw) {
    // /proc/acpi/call may include a NUL terminator in addition to the textual
    // result. Match the reviewed legacy parser: remove only trailing NUL and
    // ASCII whitespace, then require one exact hexadecimal byte response.
    QByteArray normalized=raw;
    while (!normalized.isEmpty()) {
        const unsigned char byte=static_cast<unsigned char>(normalized.back());
        if (byte!='\0' && byte!='\r' && byte!='\n' && byte!=' ' && byte!='\t') break;
        normalized.chop(1);
    }
    const QString text=QString::fromLatin1(normalized);
    static const QRegularExpression pattern("^0x([0-9a-fA-F]+)$");
    const auto match=pattern.match(text);
    if (!match.hasMatch()) return {};
    bool ok=false;
    const int value=match.captured(1).toInt(&ok,16);
    if (!ok || value < 0 || value > 255) return {};
    return value;
}

} // namespace

PosixEcIdentityReadTransport::PosixEcIdentityReadTransport(
    QString procCallPath, QString acpiMethod)
    : procCallPath_(std::move(procCallPath)), acpiMethod_(std::move(acpiMethod)) {}

bool PosixEcIdentityReadTransport::readControllerByte(int index, int *value,
                                                       QString *reason) const {
    if (index != 0x20 && index != 0x21) {
        if (reason) *reason="controller identity index is not pinned";
        return false;
    }
    // /proc/acpi/call expects the pinned WMBA argument contract before the
    // exact indexed packet. Keep the exposed provenance method canonical
    // ("\\_SB.AMW0.WMBA"), but include the validated "0 2 b" call suffix on
    // the wire, matching the reviewed N14 diagnostic transport.
    const QByteArray method=acpiMethod_.toLatin1()+" 0 2 b";
    const QByteArray encoded=n14Packet(0x2e,index).toHex();
    const QByteArray request=method+encoded;
    const int writeDescriptor=::open(QFile::encodeName(procCallPath_).constData(),
                                     O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if (writeDescriptor < 0) {
        if (reason) *reason="EC identity transport cannot be opened for write";
        return false;
    }
    const ssize_t written=::write(writeDescriptor,request.constData(),request.size());
    ::close(writeDescriptor);
    if (written != request.size()) {
        if (reason) *reason="EC identity transport request was short";
        return false;
    }
    const int readDescriptor=::open(QFile::encodeName(procCallPath_).constData(),
                                    O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (readDescriptor < 0) {
        if (reason) *reason="EC identity transport response cannot be opened";
        return false;
    }
    char buffer[256]{};
    const ssize_t count=::read(readDescriptor,buffer,sizeof(buffer)-1);
    ::close(readDescriptor);
    if (count <= 0) {
        if (reason) *reason="EC identity transport response is empty";
        return false;
    }
    const auto parsed=parseAcpiByte(QByteArray(buffer,count));
    if (!parsed) {
        if (reason) *reason="EC identity transport returned a non-byte value";
        return false;
    }
    if (value) *value=*parsed;
    return true;
}

ProductionEcIdentitySource::ProductionEcIdentitySource(
    QString smbiosRawPath, std::shared_ptr<const EcIdentityReadTransport> transport)
    : smbiosRawPath_(std::move(smbiosRawPath)), transport_(std::move(transport)) {
    if (!transport_) transport_=std::make_shared<PosixEcIdentityReadTransport>();
}

std::optional<QString> ProductionEcIdentitySource::parseSmbiosEcVersion(const QByteArray &raw) {
    if (raw.size() < 24 || static_cast<unsigned char>(raw.at(0)) != 0) return {};
    const int length=static_cast<unsigned char>(raw.at(1));
    if (length < 24 || length > raw.size()) return {};
    const int major=static_cast<unsigned char>(raw.at(22));
    const int minor=static_cast<unsigned char>(raw.at(23));
    if (major != 2 || minor != 0) return {};
    return QStringLiteral("2.0");
}

EcReadOnlyIdentityFacts ProductionEcIdentitySource::inspect() const {
    EcReadOnlyIdentityFacts result;
    QFile file(smbiosRawPath_);
    if (!file.open(QIODevice::ReadOnly)) {
        result.failureReason="SMBIOS EC identity source cannot be opened";
        return result;
    }
    if (file.size() > 4096) {
        result.failureReason="SMBIOS EC identity source is oversized";
        return result;
    }
    result.ecVersion=parseSmbiosEcVersion(file.readAll()).value_or(QString());
    if (result.ecVersion.isEmpty()) {
        result.failureReason="SMBIOS EC firmware version is not pinned to 2.0";
        return result;
    }
    int high=0, low=0;
    QString reason;
    if (!transport_->readControllerByte(0x20,&high,&reason) ||
        !transport_->readControllerByte(0x21,&low,&reason)) {
        result.failureReason=reason.isEmpty() ? QStringLiteral("EC identity transport failed") : reason;
        return result;
    }
    result.controllerId=QString("%1:%2").arg(high,2,16,QChar('0')).arg(low,2,16,QChar('0')).toUpper();
    result.acpiMethod=QStringLiteral("\\_SB.AMW0.WMBA");
    result.transportVerified=result.ecVersion == QStringLiteral("2.0") &&
                              result.controllerId == QStringLiteral("55:71");
    return result;
}

EcReadOnlyFileStatus PosixEcReadOnlyFileSystem::status(const QString &path) const {
    return inspectPath(path);
}

QByteArray PosixEcReadOnlyFileSystem::readFile(const QString &path,
                                               qsizetype maximumBytes,
                                               bool *ok) const {
    if (ok) *ok=false;
    if (maximumBytes < 0) return {};
    const QByteArray encoded=QFile::encodeName(path);
    const int descriptor=::open(encoded.constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (descriptor < 0) return {};
    struct stat info{};
    const bool trusted=::fstat(descriptor,&info) == 0 && S_ISREG(info.st_mode) &&
                       static_cast<quint64>(info.st_size) <= static_cast<quint64>(maximumBytes);
    if (!trusted) {
        ::close(descriptor);
        return {};
    }
    QByteArray data;
    QByteArray chunk;
    chunk.resize(64*1024);
    for (;;) {
        if (data.size() == maximumBytes) {
            char extra=0;
            ssize_t count;
            do {
                count=::read(descriptor,&extra,1);
            } while (count < 0 && errno == EINTR);
            if (count < 0 || count > 0) {
                ::close(descriptor);
                return {};
            }
            break;
        }
        const qsizetype remaining=maximumBytes-data.size();
        const qsizetype request=remaining < chunk.size() ? remaining : chunk.size();
        ssize_t count;
        do {
            count=::read(descriptor,chunk.data(),static_cast<size_t>(request));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            ::close(descriptor);
            return {};
        }
        if (count == 0) break;
        data.append(chunk.constData(),static_cast<qsizetype>(count));
    }
    ::close(descriptor);
    if (ok) *ok=true;
    return data;
}

QStringList PosixEcReadOnlyFileSystem::listDirectory(const QString &path) const {
    return QDir(path).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
}

QStringList ProcEcReadOnlyProcessSource::processNames(const QString &procRoot) const {
    const QDir proc(procRoot);
    QStringList result;
    for (const auto &pid : proc.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        if (!QRegularExpression("^[0-9]+$").match(pid).hasMatch()) continue;
        const QString comm=readSmallText(proc.filePath(pid+"/comm"));
        if (!comm.isEmpty()) result.append(comm);
        for (const auto &name : readCommandBasenames(proc.filePath(pid+"/cmdline")))
            if (!result.contains(name)) result.append(name);
    }
    return result;
}

EcOwnershipPolicy EcOwnershipPolicy::n14() {
    return {};
}

EcOwnershipDecision evaluateEcOwnership(const EcOwnershipFacts &facts,
                                        const EcOwnershipPolicy &policy) {
    if (!facts.runtimeDirectoryQueryAccessible)
        return deny("runtime ownership directory cannot be inspected");
    if (!facts.runtimeDirectoryIsDirectory || !facts.runtimeDirectoryNoSymlink)
        return deny("runtime ownership directory is not a trusted directory");
    if (facts.runtimeDirectoryOwnerUid != policy.requiredOwnerUid ||
        facts.runtimeDirectoryGroupGid != policy.requiredGroupGid)
        return deny("runtime ownership directory owner/group mismatch");
    if (facts.runtimeDirectoryMode != policy.requiredDirectoryMode)
        return deny("runtime ownership directory mode mismatch");

    if (!facts.lockFileQueryAccessible)
        return deny("EC ownership lock cannot be inspected");
    if (!facts.lockFileExists || !facts.lockFileIsRegular || !facts.lockFileNoSymlink)
        return deny("EC ownership lock is not a trusted regular file");
    if (facts.lockFileOwnerUid != policy.requiredOwnerUid ||
        facts.lockFileGroupGid != policy.requiredGroupGid)
        return deny("EC ownership lock owner/group mismatch");
    if (facts.lockFileMode != policy.requiredLockMode || facts.lockFileLinkCount != 1)
        return deny("EC ownership lock mode or link count mismatch");
    if (!facts.lockHeld)
        return deny("EC ownership lock is not held");
    if (!facts.competingOwners.isEmpty())
        return deny("competing hardware owner is active: " + facts.competingOwners.join(", "));
    if (!facts.sleepAllowed)
        return deny("sleep/shutdown state does not allow EC control");
    if (!facts.readOnlyTransportVerified)
        return deny(facts.identityFailureReason.isEmpty()
                        ? QStringLiteral("read-only transport preflight is incomplete")
                        : QStringLiteral("read-only transport preflight is incomplete: ")+facts.identityFailureReason);

    if (facts.vendor != policy.expectedVendor || facts.model != policy.expectedModel ||
        facts.bios != policy.expectedBios ||
        facts.ecVersion != policy.expectedEcVersion ||
        facts.controllerId != policy.expectedControllerId)
        return deny("N14 model/BIOS/EC/controller identity mismatch");
    if (facts.dsdtSha256 != policy.expectedDsdtSha256 || facts.wmiGuid != policy.expectedWmiGuid)
        return deny("N14 DSDT/WMI identity mismatch");
    if (facts.acpiMethod != policy.expectedAcpiMethod)
        return deny("ACPI control method mismatch");
    return {true,{}};
}

EcReadOnlyProbe::EcReadOnlyProbe(
    EcReadOnlyProbePaths paths,
    std::shared_ptr<const EcReadOnlyFileSystem> fileSystem,
    std::shared_ptr<const EcReadOnlyProcessSource> processSource,
    std::shared_ptr<const EcReadOnlyIdentitySource> identitySource)
    : paths_(std::move(paths)),
      fileSystem_(std::move(fileSystem)),
      processSource_(std::move(processSource)),
      identitySource_(std::move(identitySource)) {
    if (!fileSystem_) fileSystem_=std::make_shared<PosixEcReadOnlyFileSystem>();
    if (!processSource_) processSource_=std::make_shared<ProcEcReadOnlyProcessSource>();
}

EcOwnershipFacts EcReadOnlyProbe::inspect() const {
    EcOwnershipFacts facts;
    const auto directory=fileSystem_->status(paths_.runtimeDirectory);
    facts.runtimeDirectoryIsDirectory=directory.exists && directory.directory;
    facts.runtimeDirectoryNoSymlink=directory.exists && !directory.symbolicLink;
    facts.runtimeDirectoryQueryAccessible=directory.queryAccessible;
    facts.runtimeDirectoryOwnerUid=directory.ownerUid;
    facts.runtimeDirectoryGroupGid=directory.groupGid;
    facts.runtimeDirectoryMode=directory.mode;

    const auto lock=fileSystem_->status(paths_.lockPath);
    facts.lockFileExists=lock.exists;
    facts.lockFileIsRegular=lock.exists && lock.regularFile;
    facts.lockFileNoSymlink=lock.exists && !lock.symbolicLink;
    facts.lockFileQueryAccessible=lock.queryAccessible;
    facts.lockFileOwnerUid=lock.ownerUid;
    facts.lockFileGroupGid=lock.groupGid;
    facts.lockFileMode=lock.mode;
    facts.lockFileLinkCount=lock.linkCount;
    // A read-only probe cannot prove ownership of an advisory lock.
    facts.lockHeld=false;

    facts.sleepAllowed=paths_.sleepAllowed;
    // Presence of /proc/acpi/call is only an endpoint fact. It is not a
    // successful transport/identity probe and therefore cannot authorize use.
    const auto acpiCall=fileSystem_->status(paths_.acpiCallPath);
    facts.acpiEndpointPresent=acpiCall.exists && !acpiCall.symbolicLink && acpiCall.regularFile;
    facts.acpiEndpointQueryAccessible=acpiCall.queryAccessible;
    facts.readOnlyTransportVerified=false;
    facts.acpiMethod=paths_.acpiMethod;

    bool ok=false;
    const auto readText=[&](const QString &path) {
        return QString::fromUtf8(fileSystem_->readFile(path,4096,&ok)).trimmed();
    };
    facts.vendor=readText(dmiPath(paths_,"sys_vendor"));
    facts.model=readText(dmiPath(paths_,"product_name"));
    facts.bios=readText(dmiPath(paths_,"bios_version"));
    // DMI's EC-looking field is diagnostic metadata only. The authoritative
    // EC version and controller ID remain unset until a pinned, separately
    // reviewed identity source supplies them.
    facts.dmiEcVersion=readText(dmiPath(paths_,"ec_firmware_release"));

    const QString dsdtPath=QDir(paths_.sysRoot).filePath("firmware/acpi/tables/DSDT");
    const auto dsdtBytes=fileSystem_->readFile(dsdtPath,4*1024*1024,&ok);
    facts.dsdtReadable=ok;
    if (ok)
        facts.dsdtSha256=QString::fromLatin1(QCryptographicHash::hash(dsdtBytes,QCryptographicHash::Sha256).toHex());

    const QString wmiPath=QDir(paths_.sysRoot).filePath("bus/wmi/devices");
    const QRegularExpression guidPattern(
        "^"+QRegularExpression::escape(paths_.expectedWmiGuid)+"(?:-|:)[0-9]+$",
        QRegularExpression::CaseInsensitiveOption);
    facts.wmiGuid.clear();
    for (const auto &entry : fileSystem_->listDirectory(wmiPath)) {
        if (guidPattern.match(entry).hasMatch()) {
            facts.wmiGuid=paths_.expectedWmiGuid;
            break;
        }
    }

    for (const auto &comm : processSource_->processNames(paths_.procRoot)) {
        if (paths_.knownOwnerProcessNames.contains(comm) && !facts.competingOwners.contains(comm))
            facts.competingOwners.append(comm);
    }
    if (identitySource_) {
        const auto identity=identitySource_->inspect();
        facts.readOnlyTransportVerified=identity.transportVerified;
        facts.identityFailureReason=identity.failureReason;
        if (!identity.ecVersion.isEmpty()) facts.ecVersion=identity.ecVersion;
        facts.controllerId=identity.controllerId;
        if (!identity.acpiMethod.isEmpty()) facts.acpiMethod=identity.acpiMethod;
    }
    return facts;
}

int PosixEcOwnershipLockBackend::openValidated(const QString &path,
                                               const EcOwnershipPolicy &policy,
                                               QString *reason) const {
    const QFileInfo pathInfo(path);
    const QByteArray parentName=QFile::encodeName(pathInfo.absolutePath());
    const QByteArray fileName=QFile::encodeName(pathInfo.fileName());
    const int parentDescriptor=::open(parentName.constData(),
                                      O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (parentDescriptor < 0) {
        if (reason) *reason="EC ownership lock parent directory cannot be opened";
        return -1;
    }
    const int descriptor=::openat(parentDescriptor,fileName.constData(),
                                  O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    ::close(parentDescriptor);
    if (descriptor < 0) {
        if (reason) *reason="EC ownership lock cannot be opened without following symlinks";
        return -1;
    }

    struct stat info{};
    const bool metadataReadable=::fstat(descriptor,&info) == 0;
    const bool trusted=metadataReadable && S_ISREG(info.st_mode) &&
                       static_cast<quint32>(info.st_uid) == policy.requiredOwnerUid &&
                       static_cast<quint32>(info.st_gid) == policy.requiredGroupGid &&
                       static_cast<quint32>(info.st_mode & 07777) == policy.requiredLockMode &&
                       static_cast<quint32>(info.st_nlink) == 1;
    if (!trusted) {
        ::close(descriptor);
        if (reason) *reason="EC ownership lock descriptor metadata mismatch";
        return -1;
    }
    return descriptor;
}

bool PosixEcOwnershipLockBackend::tryExclusiveNonBlocking(int descriptor,
                                                          QString *reason) const {
    if (::flock(descriptor,LOCK_EX|LOCK_NB) == 0) return true;
    if (reason) *reason="EC ownership lock is contended or cannot be acquired";
    return false;
}

void PosixEcOwnershipLockBackend::closeDescriptor(int descriptor) const {
    if (descriptor >= 0) ::close(descriptor);
}

EcOwnershipLease::EcOwnershipLease(
    int descriptor, std::shared_ptr<const EcOwnershipLockBackend> backend)
    : descriptor_(descriptor), backend_(std::move(backend)) {}

EcOwnershipLease::~EcOwnershipLease() {
    release();
}

EcOwnershipLease::EcOwnershipLease(EcOwnershipLease &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_,-1)),
      failureReason_(std::move(other.failureReason_)),
      backend_(std::move(other.backend_)) {}

EcOwnershipLease &EcOwnershipLease::operator=(EcOwnershipLease &&other) noexcept {
    if (this == &other) return *this;
    release();
    descriptor_=std::exchange(other.descriptor_,-1);
    failureReason_=std::move(other.failureReason_);
    backend_=std::move(other.backend_);
    return *this;
}

void EcOwnershipLease::release() {
    if (descriptor_ >= 0 && backend_) backend_->closeDescriptor(descriptor_);
    descriptor_=-1;
    backend_.reset();
}

EcOwnershipLease EcOwnershipLease::acquire(
    const EcReadOnlyProbePaths &paths,
    const EcOwnershipPolicy &policy,
    std::shared_ptr<const EcReadOnlyFileSystem> fileSystem,
    std::shared_ptr<const EcOwnershipLockBackend> backend) {
    EcOwnershipLease result;
    if (!fileSystem) fileSystem=std::make_shared<PosixEcReadOnlyFileSystem>();
    if (!backend) backend=std::make_shared<PosixEcOwnershipLockBackend>();

    const auto directory=fileSystem->status(paths.runtimeDirectory);
    if (!directory.exists || !directory.directory || directory.symbolicLink)
        { result.failureReason_="runtime ownership directory is not trusted"; return result; }
    if (directory.ownerUid != policy.requiredOwnerUid ||
        directory.groupGid != policy.requiredGroupGid ||
        directory.mode != policy.requiredDirectoryMode)
        { result.failureReason_="runtime ownership directory metadata mismatch"; return result; }

    const auto lock=fileSystem->status(paths.lockPath);
    if (!lock.exists || !lock.regularFile || lock.symbolicLink)
        { result.failureReason_="EC ownership lock is not an existing trusted regular file"; return result; }
    if (lock.ownerUid != policy.requiredOwnerUid ||
        lock.groupGid != policy.requiredGroupGid ||
        lock.mode != policy.requiredLockMode || lock.linkCount != 1)
        { result.failureReason_="EC ownership lock metadata mismatch"; return result; }

    QString reason;
    const int descriptor=backend->openValidated(paths.lockPath,policy,&reason);
    if (descriptor < 0) {
        result.failureReason_=reason.isEmpty() ? "EC ownership lock open failed" : reason;
        return result;
    }
    if (!backend->tryExclusiveNonBlocking(descriptor,&reason)) {
        backend->closeDescriptor(descriptor);
        result.failureReason_=reason.isEmpty() ? "EC ownership lock is contended" : reason;
        return result;
    }
    result.descriptor_=descriptor;
    result.backend_=std::move(backend);
    return result;
}

bool FakeAcpiEcTransport::open(const EcOwnershipDecision &decision) {
    if (!decision.allowed) return false;
    open_=true;
    events_.append({"open",{},0});
    return true;
}

void FakeAcpiEcTransport::close() {
    open_=false;
}

void FakeAcpiEcTransport::seed(const QString &registerName, int value) {
    values_[registerName]=value;
}

int FakeAcpiEcTransport::read(const QString &registerName) {
    requireOpen(open_);
    if (!values_.contains(registerName))
        throw std::runtime_error("fake register was not seeded");
    if (readFailures_.value(registerName) > 0) {
        readFailures_[registerName]--;
        throw std::runtime_error("injected fake read failure");
    }
    const int value=readOverrides_.contains(registerName)
        ? readOverrides_.value(registerName) : values_.value(registerName);
    events_.append({"read",registerName,value});
    return value;
}

void FakeAcpiEcTransport::write(const QString &registerName, int value) {
    requireOpen(open_);
    if (writeFailures_.value(registerName) > 0) {
        writeFailures_[registerName]--;
        throw std::runtime_error("injected fake write failure");
    }
    values_[registerName]=value;
    events_.append({"write",registerName,value});
}

void FakeAcpiEcTransport::failNextRead(const QString &registerName) {
    readFailures_[registerName]++;
}

void FakeAcpiEcTransport::failNextWrite(const QString &registerName) {
    writeFailures_[registerName]++;
}

void FakeAcpiEcTransport::overrideRead(const QString &registerName, int value) {
    readOverrides_[registerName]=value;
}

void FakeAcpiEcTransport::clearReadOverride(const QString &registerName) {
    readOverrides_.remove(registerName);
}

} // namespace n14::reference