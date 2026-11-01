#include "reference_thermal.h"
#include "reference_json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>

#include <cmath>
#include <stdexcept>

namespace n14::reference {
namespace {

constexpr double MAX_AGE_SECONDS = 5.0;
constexpr qint64 SAMPLE_MAX_AGE_MS = 5000;

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

bool finite(double value) { return std::isfinite(value); }

bool canonicalUuidV4(const QString &value) {
    const QUuid uuid = QUuid::fromString(value);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value &&
           uuid.version() == QUuid::Random;
}

bool integerValue(const QJsonValue &value, qint64 low, qint64 high, qint64 *out = nullptr) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!finite(number) || std::floor(number) != number || number < low || number > high)
        return false;
    if (out) *out = static_cast<qint64>(number);
    return true;
}

std::optional<qint64> optionalInteger(const QJsonObject &object, const QString &key) {
    if (!object.contains(key) || object[key].isNull()) return {};
    qint64 result = 0;
    require(integerValue(object[key], std::numeric_limits<qint64>::min(),
                          std::numeric_limits<qint64>::max(), &result),
            "Некорректное целое поле snapshot");
    return result;
}

std::optional<SampleMetadata> parseSample(const QJsonValue &json) {
    if (json.isUndefined() || json.isNull()) return {};
    require(json.isObject(), "Sample должен быть объектом");
    const auto object = json.toObject();
    const QStringList keys{"session", "generation", "sample_id", "sampled_at_unix_ms",
                           "sampled_at_monotonic_ns", "sensor", "power_state"};
    require(object.keys().size() == keys.size(), "incomplete or unknown sensor sample members");
    for (const auto &key : keys) require(object.contains(key), "incomplete or unknown sensor sample members");
    qint64 generation = 0, sampleId = 0, unixMs = 0, monotonicNs = 0;
    require(object["session"].isString() && integerValue(object["generation"], 1,
                                                            std::numeric_limits<qint64>::max(), &generation) &&
                integerValue(object["sample_id"], 1, std::numeric_limits<qint64>::max(), &sampleId) &&
                integerValue(object["sampled_at_unix_ms"], 0, std::numeric_limits<qint64>::max(), &unixMs) &&
                integerValue(object["sampled_at_monotonic_ns"], 1,
                             std::numeric_limits<qint64>::max(), &monotonicNs) &&
                object["sensor"].isString() && object["power_state"].isString(),
            "Некорректные поля sensor sample");
    SampleMetadata result{object["session"].toString(), static_cast<quint64>(generation),
                          static_cast<quint64>(sampleId), unixMs, monotonicNs,
                          object["sensor"].toString(), object["power_state"].toString()};
    return result;
}

void validateCommonSnapshot(const ReferenceSnapshot &snapshot, double nowBoottimeSeconds,
                           qint64 nowUnixMs, bool requireMetadata, bool requireSamples) {
    require(snapshot.schemaVersion == 2 && snapshot.source == "ec0/io",
            "unsupported snapshot schema/source");
    require(snapshot.sampledAt.has_value() && finite(*snapshot.sampledAt),
            "temperature snapshot stale or invalid");
    if (nowBoottimeSeconds - *snapshot.sampledAt > MAX_AGE_SECONDS)
        throw SensorUnavailable("temperature snapshot stale or invalid");
    require(nowBoottimeSeconds >= *snapshot.sampledAt,
            "temperature snapshot stale or invalid");

    if (requireMetadata || snapshot.samplerMetadataPresent) {
        require(snapshot.sampledAtUnixMs.has_value() && snapshot.sampledAtMonotonicNs.has_value() &&
                    canonicalUuidV4(snapshot.samplerSession) && snapshot.samplerGeneration >= 1 &&
                    snapshot.sampleId >= 1,
                "invalid sampler metadata");
        const QString mode = snapshot.errors.isEmpty() ? snapshot.gpuState : "error";
        const SampleMetadata common{snapshot.samplerSession, snapshot.samplerGeneration,
                                    snapshot.sampleId, *snapshot.sampledAtUnixMs,
                                    *snapshot.sampledAtMonotonicNs, "cpu", mode};
        validateReferenceSample(common, nowUnixMs, nowBoottimeSeconds, "cpu",
                                snapshot.samplerGeneration, snapshot.samplerSession);
        require(std::abs(snapshot.sampledAtMonotonicNs.value() / 1e9 - *snapshot.sampledAt) <= 0.001,
                "snapshot clock mismatch");
        for (const auto &role : {QString("cpu"), QString("gpu")}) {
            const auto &sample = role == "cpu" ? snapshot.cpuSample : snapshot.gpuSample;
            const bool hasValue = role == "cpu" ? snapshot.cpuTempMilliC.has_value()
                                                 : snapshot.gpuTempMilliC.has_value();
            if (!hasValue) {
                require(!sample.has_value(), "sample without a measurement");
            } else if (requireSamples || sample.has_value()) {
                require(sample.has_value() && sample->session == snapshot.samplerSession &&
                            sample->generation == snapshot.samplerGeneration &&
                            sample->sampleId == snapshot.sampleId &&
                            sample->sampledAtUnixMs == *snapshot.sampledAtUnixMs &&
                            sample->sampledAtMonotonicNs == *snapshot.sampledAtMonotonicNs &&
                            sample->sensor == role && sample->powerState == mode,
                        "sample not paired with snapshot");
                validateReferenceSample(*sample, nowUnixMs, nowBoottimeSeconds, role,
                                        snapshot.samplerGeneration, snapshot.samplerSession);
            }
        }
    }
}

} // namespace

