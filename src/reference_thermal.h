#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <stdexcept>

namespace n14::reference {

class SensorUnavailable : public std::runtime_error {
public:
    explicit SensorUnavailable(const QString &message)
        : std::runtime_error(message.toStdString()) {}
};

struct SampleMetadata {
    QString session;
    quint64 generation = 0;
    quint64 sampleId = 0;
    qint64 sampledAtUnixMs = 0;
    qint64 sampledAtMonotonicNs = 0;
    QString sensor;
    QString powerState;
};

struct ReferenceSnapshot {
    int schemaVersion = 0;
    QString source;
    std::optional<double> sampledAt;
    std::optional<qint64> sampledAtUnixMs;
    std::optional<qint64> sampledAtMonotonicNs;
    QString samplerSession;
    quint64 samplerGeneration = 0;
    quint64 sampleId = 0;
    bool samplerMetadataPresent = false;
    bool errorsPresent = false;
    QStringList errors;

    QString gpuState;
    QString gpuPowerState;
    std::optional<int> cpuTempMilliC;
    std::optional<int> gpuTempMilliC;
    std::optional<double> gpuSampledAt;
    std::optional<SampleMetadata> cpuSample;
    std::optional<SampleMetadata> gpuSample;
};

ReferenceSnapshot parseReferenceSnapshot(const QByteArray &json);
void validateReferenceSample(const SampleMetadata &sample, qint64 nowUnixMs,
                             double nowBoottimeSeconds, const QString &sensor = {},
                             std::optional<quint64> generation = {},
                             const QString &session = {});
void validateReferenceSnapshot(const ReferenceSnapshot &snapshot, double nowBoottimeSeconds,
                               qint64 nowUnixMs, bool requireMetadata = false,
                               bool requireSamples = false);

bool referenceCriticalTemperature(const ReferenceSnapshot &snapshot);
QString referenceRequestedCurve(const ReferenceSnapshot &snapshot);

struct SnapshotEpoch {
    QString samplerSession;
    quint64 samplerGeneration = 0;
    QString gpuState;
    friend bool operator==(const SnapshotEpoch &, const SnapshotEpoch &) = default;
};
SnapshotEpoch referenceEpoch(const ReferenceSnapshot &snapshot);

} // namespace n14::reference