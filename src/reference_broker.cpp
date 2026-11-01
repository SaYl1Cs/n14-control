#include "reference_broker.h"

#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace n14::reference {
namespace {

std::atomic<quint64> nextOwnerGeneration{1};

bool fail(QString *reason, const QString &message) {
    if (reason) *reason=message;
    return false;
}

} // namespace

EcBroker::EcBroker(EcReadOnlyProbePaths paths,
                   EcOwnershipPolicy ownershipPolicy,
                   EcTransactionPolicy transactionPolicy,
                   std::unique_ptr<EcRegisterTransport> transport,
                   std::shared_ptr<const EcReadOnlyFileSystem> fileSystem,
                   std::shared_ptr<const EcOwnershipLockBackend> lockBackend)
    : paths_(std::move(paths)), ownershipPolicy_(std::move(ownershipPolicy)),
      transactionPolicy_(std::move(transactionPolicy)), transport_(std::move(transport)),
      fileSystem_(std::move(fileSystem)), lockBackend_(std::move(lockBackend)) {}

EcBroker::~EcBroker() {
    revoke();
}

bool EcBroker::start(EcOwnershipFacts facts, QString *reason) {
    if (active_) return fail(reason,"EC broker is already active");
    if (!transport_) return fail(reason,"EC broker transport is unavailable");

    lease_=EcOwnershipLease::acquire(paths_,ownershipPolicy_,fileSystem_,lockBackend_);
    if (!lease_.held())
        return fail(reason,lease_.failureReason().isEmpty()
                         ? QStringLiteral("EC broker ownership lock was not acquired")
                         : lease_.failureReason());

    lease_.applyTo(facts);
    decision_=evaluateEcOwnership(facts,ownershipPolicy_);
    if (!decision_.allowed) {
        lease_=EcOwnershipLease{};
        return fail(reason,decision_.reason);
    }

    ownerGeneration_=nextOwnerGeneration.fetch_add(1);
    transactionPolicy_.expectedBrokerOwnerGeneration=ownerGeneration_;
    facts_=facts;
    queue_=std::make_unique<FakeEcTransactionQueue>(
        *transport_,transactionPolicy_,[](int milliseconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        });
    active_=queue_->start(decision_);
    if (!active_) {
        queue_.reset();
        lease_=EcOwnershipLease{};
        ownerGeneration_=0;
        return fail(reason,"EC broker transaction queue did not start");
    }
    return true;
}

void EcBroker::revoke() {
    if (queue_) queue_->revoke();
    queue_.reset();
    if (transport_) transport_->close();
    lease_=EcOwnershipLease{};
    active_=false;
    ownerGeneration_=0;
}

EcTransactionResult EcBroker::submit(const EcLogicalCommand &command,
                                     const EcTransactionClock &now,
                                     const FakeEcTransactionQueue::KeylightContinuationGuard &keylightGuard) {
    if (!active_ || !queue_) {
        EcTransactionResult result;
        result.reason="EC broker authority is not active";
        return result;
    }
    const auto result=queue_->submit(command,now,keylightGuard);
    if (!result.committed) revoke();
    return result;
}

EcStatus EcBroker::readStatus() {
    if (!active_ || !transport_)
        throw std::runtime_error("EC broker authority is not active");
    // These are fixed, reviewed N14 readback registers.  No caller-supplied
    // register name reaches the production transport.
    return {transport_->read("0x50"), transport_->read("0x54"),
            transport_->read("0x55"), transport_->read("0x5b"),
            transport_->read("0x5c")};
}

bool EcBroker::ensureLockFile(const EcReadOnlyProbePaths &paths,
                              const EcOwnershipPolicy &policy,
                              QString *reason) {
    const QFileInfo pathInfo(paths.lockPath);
    if (pathInfo.fileName().isEmpty() || pathInfo.absolutePath() != paths.runtimeDirectory)
        return fail(reason,"EC broker lock path is outside its runtime directory");

    const QByteArray parentName=QFile::encodeName(paths.runtimeDirectory);
    const int parent=::open(parentName.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (parent < 0) return fail(reason,"EC broker runtime directory cannot be opened");

    struct stat directory{};
    const bool directoryOk=::fstat(parent,&directory) == 0 && S_ISDIR(directory.st_mode) &&
                           static_cast<quint32>(directory.st_uid) == policy.requiredOwnerUid &&
                           static_cast<quint32>(directory.st_gid) == policy.requiredGroupGid &&
                           static_cast<quint32>(directory.st_mode & 07777) == policy.requiredDirectoryMode;
    if (!directoryOk) {
        ::close(parent);
        return fail(reason,"EC broker runtime directory metadata mismatch");
    }

    const QByteArray name=QFile::encodeName(pathInfo.fileName());
    const int descriptor=::openat(parent,name.constData(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,
                                  static_cast<mode_t>(policy.requiredLockMode));
    ::close(parent);
    if (descriptor < 0) return fail(reason,"EC broker lock file cannot be created or opened");

    struct stat lock{};
    const bool lockOk=::fstat(descriptor,&lock) == 0 && S_ISREG(lock.st_mode) &&
                      static_cast<quint32>(lock.st_uid) == policy.requiredOwnerUid &&
                      static_cast<quint32>(lock.st_gid) == policy.requiredGroupGid &&
                      static_cast<quint32>(lock.st_mode & 07777) == policy.requiredLockMode &&
                      lock.st_nlink == 1;
    ::close(descriptor);
    if (!lockOk) return fail(reason,"EC broker lock file metadata mismatch");
    return true;
}

} // namespace n14::reference