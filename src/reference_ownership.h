#pragma once

#include <QHash>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>

namespace n14::reference {

struct EcOwnershipPolicy {
    QString expectedVendor = "COLORFUL";
    QString expectedModel = "N14 24";
    QString expectedBios = "2.07";
    QString expectedEcVersion = "2.0";
    QString expectedControllerId = "55:71";
    QString expectedDsdtSha256 =
        "4aab3c89fdaac8ab9e20d31cff3fe0a18b83877442520eb294429ff9237bef06";
    QString expectedWmiGuid = "3161c7c3-489f-4074-82ae-538101cce1c2";
    QString expectedAcpiMethod = "\\_SB.AMW0.WMBA";
    quint32 requiredOwnerUid = 0;
    quint32 requiredGroupGid = 0;
    quint32 requiredDirectoryMode = 0700;
    quint32 requiredLockMode = 0600;

    static EcOwnershipPolicy n14();
};

// Facts are supplied by a read-only probe. This struct deliberately has no
// path, file descriptor or process API: evaluating it cannot touch hardware,
// /run, /proc, debugfs or systemd.
struct EcOwnershipFacts {
    bool runtimeDirectoryIsDirectory = false;
    bool runtimeDirectoryNoSymlink = false;
    bool runtimeDirectoryQueryAccessible = true;
    quint32 runtimeDirectoryOwnerUid = 0;
    quint32 runtimeDirectoryGroupGid = 0;
    quint32 runtimeDirectoryMode = 0;

    bool lockFileExists = false;
    bool lockFileIsRegular = false;
    bool lockFileNoSymlink = false;
    bool lockFileQueryAccessible = true;
    quint32 lockFileOwnerUid = 0;
    quint32 lockFileGroupGid = 0;
    quint32 lockFileMode = 0;
    quint32 lockFileLinkCount = 0;
    bool lockHeld = false;

    QStringList competingOwners;
    bool sleepAllowed = false;
    bool readOnlyTransportVerified = false;
    QString identityFailureReason;
    bool acpiEndpointPresent = false;
    bool acpiEndpointQueryAccessible = true;
    bool dsdtReadable = false;

