#pragma once

#include "reference_ownership.h"
#include "reference_transaction.h"

#include <memory>
#include <optional>

namespace n14::reference {

struct EcStatus {
    int mode = 0;
    int cpuTargetRaw = 0;
    int gpuTargetRaw = 0;
    int keylightRaw = 0;
    int cpuTachRaw = 0;
};

// The broker is the only component allowed to combine the identity decision,
// advisory ownership lease and serialized transaction queue. It is usable with
// the fake transport in tests and with the pinned production transport in the
// privileged service. It does not create a runtime directory or lock file;
// service installation/runtime setup owns that lifecycle explicitly.
class EcBroker {
public:
    EcBroker(EcReadOnlyProbePaths paths,
             EcOwnershipPolicy ownershipPolicy,
             EcTransactionPolicy transactionPolicy,
             std::unique_ptr<EcRegisterTransport> transport,
             std::shared_ptr<const EcReadOnlyFileSystem> fileSystem = {},
             std::shared_ptr<const EcOwnershipLockBackend> lockBackend = {});
    ~EcBroker();

    EcBroker(const EcBroker &) = delete;
    EcBroker &operator=(const EcBroker &) = delete;

    // The caller supplies facts collected by the broker's read-only probe and
    // the current lifecycle state. start() acquires the existing trusted lock,
    // adds proof that this process holds it, evaluates all gates, and only then
    // opens the transport. A failed start has no transport authority.
    bool start(EcOwnershipFacts facts, QString *reason = nullptr);
    void revoke();

    bool active() const { return active_; }
    quint64 ownerGeneration() const { return ownerGeneration_; }
    const EcOwnershipFacts &facts() const { return facts_; }
    const EcOwnershipDecision &decision() const { return decision_; }

    EcTransactionResult submit(const EcLogicalCommand &command,
                               const EcTransactionClock &now,
                               const FakeEcTransactionQueue::KeylightContinuationGuard &keylightGuard = {});
    EcStatus readStatus();

    // Runtime directories are provisioned by systemd/installer. This helper
    // creates only the fixed broker lock file below an already trusted private
    // directory, never follows a symlink, and never overwrites an existing file.
    static bool ensureLockFile(const EcReadOnlyProbePaths &paths,
                               const EcOwnershipPolicy &policy,
                               QString *reason = nullptr);

private:
    EcReadOnlyProbePaths paths_;
    EcOwnershipPolicy ownershipPolicy_;
    EcTransactionPolicy transactionPolicy_;
    std::unique_ptr<EcRegisterTransport> transport_;
    std::shared_ptr<const EcReadOnlyFileSystem> fileSystem_;
    std::shared_ptr<const EcOwnershipLockBackend> lockBackend_;
    EcOwnershipLease lease_;
    std::unique_ptr<FakeEcTransactionQueue> queue_;
    EcOwnershipFacts facts_;
    EcOwnershipDecision decision_;
    quint64 ownerGeneration_ = 0;
    bool active_ = false;
};

} // namespace n14::reference