ReferenceSnapshot parseReferenceSnapshot(const QByteArray &json) {
    require(json.size() <= 16384, "snapshot too large");
    require(!hasDuplicateJsonMembers(json), "snapshot содержит повторяющиеся поля");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "Некорректный JSON snapshot");
    const auto object = document.object();
    ReferenceSnapshot result;
    qint64 integer = 0;
    require(integerValue(object["schema_version"], 0, std::numeric_limits<int>::max(), &integer),
            "Некорректная версия snapshot");
    result.schemaVersion = static_cast<int>(integer);
    require(object["source"].isString(), "Некорректный source snapshot");
    result.source = object["source"].toString();
    if (object.contains("sampled_at") && object["sampled_at"].isDouble())
        result.sampledAt = object["sampled_at"].toDouble();
    result.sampledAtUnixMs = optionalInteger(object, "sampled_at_unix_ms");
    result.sampledAtMonotonicNs = optionalInteger(object, "sampled_at_monotonic_ns");
    if (object.contains("sampler_session")) {
        require(object["sampler_session"].isString(), "Некорректная sampler session");
        result.samplerMetadataPresent = true;
        result.samplerSession = object["sampler_session"].toString();
        require(integerValue(object["sampler_generation"], 1,
                              std::numeric_limits<qint64>::max(), &integer),
                "Некорректная sampler generation");
        result.samplerGeneration = static_cast<quint64>(integer);
        require(integerValue(object["sample_id"], 1, std::numeric_limits<qint64>::max(), &integer),
                "Некорректный sample ID");
        result.sampleId = static_cast<quint64>(integer);
    }
    require(object["gpu_state"].isString(), "Некорректное GPU state");
    result.gpuState = object["gpu_state"].toString();
    if (object.contains("gpu_power_state") && object["gpu_power_state"].isString())
        result.gpuPowerState = object["gpu_power_state"].toString();
    if (object.contains("cpu_temp") && !object["cpu_temp"].isNull()) {
        require(integerValue(object["cpu_temp"], std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max(), &integer), "Некорректная CPU temperature");
        result.cpuTempMilliC = static_cast<int>(integer);
    }
    if (object.contains("gpu_temp") && !object["gpu_temp"].isNull()) {
        require(integerValue(object["gpu_temp"], std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max(), &integer), "Некорректная GPU temperature");
        result.gpuTempMilliC = static_cast<int>(integer);
    }
    if (object.contains("gpu_sampled_at") && !object["gpu_sampled_at"].isNull()) {
        require(object["gpu_sampled_at"].isDouble(), "Некорректное GPU sample clock");
        result.gpuSampledAt = object["gpu_sampled_at"].toDouble();
    }
    result.cpuSample = parseSample(object["cpu_sample"]);
    result.gpuSample = parseSample(object["gpu_sample"]);
    require(object.contains("errors") && object["errors"].isArray(),
            "snapshot errors field is missing");
    result.errorsPresent = true;
    for (const auto &value : object["errors"].toArray()) {
        require(value.isString(), "snapshot errors must be strings");
        result.errors.append(value.toString());
    }
    return result;
}