    QString vendor;
    QString model;
    QString bios;
    // Diagnostic DMI value only. It is deliberately not an EC identity fact.
    QString dmiEcVersion;
    QString ecVersion;
    QString controllerId;
    QString dsdtSha256;
    QString wmiGuid;
    QString acpiMethod;
};

struct EcOwnershipDecision {
    bool allowed = false;
    QString reason;
};

EcOwnershipDecision evaluateEcOwnership(const EcOwnershipFacts &facts,
                                        const EcOwnershipPolicy &policy = EcOwnershipPolicy::n14());

struct EcReadOnlyProbePaths {
    QString sysRoot = "/sys";
    QString procRoot = "/proc";
    QString runtimeDirectory = "/run/colorful-n14-control";
    QString lockPath = "/run/colorful-n14-control/ec-owner.lock";
    QString acpiCallPath = "/proc/acpi/call";
    QString acpiMethod = "\\_SB.AMW0.WMBA";
    QString expectedWmiGuid = "3161c7c3-489f-4074-82ae-538101cce1c2";
    QStringList knownOwnerProcessNames = {
        "nbfc_service", "coolercontrold", "colorful-n14-co", "colorful-n14-coolercontrold",
        "colorful-n14-cu", "colorful-n14-custom-device", "adapter_server.py",
        "profile_selector_service.py"};
    bool sleepAllowed = false; // Must be supplied by a separate lifecycle gate.
};

struct EcReadOnlyFileStatus {
    bool exists = false;
    bool symbolicLink = false;
    bool regularFile = false;
    bool directory = false;
    quint32 ownerUid = 0;
    quint32 groupGid = 0;
    quint32 mode = 0;
    quint32 linkCount = 0;
    bool queryAccessible = true;
};

// Deliberately read-only: there is no create, truncate, unlink, chmod, flock,
// or device-open operation. status() must be an lstat-style no-follow query.
class EcReadOnlyFileSystem {
public:
    virtual ~EcReadOnlyFileSystem() = default;
    virtual EcReadOnlyFileStatus status(const QString &path) const = 0;
    virtual QByteArray readFile(const QString &path, qsizetype maximumBytes,
                                bool *ok) const = 0;
    virtual QStringList listDirectory(const QString &path) const = 0;
};

class PosixEcReadOnlyFileSystem final : public EcReadOnlyFileSystem {
public:
    EcReadOnlyFileStatus status(const QString &path) const override;
    QByteArray readFile(const QString &path, qsizetype maximumBytes,
                        bool *ok) const override;
    QStringList listDirectory(const QString &path) const override;
};

class EcReadOnlyProcessSource {
public:
    virtual ~EcReadOnlyProcessSource() = default;
    virtual QStringList processNames(const QString &procRoot) const = 0;
};

class ProcEcReadOnlyProcessSource final : public EcReadOnlyProcessSource {
public:
    QStringList processNames(const QString &procRoot) const override;
};

struct EcReadOnlyIdentityFacts {
    bool transportVerified = false;
    QString failureReason;
    QString ecVersion;
    QString controllerId;
    QString acpiMethod;
};

// Platform-specific EC identity is intentionally injected. There is no
// generic DMI field that can safely stand in for EC controller identity.
class EcReadOnlyIdentitySource {
public:
    virtual ~EcReadOnlyIdentitySource() = default;
    virtual EcReadOnlyIdentityFacts inspect() const = 0;
};

// The identity read is deliberately narrower than the control transport. It
// can query only the two controller-ID bytes from the pinned 0x2e diagnostic
// port; it cannot write a fan, lighting, mode or arbitrary EC register.
class EcIdentityReadTransport {
public:
    virtual ~EcIdentityReadTransport() = default;
    virtual bool readControllerByte(int index, int *value, QString *reason) const = 0;
};

class PosixEcIdentityReadTransport final : public EcIdentityReadTransport {
public:
    explicit PosixEcIdentityReadTransport(
        QString procCallPath = "/proc/acpi/call",
        QString acpiMethod = "\\_SB.AMW0.WMBA");

    bool readControllerByte(int index, int *value, QString *reason) const override;

private:
    QString procCallPath_;
    QString acpiMethod_;
};

// Production identity source for the pinned platform. DMI/DSDT/WMI are still
// collected by EcReadOnlyProbe; this source supplies the authoritative EC
// version from SMBIOS Type 0 and the controller ID from the fixed diagnostic
// transport. It never supplies a generic DMI EC-looking field as EC identity.
class ProductionEcIdentitySource final : public EcReadOnlyIdentitySource {
public:
    explicit ProductionEcIdentitySource(
        QString smbiosRawPath = "/sys/firmware/dmi/entries/0-0/raw",
        std::shared_ptr<const EcIdentityReadTransport> transport = {});

    EcReadOnlyIdentityFacts inspect() const override;

    static std::optional<QString> parseSmbiosEcVersion(const QByteArray &raw);

private:
    QString smbiosRawPath_;
    std::shared_ptr<const EcIdentityReadTransport> transport_;
};

// Collects read-only facts only. It never acquires ownership, treats the lock
// as held, or infers that sleep/shutdown is safe. The default result therefore
// remains unsuitable for control until an explicit broker adds those facts.
class EcReadOnlyProbe {
public:
    explicit EcReadOnlyProbe(
        EcReadOnlyProbePaths paths = {},
        std::shared_ptr<const EcReadOnlyFileSystem> fileSystem = {},
        std::shared_ptr<const EcReadOnlyProcessSource> processSource = {},
        std::shared_ptr<const EcReadOnlyIdentitySource> identitySource = {});

