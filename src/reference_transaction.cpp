#include "reference_transaction.h"

#include <cmath>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <QFile>
#include <QRegularExpression>
#include <stdexcept>
#include <sys/types.h>
#include <unistd.h>

namespace n14::reference {
namespace {

constexpr const char *MODE_REGISTER = "0x50";
constexpr const char *CPU_TARGET_REGISTER = "0x54";
constexpr const char *GPU_TARGET_REGISTER = "0x55";
constexpr const char *KEYLIGHT_REGISTER = "0x5b";

EcTransactionResult rejected(const QString &reason) {
    EcTransactionResult result;
    result.reason=reason;
    return result;
}

std::optional<int> registerIndex(const QString &name) {
    if (name == "0x50") return 0x50;
    if (name == "0x54") return 0x54;
    if (name == "0x55") return 0x55;
    if (name == "0x5b") return 0x5b;
    if (name == "0x5c") return 0x5c;
    return {};
}

QByteArray packet(int index, std::optional<int> value) {
    QByteArray result(24,'\0');
    result.replace(0,4,"BYDL");
    const quint32 port=0x302;
    result[4]=static_cast<char>(port & 0xff);
    result[5]=static_cast<char>((port >> 8) & 0xff);
    result[6]=static_cast<char>((port >> 16) & 0xff);
    result[7]=static_cast<char>((port >> 24) & 0xff);
    result[12]=1;
    result[14]=8;
    result[15]=static_cast<char>(index);
    if (value) {
        result[13]=1;
        result[16]=static_cast<char>(*value);
    }
    return result;
}

int parseByte(const QByteArray &raw) {
    QByteArray normalized=raw;
    while (!normalized.isEmpty()) {
        const unsigned char byte=static_cast<unsigned char>(normalized.back());
        if (byte!='\0' && byte!='\r' && byte!='\n' && byte!=' ' && byte!='\t') break;
        normalized.chop(1);
    }
    const auto text=QString::fromLatin1(normalized);
    static const QRegularExpression pattern("^0x([0-9a-fA-F]+)$");
    const auto match=pattern.match(text);
    if (!match.hasMatch()) throw std::runtime_error("ACPI transport returned a non-byte value");
    bool ok=false;
    const int value=match.captured(1).toInt(&ok,16);
    if (!ok || value < 0 || value > 255)
        throw std::runtime_error("ACPI transport returned a value outside one byte");
    return value;
}

} // namespace

FakeEcTransactionQueue::FakeEcTransactionQueue(EcRegisterTransport &transport,
                                               EcTransactionPolicy policy,
                                               SettleCallback settle)
    : transport_(transport), policy_(std::move(policy)), settle_(std::move(settle)) {
    if (!settle_) settle_=[](int) {};
}

bool FakeEcTransactionQueue::start(const EcOwnershipDecision &decision) {
    if (active_ || !decision.allowed) return false;
    if (!transport_.open(decision)) return false;
    active_=true;
    lastProvenance_.reset();
    return true;
}

void FakeEcTransactionQueue::revoke() {
    if (active_) transport_.close();
    active_=false;
    lastProvenance_.reset();
}

bool FakeEcTransactionQueue::validate(const EcLogicalCommand &command,
                                      const EcTransactionClock &now,
                                      QString *reason) const {
    const auto &p=command.provenance;
    auto deny=[&](const QString &message) {
        if (reason) *reason=message;
        return false;
    };
    if (!active_) return deny("transaction authority is not active");
    if (p.samplerSession.isEmpty() || p.epochSession.isEmpty() || p.samplerGeneration == 0 ||
        p.epochGeneration == 0 || p.sampleId == 0 || p.calculationEpoch == 0 ||
        p.brokerOwnerGeneration == 0)
        return deny("transaction provenance is incomplete");
    if (p.epochSession != p.samplerSession || p.epochGeneration != p.samplerGeneration)
        return deny("transaction provenance epoch mismatch");
    if (!policy_.expectedSamplerSession.isEmpty() &&
        p.samplerSession != policy_.expectedSamplerSession)
        return deny("transaction sampler session mismatch");
    if (policy_.expectedSamplerGeneration != 0 &&
        p.samplerGeneration != policy_.expectedSamplerGeneration)
        return deny("transaction sampler generation mismatch");
    if (policy_.expectedBrokerOwnerGeneration != 0 &&
        p.brokerOwnerGeneration != policy_.expectedBrokerOwnerGeneration)
        return deny("transaction broker owner generation mismatch");
    if (!p.sleepAllowed) return deny("transaction sleep state is not allowed");
    if (policy_.maximumSampleAgeMs <= 0 || policy_.maximumSampleAgeSeconds <= 0.0 ||
        !std::isfinite(p.sampledAtBoottimeSeconds) || !std::isfinite(now.boottimeSeconds))
        return deny("transaction freshness policy is invalid");
    const qint64 unixAge=now.unixMs-p.sampledAtUnixMs;
    const double bootAge=now.boottimeSeconds-p.sampledAtBoottimeSeconds;
    if (unixAge < 0 || unixAge > policy_.maximumSampleAgeMs || bootAge < 0.0 ||
        bootAge > policy_.maximumSampleAgeSeconds)
        return deny("transaction thermal sample is stale or from the future");
    if (command.value < 0 || command.value > 100)
        return deny("transaction logical value is outside 0..100");

    if (lastProvenance_) {
        const auto &last=*lastProvenance_;
        if (p.calculationEpoch <= last.calculationEpoch)
            return deny("transaction calculation epoch replayed");
        if (p.brokerOwnerGeneration < last.brokerOwnerGeneration)
            return deny("transaction broker owner generation moved backwards");
        if (p.samplerSession == last.samplerSession && p.samplerGeneration == last.samplerGeneration) {
            if (p.sampleId < last.sampleId)
                return deny("transaction thermal sample moved backwards");
            if (p.sampledAtUnixMs < last.sampledAtUnixMs ||
                p.sampledAtBoottimeSeconds < last.sampledAtBoottimeSeconds)
                return deny("transaction sample clock moved backwards");
        } else if (p.brokerOwnerGeneration == last.brokerOwnerGeneration) {
            return deny("transaction sampler changed without a new broker owner generation");
        }
    }
    return true;
}

QJsonObject ecTransactionDiagnostics(const EcTransactionResult &result) {
    QString stage;
    switch (result.stage) {
    case EcTransactionStage::Validation: stage="validation"; break;
    case EcTransactionStage::Conversion: stage="conversion"; break;
    case EcTransactionStage::Observation: stage="observation"; break;
    case EcTransactionStage::Write: stage="write"; break;
    case EcTransactionStage::Readback: stage="readback"; break;
    case EcTransactionStage::Settle: stage="settle"; break;
    case EcTransactionStage::Committed: stage="committed"; break;
    }
    QJsonObject diagnostics{{"stage",stage},{"write_attempts",result.writeAttempts},
        {"hardware_state_uncertain",!result.committed && result.writeAttempts>0},
        {"register",result.registerName},{"expected_raw",result.expectedRaw},{"actual_raw",result.actualRaw},
        {"reason",result.reason.left(512).simplified()}};
    if (result.keylightPrewriteUs>=0) diagnostics["keylight_prewrite_us"]=result.keylightPrewriteUs;
    if (result.keylightSleepUs>=0) diagnostics["keylight_sleep_us"]=result.keylightSleepUs;
    return diagnostics;
}

bool FakeEcTransactionQueue::writeReadback(const QString &registerName, int value,
                                           EcTransactionResult *result) {
    result->stage=EcTransactionStage::Write;
    result->registerName=registerName; result->expectedRaw=value; result->actualRaw=-1;
    ++result->writeAttempts;
    try {
        transport_.write(registerName,value);
        result->stage=EcTransactionStage::Readback;
        result->actualRaw=transport_.read(registerName);
        if (result->actualRaw != value) {
            result->reason="transaction readback mismatch for "+registerName;
            return false;
        }
        return true;
    } catch (const std::exception &error) {
        result->reason=QString("transaction transport failure: ")+QString::fromUtf8(error.what());
        return false;
    }
}

EcTransactionResult FakeEcTransactionQueue::execute(
    const EcLogicalCommand &command,const KeylightContinuationGuard &keylightGuard) {
    EcTransactionResult result;
    result.stage=EcTransactionStage::Conversion;
    const auto read=[&](const QString &name) {
        result.stage=EcTransactionStage::Observation; result.registerName=name;
        result.expectedRaw=-1; result.actualRaw=-1;
        result.actualRaw=transport_.read(name);
        return result.actualRaw;
    };
    const auto write=[&](const QString &name,int value) {
        result.stage=EcTransactionStage::Write; result.registerName=name;
        result.expectedRaw=value; result.actualRaw=-1; ++result.writeAttempts;
        transport_.write(name,value);
    };
    const auto verify=[&](const QString &name,int expected) {
        result.stage=EcTransactionStage::Readback; result.registerName=name;
        result.expectedRaw=expected; result.actualRaw=-1;
        result.actualRaw=transport_.read(name);
        if (result.actualRaw==expected) return true;
        result.reason="transaction readback mismatch for "+name; return false;
    };
    const auto guardKeylight=[&] {
        if (!keylightGuard) return;
        result.stage=EcTransactionStage::Observation; result.registerName.clear();
        result.expectedRaw=-1; result.actualRaw=-1;
        keylightGuard();
    };
    try {
        if (command.target == EcCommandTarget::Keylight) {
            const int raw=keylightPercentToRaw(command.value);
            const int mode=read(MODE_REGISTER);
            if (mode != 0 && mode != 2) {
                result.reason="transaction fan mode is unknown"; return result;
            }
            int cpuTarget=0;
            int gpuTarget=0;
            const bool restoreManual=mode == 2;
            auto prewriteStarted=std::chrono::steady_clock::now();
            if (restoreManual) {
                cpuTarget=read(CPU_TARGET_REGISTER);
                gpuTarget=read(GPU_TARGET_REGISTER);
                prewriteStarted=std::chrono::steady_clock::now();
                // User-authorized, physically verified departure from pinned
                // source: preload NEW brightness before entering auto. Two
                // adjacent writes after auto did not fix the physical lag.
                guardKeylight();
                write(KEYLIGHT_REGISTER,raw);
                write(MODE_REGISTER,0);
            }
            result.stage=EcTransactionStage::Settle; result.registerName.clear();
            result.expectedRaw=-1; result.actualRaw=-1;
            const auto sleepStarted=std::chrono::steady_clock::now();
            settle_(20);
            result.keylightSleepUs=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-sleepStarted).count();
            // Keep the normal settle/check/main-write/fan-return sequence.
            // The only additional EC operation is the preload above.
            if (!verify(MODE_REGISTER,0)) {
                result.reason="automatic fan mode not confirmed for keyboard lighting"; return result;
            }
            guardKeylight();
            result.keylightPrewriteUs=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-prewriteStarted).count();
            write(KEYLIGHT_REGISTER,raw);
            if (restoreManual) {
                guardKeylight();
                write(MODE_REGISTER,2);
                write(CPU_TARGET_REGISTER,cpuTarget);
                write(GPU_TARGET_REGISTER,gpuTarget);
                if (!verify(MODE_REGISTER,2) || !verify(CPU_TARGET_REGISTER,cpuTarget) ||
                    !verify(GPU_TARGET_REGISTER,gpuTarget))
                    return result;
            }
            if (!verify(KEYLIGHT_REGISTER,raw)) {
                result.reason="transaction keylight readback mismatch"; return result;
            }
            result.committed=true; result.rawReadback=raw; result.stage=EcTransactionStage::Committed;
            return result;
        }

