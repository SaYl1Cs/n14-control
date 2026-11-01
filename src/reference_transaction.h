#pragma once

#include "reference_control.h"
#include "reference_ownership.h"

#include <QVector>
#include <QJsonObject>

#include <functional>
#include <optional>

namespace n14::reference {

enum class EcCommandTarget {
    CpuFan,
    GpuFan,
    BothFans,
    Keylight,
};

struct EcCommandProvenance {
    QString samplerSession;
    quint64 samplerGeneration = 0;
    quint64 sampleId = 0;
    QString epochSession;
    quint64 epochGeneration = 0;
    qint64 sampledAtUnixMs = 0;
    double sampledAtBoottimeSeconds = 0.0;
    quint64 calculationEpoch = 0;
    quint64 brokerOwnerGeneration = 0;
    bool sleepAllowed = false;
};

struct EcLogicalCommand {
    EcCommandTarget target = EcCommandTarget::BothFans;
    int value = 0; // Logical fan duty or keylight percentage, always 0..100.
    EcCommandProvenance provenance;
};

struct EcTransactionClock {
    qint64 unixMs = 0;
    double boottimeSeconds = 0.0;
};

struct EcTransactionPolicy {
    QString expectedSamplerSession;
    quint64 expectedSamplerGeneration = 0;
    quint64 expectedBrokerOwnerGeneration = 0;
    qint64 maximumSampleAgeMs = 2000;
    double maximumSampleAgeSeconds = 2.0;
};

enum class EcTransactionStage { Validation, Conversion, Observation, Write, Readback, Settle, Committed };

struct EcTransactionResult {
    bool committed = false;
    int rawReadback = -1;
    QString reason;
    EcTransactionStage stage = EcTransactionStage::Validation;
    // Count before invoking write(): a throwing write may already have reached
    // firmware. These are attempts, not proof of applied or reverted values.
    int writeAttempts = 0;
    QString registerName;
    int expectedRaw = -1;
    int actualRaw = -1;
    // Observability only; captured with steady_clock, logged after EC I/O.
    // No timing fallback, extra EC access or additional wait is implied.
    qint64 keylightPrewriteUs = -1;
    qint64 keylightSleepUs = -1;
};

QJsonObject ecTransactionDiagnostics(const EcTransactionResult &result);

// Serialized executor. Its policy/provenance behavior is shared by the fake
// transport tests and the later broker-owned production transport.
class FakeEcTransactionQueue {
public:
    using SettleCallback = std::function<void(int milliseconds)>;
    // Request-local, read-only checks of ORIGINAL sample age/epoch/live PM.
    // Called synchronously before each keylight phase; a throw is terminal.
    using KeylightContinuationGuard = std::function<void()>;

    explicit FakeEcTransactionQueue(EcRegisterTransport &transport,
                                    EcTransactionPolicy policy = {},
                                    SettleCallback settle = {});

    bool start(const EcOwnershipDecision &decision);
    void revoke();
    bool active() const { return active_; }

    EcTransactionResult submit(const EcLogicalCommand &command,
                               const EcTransactionClock &now,
                               const KeylightContinuationGuard &keylightGuard = {});

    const EcTransactionPolicy &policy() const { return policy_; }

private:
    bool validate(const EcLogicalCommand &command, const EcTransactionClock &now,
                  QString *reason) const;
    bool writeReadback(const QString &registerName, int value, EcTransactionResult *result);
    EcTransactionResult execute(const EcLogicalCommand &command,
                                const KeylightContinuationGuard &keylightGuard);

    EcRegisterTransport &transport_;
    EcTransactionPolicy policy_;
    SettleCallback settle_;
    bool active_ = false;
    std::optional<EcCommandProvenance> lastProvenance_;
};

// Production transport for the pinned N14 ACPI-call protocol. The public
// surface is intentionally register-name based; callers cannot supply an EC
// address, ACPI method or arbitrary packet. It is opened only after the
// broker's ownership decision and is never used by the GUI process.
class PosixEcRegisterTransport final : public EcRegisterTransport {
public:
    explicit PosixEcRegisterTransport(
        QString procCallPath = "/proc/acpi/call",
        QString acpiMethod = "\\_SB.AMW0.WMBA");

    bool open(const EcOwnershipDecision &decision) override;
    void close() override;
    int read(const QString &registerName) override;
    void write(const QString &registerName, int value) override;

private:
    int invoke(int index, std::optional<int> value);
    QString procCallPath_;
    QString acpiMethod_;
    bool open_ = false;
};

} // namespace n14::reference