void validateReferenceSample(const SampleMetadata &sample, qint64 nowUnixMs,
                             double nowBoottimeSeconds, const QString &sensor,
                             std::optional<quint64> generation, const QString &session) {
    require(canonicalUuidV4(sample.session), "invalid sampler session UUID");
    require(sample.generation >= 1 && sample.sampleId >= 1 && sample.sampledAtMonotonicNs >= 1,
            "invalid sampler generation/sample ID");
    const qint64 wallAge = nowUnixMs - sample.sampledAtUnixMs;
    const double bootAge = nowBoottimeSeconds - sample.sampledAtMonotonicNs / 1e9;
    require(0 <= wallAge && wallAge <= SAMPLE_MAX_AGE_MS && 0 <= bootAge &&
                bootAge * 1000 <= SAMPLE_MAX_AGE_MS && std::abs(wallAge - bootAge * 1000) <= 1000,
            "stale sample or clock discontinuity");
    require(sample.sensor == "cpu" || sample.sensor == "gpu",
            "invalid sensor identity/mode");
    require(sample.powerState == "active" || sample.powerState == "suspended" ||
                sample.powerState == "error", "invalid sensor identity/mode");
    if (!sensor.isEmpty()) require(sample.sensor == sensor, "sensor identity mismatch: sensor");
    if (generation) require(sample.generation == *generation, "sensor identity mismatch: generation");
    if (!session.isEmpty()) require(sample.session == session, "sensor identity mismatch: session");
}

void validateReferenceSnapshot(const ReferenceSnapshot &snapshot, double nowBoottimeSeconds,
                               qint64 nowUnixMs, bool requireMetadata, bool requireSamples) {
    validateCommonSnapshot(snapshot, nowBoottimeSeconds, nowUnixMs, requireMetadata, requireSamples);
    require(snapshot.cpuTempMilliC.has_value() && *snapshot.cpuTempMilliC >= 1 &&
                *snapshot.cpuTempMilliC <= 119999,
            "CPU temperature unavailable");
    if (snapshot.gpuState == "active") {
        if (!snapshot.gpuTempMilliC && snapshot.gpuPowerState == "D0")
            throw SensorUnavailable("active GPU temperature unavailable or stale");
        require(snapshot.gpuTempMilliC && *snapshot.gpuTempMilliC >= 1 &&
                    *snapshot.gpuTempMilliC <= 119999 && snapshot.gpuSampledAt.has_value() &&
                    finite(*snapshot.gpuSampledAt) && nowBoottimeSeconds >= *snapshot.gpuSampledAt &&
                    nowBoottimeSeconds - *snapshot.gpuSampledAt <= MAX_AGE_SECONDS &&
                    snapshot.gpuPowerState == "D0",
                "active GPU temperature unavailable or stale");
    } else if (snapshot.gpuState == "suspended") {
        require(snapshot.gpuPowerState == "D3cold" && !snapshot.gpuTempMilliC &&
                    !snapshot.gpuSampledAt.has_value(),
                "GPU sleep not confirmed or fabricated temperature");
    } else if (snapshot.gpuState == "error" && snapshot.gpuPowerState.isEmpty()) {
        throw SensorUnavailable("GPU power state unknown/transitional/error");
    } else {
        throw std::runtime_error("GPU power state unknown/transitional/error");
    }
    if (!snapshot.errors.isEmpty()) throw SensorUnavailable("snapshot reports errors");
    require(snapshot.errorsPresent && snapshot.errors.isEmpty(), "snapshot reports errors");
}

bool referenceCriticalTemperature(const ReferenceSnapshot &snapshot) {
    require(snapshot.cpuTempMilliC.has_value(), "CPU temperature unavailable");
    return *snapshot.cpuTempMilliC >= 90000 ||
           (snapshot.gpuState == "active" && snapshot.gpuTempMilliC && *snapshot.gpuTempMilliC >= 80000);
}

QString referenceRequestedCurve(const ReferenceSnapshot &snapshot) {
    if (snapshot.gpuState == "suspended" && snapshot.gpuPowerState == "D3cold") return "cpu";
    if (snapshot.gpuState == "active" && snapshot.gpuPowerState == "D0") return "max";
    throw std::runtime_error("unconfirmed GPU power mode");
}

SnapshotEpoch referenceEpoch(const ReferenceSnapshot &snapshot) {
    return {snapshot.samplerSession, snapshot.samplerGeneration, snapshot.gpuState};
}

} // namespace n14::reference