    EcOwnershipFacts inspect() const;
    const EcReadOnlyProbePaths &paths() const { return paths_; }

private:
    EcReadOnlyProbePaths paths_;
    std::shared_ptr<const EcReadOnlyFileSystem> fileSystem_;
    std::shared_ptr<const EcReadOnlyProcessSource> processSource_;
    std::shared_ptr<const EcReadOnlyIdentitySource> identitySource_;
};

// Lock acquisition is separate from the metadata-only probe. The backend has
// no create/unlink/write operation: it validates and opens an existing lock,
// then tries one non-blocking exclusive advisory lock.
class EcOwnershipLockBackend {
public:
    virtual ~EcOwnershipLockBackend() = default;
    virtual int openValidated(const QString &path, const EcOwnershipPolicy &policy,
                              QString *reason) const = 0;
    virtual bool tryExclusiveNonBlocking(int descriptor, QString *reason) const = 0;
    virtual void closeDescriptor(int descriptor) const = 0;
};

class PosixEcOwnershipLockBackend final : public EcOwnershipLockBackend {
public:
    int openValidated(const QString &path, const EcOwnershipPolicy &policy,
                      QString *reason) const override;
    bool tryExclusiveNonBlocking(int descriptor, QString *reason) const override;
    void closeDescriptor(int descriptor) const override;
};

class EcOwnershipLease {
public:
    EcOwnershipLease() = default;
    ~EcOwnershipLease();
    EcOwnershipLease(const EcOwnershipLease &) = delete;
    EcOwnershipLease &operator=(const EcOwnershipLease &) = delete;
    EcOwnershipLease(EcOwnershipLease &&other) noexcept;
    EcOwnershipLease &operator=(EcOwnershipLease &&other) noexcept;

    static EcOwnershipLease acquire(
        const EcReadOnlyProbePaths &paths,
        const EcOwnershipPolicy &policy = EcOwnershipPolicy::n14(),
        std::shared_ptr<const EcReadOnlyFileSystem> fileSystem = {},
        std::shared_ptr<const EcOwnershipLockBackend> backend = {});

    bool held() const { return descriptor_ >= 0; }
    const QString &failureReason() const { return failureReason_; }
    void applyTo(EcOwnershipFacts &facts) const { facts.lockHeld=held(); }

private:
    EcOwnershipLease(int descriptor,
                     std::shared_ptr<const EcOwnershipLockBackend> backend);
    void release();

    int descriptor_ = -1;
    QString failureReason_;
    std::shared_ptr<const EcOwnershipLockBackend> backend_;
};

struct FakeTransportEvent {
    QString operation;
    QString registerName;
    int value = 0;
};

// The transaction queue accepts only this narrow, named-register interface.
// There is no arbitrary address/register API exposed to callers.
class EcRegisterTransport {
public:
    virtual ~EcRegisterTransport() = default;
    virtual bool open(const EcOwnershipDecision &decision) = 0;
    virtual void close() = 0;
    virtual int read(const QString &registerName) = 0;
    virtual void write(const QString &registerName, int value) = 0;
};

// Offline-only transport used to prove transaction ordering and fail-closed
// behavior. It has no connection to an actual EC, ACPI, debugfs or file path.
class FakeAcpiEcTransport final : public EcRegisterTransport {
public:
    bool open(const EcOwnershipDecision &decision) override;
    void close() override;
    bool isOpen() const { return open_; }

    void seed(const QString &registerName, int value);
    int read(const QString &registerName) override;
    void write(const QString &registerName, int value) override;
    void failNextRead(const QString &registerName);
    void failNextWrite(const QString &registerName);
    void overrideRead(const QString &registerName, int value);
    void clearReadOverride(const QString &registerName);

    const QVector<FakeTransportEvent> &events() const { return events_; }
    void clearEvents() { events_.clear(); }

private:
    bool open_ = false;
    QHash<QString,int> values_;
    QHash<QString,int> readFailures_;
    QHash<QString,int> writeFailures_;
    QHash<QString,int> readOverrides_;
    QVector<FakeTransportEvent> events_;
};

} // namespace n14::reference