        const int raw=fanDutyToRaw(command.value);
        if (!writeReadback(MODE_REGISTER,2,&result)) return result;
        if (command.target == EcCommandTarget::CpuFan || command.target == EcCommandTarget::BothFans) {
            if (!writeReadback(CPU_TARGET_REGISTER,raw,&result)) return result;
        }
        if (command.target == EcCommandTarget::GpuFan || command.target == EcCommandTarget::BothFans) {
            if (!writeReadback(GPU_TARGET_REGISTER,raw,&result)) return result;
        }
        result.committed=true; result.rawReadback=raw; result.stage=EcTransactionStage::Committed;
        return result;
    } catch (const std::exception &error) {
        result.reason=QString("transaction conversion or transport failure: ")+QString::fromUtf8(error.what());
        return result;
    }
}

EcTransactionResult FakeEcTransactionQueue::submit(const EcLogicalCommand &command,
                                                    const EcTransactionClock &now,
                                                    const KeylightContinuationGuard &keylightGuard) {
    QString reason;
    if (!validate(command,now,&reason)) {
        revoke();
        return rejected(reason);
    }
    const auto result=execute(command,keylightGuard);
    if (!result.committed) {
        // Unknown partial state is terminal. No recovery write or retry is
        // issued; a fresh start/provenance fence is required.
        revoke();
        return result;
    }
    lastProvenance_=command.provenance;
    return result;
}

PosixEcRegisterTransport::PosixEcRegisterTransport(QString procCallPath,
                                                   QString acpiMethod)
    : procCallPath_(std::move(procCallPath)), acpiMethod_(std::move(acpiMethod)) {}

bool PosixEcRegisterTransport::open(const EcOwnershipDecision &decision) {
    if (open_ || !decision.allowed || ::geteuid() != 0 ||
        acpiMethod_ != QStringLiteral("\\_SB.AMW0.WMBA"))
        return false;
    open_=true;
    return true;
}

void PosixEcRegisterTransport::close() {
    open_=false;
}

int PosixEcRegisterTransport::invoke(int index, std::optional<int> value) {
    if (!open_) throw std::runtime_error("ACPI transport is not open");
    if (index != 0x50 && index != 0x54 && index != 0x55 && index != 0x5b && index != 0x5c)
        throw std::runtime_error("ACPI register is not pinned");
    if (value && (*value < 0 || *value > 255))
        throw std::runtime_error("ACPI register value is outside one byte");

    // Keep the exact reviewed WMBA call contract used by the identity probe
    // and the original N14 diagnostic implementation.
    const QByteArray request=acpiMethod_.toLatin1()+" 0 2 b"+packet(index,value).toHex();
    const QByteArray path=QFile::encodeName(procCallPath_);
    const int writer=::open(path.constData(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if (writer < 0) throw std::runtime_error("cannot open pinned ACPI transport for write");
    const ssize_t written=::write(writer,request.constData(),request.size());
    ::close(writer);
    if (written != request.size()) throw std::runtime_error("short ACPI transport request");

    const int reader=::open(path.constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (reader < 0) throw std::runtime_error("cannot open pinned ACPI transport for readback");
    char buffer[256]{};
    const ssize_t count=::read(reader,buffer,sizeof(buffer)-1);
    ::close(reader);
    if (count <= 0) throw std::runtime_error("empty ACPI transport response");
    return parseByte(QByteArray(buffer,count));
}

int PosixEcRegisterTransport::read(const QString &registerName) {
    const auto index=registerIndex(registerName);
    if (!index) throw std::runtime_error("unknown pinned ACPI register");
    return invoke(*index,{});
}

void PosixEcRegisterTransport::write(const QString &registerName, int value) {
    if (registerName == "0x5c")
        throw std::runtime_error("tachometer register is read-only");
    const auto index=registerIndex(registerName);
    if (!index) throw std::runtime_error("unknown pinned ACPI register");
    const int response=invoke(*index,value);
    if (response != 0)
        throw std::runtime_error("ACPI transport rejected pinned register write");
}

} // namespace n14::reference