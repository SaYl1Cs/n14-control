#include "core.h"
#include "reference_control.h"
#include "reference_standard.h"
#include "reference_thermal.h"
#include "reference_runtime.h"
#include "reference_ownership.h"
#include "reference_transaction.h"
#include "reference_broker.h"
#include "reference_json.h"
#include "reference_power.h"
#include "telemetry.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace n14;

class RecordingReadOnlyFileSystem final : public reference::EcReadOnlyFileSystem {
public:
    QHash<QString,reference::EcReadOnlyFileStatus> statuses;
    QHash<QString,QByteArray> contents;
    QHash<QString,QStringList> directories;
    mutable QStringList statusCalls;
    mutable QStringList readCalls;
    mutable QStringList listCalls;

    reference::EcReadOnlyFileStatus status(const QString &path) const override {
        statusCalls.append(path);
        return statuses.value(path);
    }
    QByteArray readFile(const QString &path, qsizetype maximumBytes, bool *ok) const override {
        readCalls.append(path);
        if (ok) *ok=false;
        if (!contents.contains(path) || contents.value(path).size() > maximumBytes) return {};
        if (ok) *ok=true;
        return contents.value(path);
    }
    QStringList listDirectory(const QString &path) const override {
        listCalls.append(path);
        return directories.value(path);
    }
};

class RecordingReadOnlyProcessSource final : public reference::EcReadOnlyProcessSource {
public:
    QStringList names;
    mutable QStringList roots;

    QStringList processNames(const QString &procRoot) const override {
        roots.append(procRoot);
        return names;
    }
};

class FixedReadOnlyIdentitySource final : public reference::EcReadOnlyIdentitySource {
public:
    reference::EcReadOnlyIdentityFacts facts;

    reference::EcReadOnlyIdentityFacts inspect() const override { return facts; }
};

class FixedIdentityReadTransport final : public reference::EcIdentityReadTransport {
public:
    int high = 0x55;
    int low = 0x71;
    bool available = true;

    bool readControllerByte(int index, int *value, QString *reason) const override {
        if (!available || (index != 0x20 && index != 0x21)) {
            if (reason) *reason = "fake identity transport unavailable";
            return false;
        }
        if (value) *value = index == 0x20 ? high : low;
        return true;
    }
};

class RecordingLockBackend final : public reference::EcOwnershipLockBackend {
public:
    mutable int openCalls = 0;
    mutable int lockCalls = 0;
    mutable int closeCalls = 0;
    bool contended = false;

    int openValidated(const QString &, const reference::EcOwnershipPolicy &,
                      QString *) const override {
        ++openCalls;
        return 42;
    }
    bool tryExclusiveNonBlocking(int, QString *reason) const override {
        ++lockCalls;
        if (contended) {
            if (reason) *reason="EC ownership lock is contended";
            return false;
        }
        return true;
    }
    void closeDescriptor(int) const override { ++closeCalls; }
};

class CoreTests : public QObject {
    Q_OBJECT
private:
    static reference::StandardFunction standardFunction(const QJsonObject &json) {
        reference::StandardFunction function;
        function.stepMinimum = json["step_min"].toInt();
        function.stepMaximum = json["step_max"].toInt();
        function.stepMinimumDecreasing = json["down_min"].toInt();
        function.stepMaximumDecreasing = json["down_max"].toInt();
        function.responseDelay = json["delay"].toInt();
        function.deviance = json["deviance"].toDouble();
        function.onlyDownward = json["only_downward"].toBool();
        function.thresholdHopping = json["hopping"].toBool();
        function.bypassMinimumAtExtremes = json["extremes"].toBool();
        return function;
    }
    static QJsonObject standardStateJson(const reference::StandardState &state) {
        QJsonArray temperatures, duties;
        for (double value : state.temperatureHistory) temperatures.append(value);
        for (int value : state.dutyHistory) duties.append(value);
        return {{"temperature_history", temperatures}, {"duty_history", duties},
                {"ideal_stack_size", state.idealStackSize},
                {"last_temperature", state.lastAppliedTemperature},
                {"last_duty", state.lastAppliedDuty ? QJsonValue(*state.lastAppliedDuty) : QJsonValue(QJsonValue::Null)},
                {"no_duty_count", state.noDutySetCount},
                {"maximum_no_duty_count", state.maximumNoDutySetCount}};
    }
    static void write(const QString &root,const QString &path,const QByteArray &data) {
        const QString p=root+path; QVERIFY(QDir().mkpath(QFileInfo(p).absolutePath()));
        QFile file(p); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data),data.size());
    }
    static QByteArray screenJson() {
        return R"({"outputs":[{"id":1,"name":"eDP-1","connected":true,"enabled":true,"currentModeId":"1","modes":[{"id":"1","size":{"width":2560,"height":1600},"refreshRate":165},{"id":"2","size":{"width":2560,"height":1600},"refreshRate":60.002},{"id":"3","size":{"width":1920,"height":1080},"refreshRate":60}]}]})";
    }
    static QByteArray referenceSnapshotJson(bool sleeping = false) {
        const QString session = "12345678-1234-4234-8234-123456789abc";
        const QString mode = sleeping ? "suspended" : "active";
        const QString power = sleeping ? "D3cold" : "D0";
        QJsonObject common{{"session", session}, {"generation", 1}, {"sample_id", 1},
                           {"sampled_at_unix_ms", 1800000000000LL},
                           {"sampled_at_monotonic_ns", 1000000000000LL}};
        QJsonObject cpuSample = common;
        cpuSample["sensor"] = "cpu"; cpuSample["power_state"] = mode;
        QJsonValue gpuSample = QJsonValue(QJsonValue::Null);
        if (!sleeping) {
            QJsonObject sample = common;
            sample["sensor"] = "gpu"; sample["power_state"] = mode;
            gpuSample = sample;
        }
        QJsonObject snapshot{
            {"schema_version", 2}, {"source", "ec0/io"}, {"sampled_at", 1000.0},
            {"sampled_at_unix_ms", 1800000000000LL},
            {"sampled_at_monotonic_ns", 1000000000000LL}, {"sampler_session", session},
            {"sampler_generation", 1}, {"sample_id", 1}, {"gpu_state", mode},
            {"gpu_power_state", power}, {"cpu_temp", 61000},
            {"gpu_temp", sleeping ? QJsonValue(QJsonValue::Null) : QJsonValue(55000)},
            {"gpu_sampled_at", sleeping ? QJsonValue(QJsonValue::Null) : QJsonValue(1000.0)},
            {"cpu_sample", cpuSample}, {"gpu_sample", gpuSample},
            {"errors", QJsonArray{}}};
        return QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    }
    static QByteArray versionedReferenceSnapshotJson(bool sleeping, const QString &session,
                                                     int generation, int sampleId,
                                                     double sampledAt, qint64 sampledAtUnixMs) {
        QJsonParseError error;
        auto object=QJsonDocument::fromJson(referenceSnapshotJson(sleeping), &error).object();
        Q_UNUSED(error);
        object["sampled_at"] = sampledAt;
        object["sampled_at_unix_ms"] = sampledAtUnixMs;
        object["sampled_at_monotonic_ns"] = static_cast<qint64>(sampledAt * 1e9);
        object["sampler_session"] = session;
        object["sampler_generation"] = generation;
        object["sample_id"] = sampleId;
        auto cpu=object["cpu_sample"].toObject();
        cpu["session"] = session; cpu["generation"] = generation; cpu["sample_id"] = sampleId;
        cpu["sampled_at_unix_ms"] = sampledAtUnixMs;
        cpu["sampled_at_monotonic_ns"] = static_cast<qint64>(sampledAt * 1e9);
        object["cpu_sample"] = cpu;
        if (!sleeping) {
            auto gpu=object["gpu_sample"].toObject();
            gpu["session"] = session; gpu["generation"] = generation; gpu["sample_id"] = sampleId;
            gpu["sampled_at_unix_ms"] = sampledAtUnixMs;
            gpu["sampled_at_monotonic_ns"] = static_cast<qint64>(sampledAt * 1e9);
            object["gpu_sample"] = gpu;
            object["gpu_sampled_at"] = sampledAt;
        }
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    static reference::RuntimeSnapshotFilePolicy snapshotFilePolicy(const QString &path) {
        reference::RuntimeSnapshotFilePolicy policy;
        policy.path=path;
        policy.requireOwner=false;
        policy.requireGroup=false;
        return policy;
    }
    static void writePrivateSnapshot(const QString &path, const QByteArray &data) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        QCOMPARE(file.write(data),data.size());
        file.close();
        QVERIFY(!file.isOpen());
        QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
    }
    static reference::EcOwnershipFacts validOwnershipFacts() {
        reference::EcOwnershipFacts facts;
        facts.runtimeDirectoryIsDirectory=true;
        facts.runtimeDirectoryNoSymlink=true;
        facts.runtimeDirectoryOwnerUid=0;
        facts.runtimeDirectoryGroupGid=0;
        facts.runtimeDirectoryMode=0700;
        facts.lockFileExists=true;
        facts.lockFileIsRegular=true;
        facts.lockFileNoSymlink=true;
        facts.lockFileOwnerUid=0;
        facts.lockFileGroupGid=0;
        facts.lockFileMode=0600;
        facts.lockFileLinkCount=1;
        facts.lockHeld=true;
        facts.sleepAllowed=true;
        facts.readOnlyTransportVerified=true;
        facts.vendor="COLORFUL";
        facts.model="N14 24";
        facts.bios="2.07";
        facts.ecVersion="2.0";
        facts.controllerId="55:71";
        facts.dsdtSha256="4aab3c89fdaac8ab9e20d31cff3fe0a18b83877442520eb294429ff9237bef06";
        facts.wmiGuid="3161c7c3-489f-4074-82ae-538101cce1c2";
        facts.acpiMethod="\\_SB.AMW0.WMBA";
        return facts;
    }
private slots:
    void standardGolden_data() {
        QTest::addColumn<QJsonObject>("scenario");
        QFile file(QFINDTESTDATA("fixtures/standard-golden.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const auto fixture = QJsonDocument::fromJson(file.readAll(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(fixture["schema"].toInt(), 1);
        QFile lockFile(QFINDTESTDATA("../reference/standard-source-lock.json"));
        QVERIFY(lockFile.open(QIODevice::ReadOnly));
        const auto lock = QJsonDocument::fromJson(lockFile.readAll()).object();
        QCOMPARE(fixture["upstream_commit"], lock["commit"]);
        QJsonObject hashes;
        const auto files = lock["files"].toObject();
        for (auto it = files.begin(); it != files.end(); ++it)
            hashes[it.key()] = it.value().toObject()["sha256"];
        QCOMPARE(fixture["source_sha256"].toObject(), hashes);
        const auto cases = fixture["cases"].toArray();
        QVERIFY(cases.size() >= 40);
        for (const auto &value : cases) {
            const auto scenario = value.toObject();
            QTest::newRow(qPrintable(scenario["name"].toString())) << scenario;
        }
    }
    void standardGolden() {
        using namespace n14::reference;
        QFETCH(QJsonObject, scenario);
        SpeedProfile graph;
        for (const auto &value : scenario["graph"].toArray()) {
            const auto point = value.toArray();
            graph.append({point[0].toDouble(), point[1].toInt()});
        }
        const double limit = scenario["temperature_limit"].toDouble();
        const auto normalized = normalizeSpeedProfile(graph, limit);
        QJsonArray normalizedJson;
        for (const auto &point : normalized)
            normalizedJson.append(QJsonArray{point.temperature, point.duty});
        const auto ticks = scenario["ticks"].toArray();
        QVERIFY(!ticks.isEmpty());
        const auto firstFunction = ticks.first().toObject()["function"].toObject();
        StandardGraphProcessor processor(normalized, limit, standardFunction(firstFunction),
                                         firstFunction["poll_rate"].toDouble());
        QVector<double> history;
        for (const auto &value : scenario["initial_history"].toArray()) history.append(value.toDouble());
        int cycle = 0;
        for (const auto &value : ticks) {
            const auto tick = value.toObject();
            const auto function = tick["function"].toObject();
            processor.configure(standardFunction(function), function["poll_rate"].toDouble());
            history.append(tick["input"].toDouble());
            const auto result = processor.update(history);
            const QJsonObject actual{
                {"duty", result.duty ? QJsonValue(*result.duty) : QJsonValue(QJsonValue::Null)},
                {"temperature", result.temperature ? QJsonValue(*result.temperature) : QJsonValue(QJsonValue::Null)},
                {"latch", result.safetyLatchTriggered}, {"state", standardStateJson(processor.state())},
                {"normalized", normalizedJson}};
            const auto expected = tick["expected"].toObject();
            if (actual != expected) {
                const QByteArray message = "Cycle " + QByteArray::number(cycle) + "\nactual: " +
                    QJsonDocument(actual).toJson(QJsonDocument::Compact) + "\nexpected: " +
                    QJsonDocument(expected).toJson(QJsonDocument::Compact);
                QFAIL(message.constData());
            }
            ++cycle;
        }
    }
    void standardDefaultsMatchInstalledFunctions() {
        using namespace n14::reference;
        const auto cpu = defaultCpuStandardFunction();
        const auto gpu = defaultGpuStandardFunction();
        QCOMPARE(cpu.stepMinimum, 1); QCOMPARE(cpu.stepMaximum, 100);
        QCOMPARE(cpu.stepMinimumDecreasing, 0); QCOMPARE(cpu.stepMaximumDecreasing, 0);
        QCOMPARE(cpu.responseDelay, 0); QCOMPARE(cpu.deviance, 5.0);
        QVERIFY(!cpu.onlyDownward); QVERIFY(cpu.thresholdHopping);
        QVERIFY(!cpu.bypassMinimumAtExtremes); QCOMPARE(gpu.deviance, 1.0);
        QVERIFY(validateStandardFunction(cpu).isEmpty());
        QVERIFY(validateStandardFunction(gpu).isEmpty());
    }
    void standardRejectsMissingOrInvalidInputWithoutAdvancingState() {
        using namespace n14::reference;
        StandardGraphProcessor processor(defaultCoolerControlGraphs().cpu, 100, defaultCpuStandardFunction());
        QCOMPARE(processor.update({20}).duty, std::optional<int>(0));
        const auto before = standardStateJson(processor.state());
        for (const auto &history : QVector<QVector<double>>{{}, {std::numeric_limits<double>::quiet_NaN()},
                {std::numeric_limits<double>::infinity()}, {20, std::numeric_limits<double>::quiet_NaN(), 60}}) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, processor.update(history));
            QCOMPARE(standardStateJson(processor.state()), before);
        }
        QCOMPARE(processor.state().lastAppliedDuty, std::optional<int>(0));
    }
    void standardConfigurationValidationIsAtomic() {
        using namespace n14::reference;
        const SpeedProfile graph{{0, 0}, {100, 100}};
        const auto defaults = defaultCpuStandardFunction();
        StandardGraphProcessor processor(graph, 100, defaults);
        processor.update({40});
        const auto before = standardStateJson(processor.state());
        QVector<StandardFunction> invalid;
        for (int index = 0; index < 7; ++index) {
            auto function = defaults;
            if (index == 0) function.stepMinimum = -1;
            if (index == 1) function.stepMaximum = 101;
            if (index == 2) function.stepMinimumDecreasing = 101;
            if (index == 3) function.stepMaximumDecreasing = -1;
            if (index == 4) function.responseDelay = 256;
            if (index == 5) function.deviance = -0.1;
            if (index == 6) function.deviance = std::numeric_limits<double>::quiet_NaN();
            invalid.append(function);
        }
        for (const auto &function : invalid) {
            QVERIFY(!validateStandardFunction(function).isEmpty());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, processor.configure(function, 1));
            QCOMPARE(standardStateJson(processor.state()), before);
        }
        for (double poll : {0.0, -1.0, std::numeric_limits<double>::infinity()})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, processor.configure(defaults, poll));
        QVERIFY(!processor.update({40}).duty); // rejected configure did not change settings
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, StandardGraphProcessor({}, 100, defaults));
    }
    void standardStatePersistsAcrossCopiesAndCanReset() {
        using namespace n14::reference;
        StandardGraphProcessor processor(defaultCoolerControlGraphs().cpu, 100, defaultCpuStandardFunction());
        processor.update({20});
        auto copy = processor;
        QCOMPARE(copy.update({80}).duty, processor.update({80}).duty);
        QCOMPARE(standardStateJson(copy.state()), standardStateJson(processor.state()));
        copy.reset();
        QCOMPARE(standardStateJson(copy.state()), standardStateJson(StandardState{}));
        QCOMPARE(copy.update({20}).duty, std::optional<int>(0));
        QVERIFY(processor.state().lastAppliedDuty != copy.state().lastAppliedDuty);
    }
    void publishedCurvesAndExactSchema() {
        using namespace n14::reference;
        const auto curves = defaultFanCurves();
        QCOMPARE(curves.cpu.size(), 5);
        QCOMPARE(curves.gpu.size(), 5);
        QCOMPARE(curves.cpu[0].upThreshold, 45.0);
        QCOMPARE(curves.cpu[0].downThreshold, 0.0);
        QCOMPARE(curves.cpu[0].fanSpeed, 40.0);
        QCOMPARE(curves.gpu.last().fanSpeed, 100.0);
        QVERIFY(validateFanCurves(curves).isEmpty());
        const auto json = fanCurvesJson(curves);
        const auto roundTrip = parseFanCurves(json);
        QCOMPARE(fanCurvesJson(roundTrip), json);
    }
    void coolerControlGraphsUseCurrentProfilesAndAllowZeroDuty() {
        using namespace n14::reference;
        const auto graphs = defaultCoolerControlGraphs();
        QVERIFY(validateCoolerControlGraphs(graphs).isEmpty());
        QCOMPARE(graphs.cpu.size(), 6);
        QCOMPARE(graphs.cpu[0].temperature, 0.0);
        QCOMPARE(graphs.cpu[0].duty, 0);
        QCOMPARE(graphs.cpu[3].temperature, 75.0);
        QCOMPARE(graphs.cpu[3].duty, 54);
        QCOMPARE(graphs.gpu.size(), 5);
        QCOMPARE(graphs.gpu[2].temperature, 65.0);
        QCOMPARE(graphs.gpu[2].duty, 55);

        const ThermalInput input{80.0, 80.0, GpuState::Active};
        const auto demand = curveDemand(graphs.cpu, graphs.gpu, input, 100.0, 100.0);
        QVERIFY(demand);
        QCOMPARE(*demand, 100);

        const SpeedProfile zeroGraph{{0.0, 0}, {90.0, 0}};
        QVERIFY(validateCoolerControlGraph(zeroGraph).isEmpty());
        const ThermalInput zeroInput{20.0, 20.0, GpuState::Active};
        const auto zeroDemand = curveDemand(zeroGraph, zeroGraph, zeroInput, 100.0, 100.0);
        QVERIFY(zeroDemand);
        QCOMPARE(*zeroDemand, 0);
    }
    void curveValidationMatchesPublishedBoundaries() {
        using namespace n14::reference;
        auto curves = defaultFanCurves();
        curves.cpu[0].downThreshold = 1.0;
        QVERIFY(!validateFanCurves(curves).isEmpty());
        curves = defaultFanCurves();
        curves.gpu[1].fanSpeed = 39.99;
        QVERIFY(!validateFanCurves(curves).isEmpty());
        curves = defaultFanCurves();
        curves.cpu.last().fanSpeed = 99.0;
        QVERIFY(!validateFanCurves(curves).isEmpty());
        curves = defaultFanCurves();
        curves.cpu[0].upThreshold = 45.5;
        curves.cpu[1].fanSpeed = 40.0;
        QVERIFY(validateFanCurves(curves).isEmpty());
        QJsonObject row{{"UpThreshold", 45.0}, {"DownThreshold", 0.0},
                        {"FanSpeed", 40.0}, {"unexpected", 1.0}};
        QJsonArray rows{row, QJsonObject{{"UpThreshold", 90.0}, {"DownThreshold", 75.0},
                                         {"FanSpeed", 100.0}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, parseThresholdCurve(rows, CPU_LIMIT_C));
    }
    void thresholdSelectionIsStatefulAndHysteretic() {
        using namespace n14::reference;
        const auto curves = defaultFanCurves();
        ThresholdSelector selector(curves.cpu);
        QCOMPARE(selector.update(44.0), 40.0);
        QCOMPARE(selector.update(45.0), 50.0);
        QCOMPARE(selector.update(59.0), 50.0);
        QCOMPARE(selector.update(60.0), 65.0);
        QCOMPARE(selector.update(69.0), 65.0);
        QCOMPARE(selector.update(70.0), 80.0);
        QCOMPARE(selector.update(80.0), 100.0);
        // Published semantics use <= for DownThreshold, so 75°C leaves the
        // final row exactly at its configured down threshold.
        QCOMPARE(selector.update(75.0), 80.0);
        QCOMPARE(selector.update(74.0), 80.0);
        QCOMPARE(selector.update(65.0), 65.0);
        QCOMPARE(selector.update(55.0), 50.0);
        QCOMPARE(selector.update(40.0), 40.0);
        QCOMPARE(selector.row(), 0);
    }
    void graphNormalizationInterpolationAndRounding() {
        using namespace n14::reference;
        const SpeedProfile profile{{20.0, 30}, {90.0, 100}};
        const auto normalized = normalizeSpeedProfile(profile, 100.0);
        QCOMPARE(normalized.size(), 2);
        QCOMPARE(interpolateSpeedProfile(normalized, 20.0), 30);
        QCOMPARE(interpolateSpeedProfile(normalized, 63.0), 73);
        QCOMPARE(interpolateSpeedProfile(normalized, 110.0), 100);
        const SpeedProfile duplicate{{0.0, 20}, {50.0, 10}, {50.0, 60}, {80.0, 120}};
        const auto cleaned = normalizeSpeedProfile(duplicate, 100.0);
        QCOMPARE(cleaned.size(), 3);
        QCOMPARE(cleaned[1].duty, 60);
        QCOMPARE(cleaned.last().duty, 100);
    }
    void gpuSleepAndMissingDataHaveNoSubstitution() {
        using namespace n14::reference;
        const SpeedProfile cpu{{20.0, 30}, {90.0, 100}};
        const SpeedProfile gpu{{20.0, 20}, {90.0, 80}};
        ThermalInput input{63.0, std::nullopt, GpuState::Suspended};
        auto demand = curveDemand(cpu, gpu, input, 100.0, 100.0);
        QVERIFY(demand);
        QCOMPARE(*demand, 73);
        input.gpuState = GpuState::Active;
        QVERIFY(!curveDemand(cpu, gpu, input, 100.0, 100.0));
        input.gpuC = 63.0;
        demand = curveDemand(cpu, gpu, input, 100.0, 100.0);
        QVERIFY(demand);
        QCOMPARE(*demand, 73);
        input.gpuState = GpuState::Unknown;
        QVERIFY(!curveDemand(cpu, gpu, input, 100.0, 100.0));
        input.gpuState = GpuState::Suspended;
        input.cpuC.reset();
        QVERIFY(!curveDemand(cpu, gpu, input, 100.0, 100.0));
    }
    void ecAndKeylightConversionsUsePythonTieToEven() {
        using namespace n14::reference;
        QCOMPARE(fanDutyToRaw(0), 1);
        QCOMPARE(fanDutyToRaw(50), 35);
        QCOMPARE(fanDutyToRaw(100), 70);
        QCOMPARE(fanRawToDuty(1), 0);
        QCOMPARE(fanRawToDuty(0), 0);
        QCOMPARE(fanRawToDuty(70), 100);
        QCOMPARE(keylightPercentToRaw(0), 175);
        QCOMPARE(keylightPercentToRaw(50), 88);
        QCOMPARE(keylightPercentToRaw(100), 0);
        QCOMPARE(keylightRawToPercent(175), 0);
        QCOMPARE(keylightRawToPercent(110), 38);
        QCOMPARE(keylightRawToPercent(0), 100);
        for (const auto invalid : {-1, 101}) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, fanDutyToRaw(invalid));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, keylightPercentToRaw(invalid));
        }
    }
    void thermalSnapshotValidationPreservesModesAndProvenance() {
        using namespace n14::reference;
        const auto active = parseReferenceSnapshot(referenceSnapshotJson());
        validateReferenceSnapshot(active, 1001.0, 1800000001000LL, true, true);
        QVERIFY(!referenceCriticalTemperature(active));
        QCOMPARE(referenceRequestedCurve(active), QString("max"));
        const auto epoch = referenceEpoch(active);
        QCOMPARE(epoch.samplerSession, active.samplerSession);
        QCOMPARE(epoch.samplerGeneration, quint64(1));
        QCOMPARE(epoch.gpuState, QString("active"));

        const auto sleeping = parseReferenceSnapshot(referenceSnapshotJson(true));
        validateReferenceSnapshot(sleeping, 1001.0, 1800000001000LL, true, true);
        QCOMPARE(referenceRequestedCurve(sleeping), QString("cpu"));
        QVERIFY(!sleeping.gpuTempMilliC.has_value());

        auto stale = active;
        stale.sampledAt = 994.0;
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,
                                 validateReferenceSnapshot(stale, 1000.0, 1800000000000LL, true, true));
        auto future = active;
        future.sampledAt = 1002.0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 validateReferenceSnapshot(future, 1001.0, 1800000001000LL, true, true));
    }
    void thermalSnapshotRejectsUnconfirmedGpuAndErrors() {
        using namespace n14::reference;
        auto active = parseReferenceSnapshot(referenceSnapshotJson());
        active.gpuTempMilliC.reset();
        active.gpuSample.reset();
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,
                                 validateReferenceSnapshot(active, 1001.0, 1800000001000LL, true, true));
        auto error = parseReferenceSnapshot(referenceSnapshotJson());
        error.gpuState = "error";
        error.gpuPowerState.clear();
        error.gpuTempMilliC.reset();
        error.gpuSample.reset();
        error.cpuSample->powerState = "error";
        error.errors.append("GPU EC/PM read: offline");
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,
                                 validateReferenceSnapshot(error, 1001.0, 1800000001000LL, true, true));
        auto badSleep = parseReferenceSnapshot(referenceSnapshotJson(true));
        badSleep.gpuTempMilliC = 55000;
        badSleep.gpuSample = SampleMetadata{badSleep.samplerSession, 1, 1, 1800000000000LL,
                                            1000000000000LL, "gpu", "suspended"};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 validateReferenceSnapshot(badSleep, 1001.0, 1800000001000LL, true, true));
    }
    void duplicateJsonMembersAreRejectedBeforeQtLastValueWins() {
        using namespace n14::reference;
        QVERIFY(hasDuplicateJsonMembers("{\"schema_version\":2,\"schema_version\":2}"));
        QVERIFY(hasDuplicateJsonMembers("{\"nested\":{\"x\":1,\"x\":2}}"));
        QVERIFY(!hasDuplicateJsonMembers("{\"a\":1,\"nested\":{\"x\":1,\"y\":2}}"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 parseReferenceSnapshot("{\"schema_version\":2,\"schema_version\":2}"));
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path=directory.path()+"/config.json";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{\"schema\":3,\"schema\":3}")>0);
        file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::load(path));
    }
    void liveGpuPowerSourceUsesOnlyStableNvidiaDisplayState() {
        using namespace n14::reference;
        QTemporaryDir root; QVERIFY(root.isValid());
        write(root.path(),"/0000:01:00.0/vendor","0x10de\n");
        write(root.path(),"/0000:01:00.0/class","0x030000\n");
        write(root.path(),"/0000:01:00.0/power/runtime_status","suspended\n");
        write(root.path(),"/0000:01:00.0/power_state","D3cold\n");
        PosixGpuPowerStateSource source(root.path());
        QString reason;
        const auto state=source.read(&reason);
        QVERIFY2(state.has_value(),qPrintable(reason));
        QCOMPARE(state->first,QString("suspended"));
        QCOMPARE(state->second,QString("D3cold"));
        write(root.path(),"/0000:02:00.0/vendor","0x10de\n");
        write(root.path(),"/0000:02:00.0/class","0x030000\n");
        QVERIFY(!source.read(&reason));
        QVERIFY(reason.contains("exactly one"));
    }
    void runtimeSamplerAcceptsFreshNondecreasingSamples() {
        using namespace n14::reference;
        const QString session="12345678-1234-4234-8234-123456789abc";
        RuntimeThermalSampler sampler;
        const auto first=sampler.accept(referenceSnapshotJson(), 1800000001000LL, 1001.0);
        QCOMPARE(first.cpuC,61.0); QCOMPARE(*first.gpuC,55.0);
        QCOMPARE(first.gpuState,GpuState::Active); QCOMPARE(first.sampleId,quint64(1));
        QCOMPARE(first.controlInput().gpuState,GpuState::Active);
        QVERIFY(sampler.hasAcceptedSample()); QCOMPARE(*sampler.sampleId(),quint64(1));
        const auto secondJson=versionedReferenceSnapshotJson(false,session,1,2,1000.5,1800000000500LL);
        const auto second=sampler.accept(secondJson,1800000001500LL,1001.5);
        QCOMPARE(second.sampleId,quint64(2));
        QCOMPARE(sampler.epoch()->samplerGeneration,quint64(1));
        const auto repeated=sampler.accept(secondJson,1800000001500LL,1001.5);
        QCOMPARE(repeated.sampleId,quint64(2));
        QCOMPARE(*sampler.sampleId(),quint64(2));
        const auto older=versionedReferenceSnapshotJson(false,session,1,1,1000.4,1800000000400LL);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,
                                 sampler.accept(older,1800000001500LL,1001.5));
    }
    void runtimeSamplerAllowsNewSessionButRejectsGenerationRollback() {
        using namespace n14::reference;
        const QString session="12345678-1234-4234-8234-123456789abc";
        const QString newSession="87654321-4321-4432-8234-cba987654321";
        RuntimeThermalSampler sampler;
        sampler.accept(referenceSnapshotJson(),1800000001000LL,1001.0);
        sampler.accept(versionedReferenceSnapshotJson(false,session,2,2,1000.5,1800000000500LL),
                       1800000001500LL,1001.5);
        const auto rollback=versionedReferenceSnapshotJson(false,session,1,3,1000.6,1800000000600LL);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 sampler.accept(rollback,1800000001600LL,1001.6));
        const auto restarted=versionedReferenceSnapshotJson(false,newSession,1,1,1000.7,1800000000700LL);
        const auto accepted=sampler.accept(restarted,1800000001700LL,1001.7);
        QCOMPARE(accepted.epoch.samplerSession,newSession);
        QCOMPARE(accepted.sampleId,quint64(1));
    }
    void runtimeSamplerPreservesSuspendedGpuSemanticsAndFailureState() {
        using namespace n14::reference;
        RuntimeThermalSampler suspendedSampler;
        const auto suspended=suspendedSampler.accept(referenceSnapshotJson(true),
                                                     1800000001000LL,1001.0);
        QCOMPARE(suspended.gpuState,GpuState::Suspended);
        QVERIFY(!suspended.gpuC.has_value());
        QVERIFY(!suspended.controlInput().gpuC.has_value());

        RuntimeThermalSampler sampler;
        sampler.accept(referenceSnapshotJson(),1800000001000LL,1001.0);
        auto invalid=parseReferenceSnapshot(referenceSnapshotJson());
        invalid.cpuTempMilliC.reset();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 sampler.accept(invalid,1800000001000LL,1001.0));
        QCOMPARE(*sampler.sampleId(),quint64(1));
    }
    void runtimeSnapshotProviderReadsPrivateAtomicReplacement() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        const QString session="12345678-1234-4234-8234-123456789abc";
        const QByteArray first=referenceSnapshotJson();
        const QByteArray second=versionedReferenceSnapshotJson(false,session,1,2,1000.5,
                                                                1800000000500LL);
        writePrivateSnapshot(path,first);
        RuntimeSnapshotProvider provider(snapshotFilePolicy(path));
        QCOMPARE(provider.read(),first);
        RuntimeThermalSampler sampler;
        QCOMPARE(provider.readAndAccept(sampler,1800000001000LL,1001.0).sampleId,quint64(1));

        const QString replacement=directory.path()+"/temperatures.json.tmp";
        writePrivateSnapshot(replacement,second);
        QVERIFY(::rename(QFile::encodeName(replacement).constData(),
                         QFile::encodeName(path).constData()) == 0);
        QCOMPARE(provider.read(),second);
        QCOMPARE(provider.readAndAccept(sampler,1800000001500LL,1001.5).sampleId,quint64(2));
    }
    void runtimeSnapshotProviderRejectsMissingSymlinkAndNonRegularFiles() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        const QString target=directory.path()+"/target.json";
        const auto policy=snapshotFilePolicy(path);
        RuntimeSnapshotProvider missing(policy);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,missing.read());
        auto traversal=policy;
        traversal.path=directory.path()+"/../temperatures.json";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,RuntimeSnapshotProvider invalid(traversal));

        writePrivateSnapshot(target,referenceSnapshotJson());
        QVERIFY(QFile::link(target,path));
        RuntimeSnapshotProvider symlink(policy);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,symlink.read());

        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkpath(path));
        RuntimeSnapshotProvider directoryProvider(policy);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,directoryProvider.read());
    }
    void runtimeSnapshotProviderRejectsPermissionsAndSize() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        writePrivateSnapshot(path,referenceSnapshotJson());
        QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ReadGroup));
        RuntimeSnapshotProvider permissionProvider(snapshotFilePolicy(path));
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,permissionProvider.read());

        auto ownerPolicy=snapshotFilePolicy(path);
        ownerPolicy.requireOwner=true;
        ownerPolicy.requiredOwnerUid=static_cast<quint32>(::getuid()) == 0 ? 1 : 0;
        RuntimeSnapshotProvider ownerProvider(ownerPolicy);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,ownerProvider.read());

        auto groupPolicy=snapshotFilePolicy(path);
        groupPolicy.requireGroup=true;
        groupPolicy.requiredGroupGid=static_cast<quint32>(::getgid()) == 0 ? 1 : 0;
        RuntimeSnapshotProvider groupProvider(groupPolicy);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,groupProvider.read());

        QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
        RuntimeSnapshotFilePolicy limited=snapshotFilePolicy(path);
        limited.maximumBytes=32;
        RuntimeSnapshotProvider sizeProvider(limited);
        QVERIFY_THROWS_EXCEPTION(SensorUnavailable,sizeProvider.read());
    }
    void runtimeSnapshotProviderPassesTruncatedDataToGate() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        writePrivateSnapshot(path,"{\"schema_version\":2,\"source\":");
        RuntimeSnapshotProvider provider(snapshotFilePolicy(path));
        RuntimeThermalSampler sampler;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 provider.readAndAccept(sampler,1800000001000LL,1001.0));
        QVERIFY(!sampler.hasAcceptedSample());
    }
    void runtimeSnapshotProviderAllowsRepeatedFreshSnapshotAfterReplacement() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        const QByteArray snapshot=referenceSnapshotJson();
        writePrivateSnapshot(path,snapshot);
        RuntimeSnapshotProvider provider(snapshotFilePolicy(path));
        RuntimeThermalSampler sampler;
        provider.readAndAccept(sampler,1800000001000LL,1001.0);
        const QString replacement=directory.path()+"/replay.tmp";
        writePrivateSnapshot(replacement,snapshot);
        QVERIFY(::rename(QFile::encodeName(replacement).constData(),
                         QFile::encodeName(path).constData()) == 0);
        const auto repeated=provider.readAndAccept(sampler,1800000001000LL,1001.0);
        QCOMPARE(repeated.sampleId,quint64(1));
        QCOMPARE(*sampler.sampleId(),quint64(1));
    }
    void runtimeSamplerLoopFailsClosedOnTimeoutAndRecovers() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        const QString session="12345678-1234-4234-8234-123456789abc";
        writePrivateSnapshot(path,referenceSnapshotJson());
        qint64 nowUnixMs=1800000001000LL;
        double nowBoottimeSeconds=1001.0;
        RuntimeSamplerClock clock{
            [&] { return nowUnixMs; },
            [&] { return nowBoottimeSeconds; }
        };
        RuntimeThermalSamplerLoop loop(RuntimeSnapshotProvider(snapshotFilePolicy(path)),
                                       1000,500,clock);
        QSignalSpy accepted(&loop,&RuntimeThermalSamplerLoop::sampleAccepted);
        QSignalSpy unavailable(&loop,&RuntimeThermalSamplerLoop::sampleUnavailable);
        QSignalSpy freshness(&loop,&RuntimeThermalSamplerLoop::freshnessChanged);

        loop.start();
        QCOMPARE(accepted.count(),1);
        QCOMPARE(loop.acceptedSampleCount(),quint64(1));
        QVERIFY(loop.hasFreshSample());
        QCOMPARE(loop.latestSample()->sampleId,quint64(1));

        nowUnixMs+=200; nowBoottimeSeconds=1001.2;
        QVERIFY(loop.pollOnce());
        QVERIFY(loop.hasFreshSample());
        QCOMPARE(loop.acceptedSampleCount(),quint64(1));
        QCOMPARE(unavailable.count(),0);

        nowUnixMs+=400; nowBoottimeSeconds=1001.6;
        QVERIFY(!loop.pollOnce());
        QVERIFY(!loop.hasFreshSample());
        QVERIFY(!loop.latestSample().has_value());
        QCOMPARE(freshness.count(),2);

        const QString replacement=directory.path()+"/next.tmp";
        writePrivateSnapshot(replacement,versionedReferenceSnapshotJson(false,session,1,2,
                                                                         1000.5,1800000000500LL));
        QVERIFY(::rename(QFile::encodeName(replacement).constData(),
                         QFile::encodeName(path).constData()) == 0);
        nowUnixMs+=100; nowBoottimeSeconds=1001.7;
        QVERIFY(loop.pollOnce());
        QVERIFY(loop.hasFreshSample());
        QCOMPARE(loop.latestSample()->sampleId,quint64(2));
        QCOMPARE(loop.acceptedSampleCount(),quint64(2));
        QCOMPARE(freshness.count(),3);
        loop.stop();
    }
    void runtimeSamplerLoopLifecycleAndProducerRestart() {
        using namespace n14::reference;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path=directory.path()+"/temperatures.json";
        const QString newSession="87654321-4321-4432-8234-cba987654321";
        writePrivateSnapshot(path,referenceSnapshotJson());
        qint64 nowUnixMs=1800000001000LL;
        double nowBoottimeSeconds=1001.0;
        RuntimeSamplerClock clock{
            [&] { return nowUnixMs; },
            [&] { return nowBoottimeSeconds; }
        };
        RuntimeThermalSamplerLoop loop(RuntimeSnapshotProvider(snapshotFilePolicy(path)),
                                       1,5000,clock);
        QSignalSpy accepted(&loop,&RuntimeThermalSamplerLoop::sampleAccepted);

        loop.start();
        QCOMPARE(accepted.count(),1);
        const QString replacement=directory.path()+"/restart.tmp";
        writePrivateSnapshot(replacement,versionedReferenceSnapshotJson(false,newSession,1,1,
                                                                         1001.1,1800000001100LL));
        QVERIFY(::rename(QFile::encodeName(replacement).constData(),
                         QFile::encodeName(path).constData()) == 0);
        nowUnixMs=1800000001200LL; nowBoottimeSeconds=1001.2;
        QTest::qWait(15);
        QVERIFY(accepted.count()>=2);
        QVERIFY(loop.isRunning());
        QCOMPARE(loop.latestSample()->epoch.samplerSession,newSession);

        loop.stop();
        QVERIFY(!loop.isRunning());
        QVERIFY(!loop.hasFreshSample());
        QVERIFY(!loop.latestSample().has_value());
        const int acceptedBeforeRestart=accepted.count();
        const quint64 acceptedSamplesBeforeRestart=loop.acceptedSampleCount();
        loop.start();
        QCOMPARE(accepted.count(),acceptedBeforeRestart+1);
        QCOMPARE(loop.latestSample()->sampleId,quint64(1));
        QCOMPARE(loop.acceptedSampleCount(),acceptedSamplesBeforeRestart+1);
        loop.stop();
    }
    void ecOwnershipPreflightAcceptsOnlyCompleteTrustedFacts() {
        using namespace n14::reference;
        const auto decision=evaluateEcOwnership(validOwnershipFacts());
        QVERIFY(decision.allowed);
        QVERIFY(decision.reason.isEmpty());

        FakeAcpiEcTransport transport;
        QVERIFY(transport.open(decision));
        transport.seed("fan_target",42);
        QCOMPARE(transport.read("fan_target"),42);
        transport.write("fan_target",43);
        QCOMPARE(transport.events().size(),3);
        QCOMPARE(transport.events().at(0).operation,QString("open"));
        QCOMPARE(transport.events().at(1).operation,QString("read"));
        QCOMPARE(transport.events().at(2).operation,QString("write"));
        QCOMPARE(transport.events().at(2).value,43);
        transport.close();
        QVERIFY(!transport.isOpen());
    }
    void ecOwnershipPreflightRejectsLockIdentityAndConflictFailures() {
        using namespace n14::reference;
        auto facts=validOwnershipFacts();
        facts.lockHeld=false;
        auto decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("lock"));

        facts=validOwnershipFacts(); facts.lockFileNoSymlink=false;
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("regular file"));
        facts=validOwnershipFacts(); facts.lockFileOwnerUid=1000;
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("owner/group"));
        facts=validOwnershipFacts(); facts.lockFileMode=0644;
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("mode"));
        facts=validOwnershipFacts(); facts.competingOwners={"coolercontrold"};
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("competing"));
        facts=validOwnershipFacts(); facts.model="OTHER MODEL";
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("identity"));
        facts=validOwnershipFacts(); facts.acpiMethod="\\_SB.Other.Method";
        decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed); QVERIFY(decision.reason.contains("ACPI"));
    }
    void failedEcOwnershipPreflightCannotOpenFakeTransport() {
        using namespace n14::reference;
        auto facts=validOwnershipFacts(); facts.readOnlyTransportVerified=false;
        const auto decision=evaluateEcOwnership(facts);
        QVERIFY(!decision.allowed);
        FakeAcpiEcTransport transport;
        QVERIFY(!transport.open(decision));
        QVERIFY(!transport.isOpen());
        QVERIFY(transport.events().isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,transport.write("fan_target",42));
        QVERIFY(transport.events().isEmpty());
    }
    void ecReadOnlyProbeCollectsFactsWithoutGrantingOwnershipOrWriting() {
        using namespace n14::reference;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString sys=root.path()+"/sys";
        const QString proc=root.path()+"/proc";
        const QString runtime=root.path()+"/run/colorful-n14-control";
        const QString lock=runtime+"/ec-owner.lock";
        QVERIFY(QDir().mkpath(sys+"/class/dmi/id"));
        QVERIFY(QDir().mkpath(sys+"/firmware/acpi/tables"));
        QVERIFY(QDir().mkpath(sys+"/bus/wmi/devices/3161c7c3-489f-4074-82ae-538101cce1c2-0"));
        QVERIFY(QDir().mkpath(proc+"/acpi"));
        QVERIFY(QDir().mkpath(proc+"/123"));
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));
        write(root.path(),"/sys/class/dmi/id/sys_vendor","COLORFUL\n");
        write(root.path(),"/sys/class/dmi/id/product_name","N14 24\n");
        write(root.path(),"/sys/class/dmi/id/bios_version","2.07\n");
        write(root.path(),"/sys/class/dmi/id/ec_firmware_release","2.0\n");
        write(root.path(),"/sys/firmware/acpi/tables/DSDT","dsdt");
        write(root.path(),"/proc/acpi/call","read-only endpoint fixture");
        write(root.path(),"/proc/123/comm","nbfc_service\n");
        writePrivateSnapshot(lock,"lock fixture");

        EcReadOnlyProbePaths paths;
        paths.sysRoot=sys; paths.procRoot=proc; paths.runtimeDirectory=runtime;
        paths.lockPath=lock; paths.acpiCallPath=proc+"/acpi/call"; paths.sleepAllowed=true;
        EcReadOnlyProbe probe(paths);
        const auto before=QFile(lock).size();
        auto facts=probe.inspect();
        QCOMPARE(QFile(lock).size(),before);
        QVERIFY(facts.runtimeDirectoryIsDirectory);
        QVERIFY(facts.runtimeDirectoryNoSymlink);
        QVERIFY(facts.runtimeDirectoryQueryAccessible);
        QVERIFY(facts.lockFileExists);
        QVERIFY(facts.lockFileQueryAccessible);
        QVERIFY(!facts.lockHeld);
        QVERIFY(facts.acpiEndpointPresent);
        QVERIFY(facts.acpiEndpointQueryAccessible);
        QVERIFY(facts.dsdtReadable);
        QVERIFY(!facts.readOnlyTransportVerified);
        QVERIFY(facts.competingOwners.contains("nbfc_service"));
        QCOMPARE(facts.vendor,QString("COLORFUL"));
        QCOMPARE(facts.model,QString("N14 24"));
        QCOMPARE(facts.bios,QString("2.07"));
        QCOMPARE(facts.dmiEcVersion,QString("2.0"));
        QVERIFY(facts.ecVersion.isEmpty());
        QVERIFY(facts.controllerId.isEmpty());
        QCOMPARE(facts.dsdtSha256,QString::fromLatin1(
            QCryptographicHash::hash("dsdt",QCryptographicHash::Sha256).toHex()));
        QCOMPARE(facts.wmiGuid,QString("3161c7c3-489f-4074-82ae-538101cce1c2"));

        const QString runtimeLink=root.path()+"/run-link";
        QVERIFY(QFile::link(runtime,runtimeLink));
        paths.runtimeDirectory=runtimeLink;
        const auto linkFacts=EcReadOnlyProbe(paths).inspect();
        QVERIFY(!linkFacts.runtimeDirectoryIsDirectory);
        QVERIFY(!linkFacts.runtimeDirectoryNoSymlink);
        QCOMPARE(QFile(lock).size(),before);

        const QString identityTarget=root.path()+"/identity-target";
        const QString identityLink=root.path()+"/identity-link";
        write(root.path(),"/identity-target","identity");
        QVERIFY(QFile::link(identityTarget,identityLink));
        PosixEcReadOnlyFileSystem fileSystem;
        bool readOk=false;
        QVERIFY(fileSystem.readFile(identityLink,64,&readOk).isEmpty());
        QVERIFY(!readOk);

        QTemporaryDir inaccessibleRoot;
        QVERIFY(inaccessibleRoot.isValid());
        const QString inaccessibleDirectory=inaccessibleRoot.path()+"/private";
        QVERIFY(QDir().mkpath(inaccessibleDirectory));
        QVERIFY(QFile::setPermissions(inaccessibleDirectory,QFileDevice::Permissions{}));
        const auto inaccessibleStatus=fileSystem.status(inaccessibleDirectory+"/child");
        QVERIFY(!inaccessibleStatus.exists);
        QVERIFY(!inaccessibleStatus.queryAccessible);
        QVERIFY(QFile::setPermissions(inaccessibleDirectory,
                                      QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));

        auto noLockPaths=paths;
        noLockPaths.runtimeDirectory=runtime;
        noLockPaths.lockPath=runtime+"/missing.lock";
        const auto noLockFacts=EcReadOnlyProbe(noLockPaths).inspect();
        QVERIFY(!noLockFacts.lockFileExists);
        QVERIFY(!QFileInfo::exists(noLockPaths.lockPath));

        EcOwnershipPolicy policy=EcOwnershipPolicy::n14();
        policy.requiredOwnerUid=static_cast<quint32>(::getuid());
        policy.requiredGroupGid=static_cast<quint32>(::getgid());
        policy.expectedDsdtSha256=facts.dsdtSha256;
        auto denied=evaluateEcOwnership(facts,policy);
        QVERIFY(!denied.allowed);
        QVERIFY(denied.reason.contains("lock"));

        facts.competingOwners.clear();
        facts.lockHeld=true;
        facts.readOnlyTransportVerified=true;
        // A positive decision must use an explicit identity-source fact;
        // generic DMI metadata is intentionally not authoritative.
        facts.ecVersion=policy.expectedEcVersion;
        facts.controllerId="55:71";
        facts.acpiMethod=policy.expectedAcpiMethod;
        const auto allowed=evaluateEcOwnership(facts,policy);
        QVERIFY(allowed.allowed);
        QCOMPARE(QFile(lock).size(),before);

        paths.runtimeDirectory=runtime;
        paths.knownOwnerProcessNames.clear();
        auto identity=std::make_shared<FixedReadOnlyIdentitySource>();
        identity->facts.transportVerified=true;
        identity->facts.ecVersion="2.0";
        identity->facts.controllerId="55:71";
        identity->facts.acpiMethod=policy.expectedAcpiMethod;
        auto verifiedFacts=EcReadOnlyProbe(paths,{}, {},identity).inspect();
        auto lease=EcOwnershipLease::acquire(paths,policy);
        QVERIFY(lease.held());
        lease.applyTo(verifiedFacts);
        verifiedFacts.sleepAllowed=true;
        const auto leasedDecision=evaluateEcOwnership(verifiedFacts,policy);
        QVERIFY(leasedDecision.allowed);
    }
    void ecOwnershipLeaseHandlesContentionReleaseAndStaleFile() {
        using namespace n14::reference;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString runtime=root.path()+"/run/colorful-n14-control";
        const QString lock=runtime+"/ec-owner.lock";
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));
        writePrivateSnapshot(lock,"stable lock contents");

        EcReadOnlyProbePaths paths;
        paths.runtimeDirectory=runtime; paths.lockPath=lock;
        EcOwnershipPolicy policy=EcOwnershipPolicy::n14();
        policy.requiredOwnerUid=static_cast<quint32>(::getuid());
        policy.requiredGroupGid=static_cast<quint32>(::getgid());
        QFile lockSnapshot(lock);
        QVERIFY(lockSnapshot.open(QIODevice::ReadOnly));
        const QByteArray before=lockSnapshot.readAll();
        {
            auto first=EcOwnershipLease::acquire(paths,policy);
            QVERIFY(first.held());
            QVERIFY(first.failureReason().isEmpty());
            EcOwnershipFacts facts;
            first.applyTo(facts);
            QVERIFY(facts.lockHeld);

            const auto second=EcOwnershipLease::acquire(paths,policy);
            QVERIFY(!second.held());
            QVERIFY(second.failureReason().contains("contended"));
            QCOMPARE(QFile(lock).size(),qint64(before.size()));
        }

        // A released lock file remains in place; no stale-file deletion is
        // attempted, and the next owner can acquire it normally.
        const auto reacquired=EcOwnershipLease::acquire(paths,policy);
        QVERIFY(reacquired.held());
        QCOMPARE(QFile(lock).size(),qint64(before.size()));

        QVERIFY(QFile::setPermissions(lock,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ReadGroup));
        const auto wrongMode=EcOwnershipLease::acquire(paths,policy);
        QVERIFY(!wrongMode.held());
        QVERIFY(wrongMode.failureReason().contains("metadata"));
        QVERIFY(QFile::setPermissions(lock,QFileDevice::ReadOwner|QFileDevice::WriteOwner));

        const QString target=root.path()+"/lock-target";
        writePrivateSnapshot(target,"target contents");
        const QString lockSymlink=root.path()+"/lock-link";
        QVERIFY(QFile::link(target,lockSymlink));
        auto symlinkPaths=paths;
        symlinkPaths.lockPath=lockSymlink;
        const auto symlinkLease=EcOwnershipLease::acquire(symlinkPaths,policy);
        QVERIFY(!symlinkLease.held());
        QVERIFY(symlinkLease.failureReason().contains("regular file"));
        QCOMPARE(QFile(target).size(),qint64(QByteArray("target contents").size()));

        auto missingPaths=paths;
        missingPaths.lockPath=runtime+"/missing.lock";
        const auto missing=EcOwnershipLease::acquire(missingPaths,policy);
        QVERIFY(!missing.held());
        QVERIFY(!QFileInfo::exists(missingPaths.lockPath));
    }
    void ecBrokerEnsureLockFileCreatesTrustedPlaceholder() {
        using namespace n14::reference;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString runtime=root.path()+"/run/colorful-n14-control";
        const QString lock=runtime+"/ec-owner.lock";
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));

        EcReadOnlyProbePaths paths;
        paths.runtimeDirectory=runtime;
        paths.lockPath=lock;
        EcOwnershipPolicy policy=EcOwnershipPolicy::n14();
        policy.requiredOwnerUid=static_cast<quint32>(::getuid());
        policy.requiredGroupGid=static_cast<quint32>(::getgid());
        QString reason;
        QVERIFY2(EcBroker::ensureLockFile(paths,policy,&reason),qPrintable(reason));

        const QFileInfo info(lock);
        QVERIFY(info.exists());
        QVERIFY(info.isFile());
        QVERIFY(!info.isSymLink());
        struct stat lockStat{};
        QVERIFY(::lstat(QFile::encodeName(lock).constData(),&lockStat)==0);
        QCOMPARE(static_cast<quint32>(lockStat.st_uid),policy.requiredOwnerUid);
        QCOMPARE(static_cast<quint32>(lockStat.st_gid),policy.requiredGroupGid);
        QCOMPARE(static_cast<quint32>(lockStat.st_mode&07777),policy.requiredLockMode);
        QCOMPARE(static_cast<quint32>(lockStat.st_nlink),quint32(1));

        QVERIFY2(EcBroker::ensureLockFile(paths,policy,&reason),qPrintable(reason));
        QVERIFY(::lstat(QFile::encodeName(lock).constData(),&lockStat)==0);
        QCOMPARE(static_cast<quint32>(lockStat.st_nlink),quint32(1));
    }
    void ecReadOnlyProbeUsesInjectedReadOnlyInterfaces() {
        using namespace n14::reference;
        const QString sys="/fixture/sys";
        const QString proc="/fixture/proc";
        const QString runtime="/fixture/run/colorful-n14-control";
        const QString lock=runtime+"/ec-owner.lock";
        const auto uid=static_cast<quint32>(::getuid());
        const auto gid=static_cast<quint32>(::getgid());
        auto fileSystem=std::make_shared<RecordingReadOnlyFileSystem>();
        fileSystem->statuses[runtime]={true,false,false,true,uid,gid,0700,1};
        fileSystem->statuses[lock]={true,false,true,false,uid,gid,0600,1};
        fileSystem->statuses[proc+"/acpi/call"]={true,false,true,false,uid,gid,0600,1};
        fileSystem->contents[sys+"/class/dmi/id/product_name"]="N14 24\n";
        fileSystem->contents[sys+"/class/dmi/id/sys_vendor"]="COLORFUL\n";
        fileSystem->contents[sys+"/class/dmi/id/bios_version"]="2.07\n";
        fileSystem->contents[sys+"/class/dmi/id/ec_firmware_release"]="2.0\n";
        fileSystem->contents[sys+"/firmware/acpi/tables/DSDT"]="dsdt";
        fileSystem->directories[sys+"/bus/wmi/devices"]={
            "3161c7c3-489f-4074-82ae-538101cce1c2-0"};
        auto processes=std::make_shared<RecordingReadOnlyProcessSource>();
        processes->names={"nbfc_service","unrelated_process"};

        EcReadOnlyProbePaths paths;
        paths.sysRoot=sys; paths.procRoot=proc; paths.runtimeDirectory=runtime;
        paths.lockPath=lock; paths.acpiCallPath=proc+"/acpi/call";
        EcReadOnlyProbe probe(paths,fileSystem,processes);
        const auto facts=probe.inspect();

        QCOMPARE(facts.runtimeDirectoryMode,quint32(0700));
        QCOMPARE(facts.lockFileMode,quint32(0600));
        QCOMPARE(facts.vendor,QString("COLORFUL"));
        QCOMPARE(facts.model,QString("N14 24"));
        QVERIFY(facts.acpiEndpointPresent);
        QVERIFY(facts.competingOwners.contains("nbfc_service"));
        QVERIFY(fileSystem->statusCalls.contains(runtime));
        QVERIFY(fileSystem->statusCalls.contains(lock));
        QVERIFY(fileSystem->statusCalls.contains(paths.acpiCallPath));
        QVERIFY(fileSystem->readCalls.contains(sys+"/firmware/acpi/tables/DSDT"));
        QVERIFY(fileSystem->listCalls.contains(sys+"/bus/wmi/devices"));
        QCOMPARE(processes->roots,QStringList{proc});

        auto inaccessibleFileSystem=std::make_shared<RecordingReadOnlyFileSystem>();
        inaccessibleFileSystem->statuses[runtime]={false,false,false,false,0,0,0,0,false};
        inaccessibleFileSystem->statuses[lock]={false,false,false,false,0,0,0,0,false};
        const auto inaccessibleFacts=EcReadOnlyProbe(paths,inaccessibleFileSystem,processes).inspect();
        QVERIFY(!inaccessibleFacts.runtimeDirectoryQueryAccessible);
        QVERIFY(!inaccessibleFacts.lockFileQueryAccessible);
        const auto inaccessibleDecision=evaluateEcOwnership(inaccessibleFacts);
        QVERIFY(!inaccessibleDecision.allowed);
        QVERIFY(inaccessibleDecision.reason.contains("cannot be inspected"));

        EcOwnershipPolicy policy=EcOwnershipPolicy::n14();
        policy.requiredOwnerUid=uid; policy.requiredGroupGid=gid;
        auto lockBackend=std::make_shared<RecordingLockBackend>();
        {
            const auto lease=EcOwnershipLease::acquire(paths,policy,fileSystem,lockBackend);
            QVERIFY(lease.held());
        }
        QCOMPARE(lockBackend->openCalls,1);
        QCOMPARE(lockBackend->lockCalls,1);
        QCOMPARE(lockBackend->closeCalls,1);
    }
    void ecReadOnlyProcessSourceIncludesCommandBasenames() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir().mkpath(root.path()+"/123"));
        write(root.path(),"/123/comm","python3\n");
        QByteArray commandLine("/usr/bin/python3");
        commandLine.append('\0');
        commandLine.append("/usr/local/share/colorful-n14-control/adapter_server.py");
        commandLine.append('\0');
        write(root.path(),"/123/cmdline",commandLine);
        const auto names=reference::ProcEcReadOnlyProcessSource().processNames(root.path());
        QVERIFY(names.contains("python3"));
        QVERIFY(names.contains("adapter_server.py"));
    }
    void productionIdentitySourceUsesOnlyPinnedIdentityInputs() {
        using namespace n14::reference;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString smbios=root.path()+"/raw";
        QByteArray raw(32,'\0');
        raw[0]=0; raw[1]=24; raw[22]=2; raw[23]=0;
        writePrivateSnapshot(smbios,raw);
        auto transport=std::make_shared<FixedIdentityReadTransport>();
        ProductionEcIdentitySource source(smbios,transport);
        const auto facts=source.inspect();
        QVERIFY(facts.transportVerified);
        QCOMPARE(facts.ecVersion,QString("2.0"));
        QCOMPARE(facts.controllerId,QString("55:71"));
        QCOMPARE(facts.acpiMethod,QString("\\_SB.AMW0.WMBA"));

        transport->high=0x55; transport->low=0x70;
        const auto mismatch=source.inspect();
        QVERIFY(!mismatch.transportVerified);
        QCOMPARE(mismatch.controllerId,QString("55:70"));
        QVERIFY(!ProductionEcIdentitySource::parseSmbiosEcVersion(QByteArray("bad")));
        QByteArray wrongVersion=raw; wrongVersion[23]=1;
        QVERIFY(!ProductionEcIdentitySource::parseSmbiosEcVersion(wrongVersion));
    }
    void ecBrokerOwnsLeaseBeforeTransportAndRevokesOnTransactionFailure() {
        using namespace n14::reference;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString runtime=root.path()+"/runtime";
        const QString lock=runtime+"/ec-owner.lock";
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime,QFileDevice::ReadOwner|QFileDevice::WriteOwner|
                                      QFileDevice::ExeOwner));
        writePrivateSnapshot(lock,"broker lock");

        EcReadOnlyProbePaths paths;
        paths.runtimeDirectory=runtime;
        paths.lockPath=lock;
        paths.knownOwnerProcessNames.clear();
        EcOwnershipPolicy policy=EcOwnershipPolicy::n14();
        policy.requiredOwnerUid=static_cast<quint32>(::getuid());
        policy.requiredGroupGid=static_cast<quint32>(::getgid());
        auto facts=validOwnershipFacts();
        facts.runtimeDirectoryOwnerUid=policy.requiredOwnerUid;
        facts.runtimeDirectoryGroupGid=policy.requiredGroupGid;
        facts.lockFileOwnerUid=policy.requiredOwnerUid;
        facts.lockFileGroupGid=policy.requiredGroupGid;
        facts.lockHeld=false; // The broker must prove this itself.

        auto transport=std::make_unique<FakeAcpiEcTransport>();
        auto *rawTransport=transport.get();
        rawTransport->seed("0x50",2);
        rawTransport->seed("0x54",70);
        rawTransport->seed("0x55",70);
        rawTransport->seed("0x5b",0);
        EcTransactionPolicy transactionPolicy;
        transactionPolicy.expectedSamplerSession="12345678-1234-4234-8234-123456789abc";
        transactionPolicy.expectedSamplerGeneration=1;
        EcBroker broker(paths,policy,transactionPolicy,std::move(transport));
        QString reason;
        QVERIFY2(broker.start(facts,&reason),qPrintable(reason));
        QVERIFY(broker.active());
        QVERIFY(broker.ownerGeneration()>0);
        QVERIFY(rawTransport->isOpen());

        EcCommandProvenance provenance;
        provenance.samplerSession=transactionPolicy.expectedSamplerSession;
        provenance.samplerGeneration=1;
        provenance.sampleId=1;
        provenance.epochSession=provenance.samplerSession;
        provenance.epochGeneration=1;
        provenance.sampledAtUnixMs=100000;
        provenance.sampledAtBoottimeSeconds=100.0;
        provenance.calculationEpoch=1;
        provenance.brokerOwnerGeneration=broker.ownerGeneration();
        provenance.sleepAllowed=true;
        const auto committed=broker.submit({EcCommandTarget::CpuFan,50,provenance},
                                           {100500,100.5});
        QVERIFY(committed.committed);

        provenance.sampleId=2; provenance.calculationEpoch=2;
        int keylightGuards=0;
        const auto light=broker.submit({EcCommandTarget::Keylight,50,provenance},{100600,100.6},
                                       [&] { ++keylightGuards; });
        QVERIFY2(light.committed,qPrintable(light.reason));
        QCOMPARE(keylightGuards,3);
        // Exercise the actual broker's production sleep callback, not a fake
        // delay recorder. No upper bound: loaded CI must not make this flaky.
        QVERIFY(light.keylightSleepUs>=20000);
        QVERIFY(light.keylightPrewriteUs>=light.keylightSleepUs);
        QCOMPARE(ecTransactionDiagnostics(light)["keylight_prewrite_us"].toInteger(),light.keylightPrewriteUs);
        QCOMPARE(ecTransactionDiagnostics(light)["keylight_sleep_us"].toInteger(),light.keylightSleepUs);

        rawTransport->failNextWrite("0x54");
        provenance.sampleId=3;
        provenance.calculationEpoch=3;
        const auto failed=broker.submit({EcCommandTarget::CpuFan,60,provenance},
                                        {100700,100.7});
        QVERIFY(!failed.committed);
        QVERIFY(!broker.active());
        QVERIFY(!rawTransport->isOpen());
    }
    void fakeTransactionQueueSerializesFanAndKeylightWithProvenance() {
        using namespace n14::reference;
        FakeAcpiEcTransport transport;
        transport.seed("0x50",2);
        transport.seed("0x54",70);
        transport.seed("0x55",70);
        transport.seed("0x5b",88);
        EcTransactionPolicy policy;
        policy.expectedSamplerSession="12345678-1234-4234-8234-123456789abc";
        policy.expectedSamplerGeneration=1;
        policy.expectedBrokerOwnerGeneration=1;
        QVector<int> settle;
        FakeEcTransactionQueue queue(transport,policy,[&](int milliseconds){ settle.append(milliseconds); });
        QVERIFY(queue.start({true,{}}));

        const auto provenance=[](quint64 sampleId, quint64 calculationEpoch) {
            EcCommandProvenance value;
            value.samplerSession="12345678-1234-4234-8234-123456789abc";
            value.samplerGeneration=1;
            value.sampleId=sampleId;
            value.epochSession=value.samplerSession;
            value.epochGeneration=1;
            value.sampledAtUnixMs=100000;
            value.sampledAtBoottimeSeconds=100.0;
            value.calculationEpoch=calculationEpoch;
            value.brokerOwnerGeneration=1;
            value.sleepAllowed=true;
            return value;
        };
        EcLogicalCommand fan{EcCommandTarget::BothFans,50,provenance(1,1)};
        const auto fanResult=queue.submit(fan,{100500,100.5},[] {
            throw std::runtime_error("keylight guard must not run for fans");
        });
        QVERIFY(fanResult.committed);
        QCOMPARE(fanResult.rawReadback,fanDutyToRaw(50));
        QCOMPARE(transport.events().size(),7);
        QCOMPARE(transport.events().at(0).operation,QString("open"));
        QCOMPARE(transport.events().at(1).registerName,QString("0x50"));
        QCOMPARE(transport.events().at(2).operation,QString("read"));
        QCOMPARE(transport.events().at(5).registerName,QString("0x55"));

        EcLogicalCommand light{EcCommandTarget::Keylight,50,provenance(2,2)};
        const auto lightResult=queue.submit(light,{100600,100.6});
        QVERIFY(lightResult.committed);
        QCOMPARE(lightResult.rawReadback,keylightPercentToRaw(50));
        QCOMPARE(settle,QVector<int>{20});
        QVERIFY(queue.active());
        QCOMPARE(transport.events().last().operation,QString("read"));
        QCOMPARE(transport.events().last().registerName,QString("0x5b"));
    }
    void keylightConfirmsAutoBeforeMainLightWrite_data() {
        QTest::addColumn<int>("modeAfterSettle");
        QTest::newRow("confirmed-auto")<<0;
        QTest::newRow("firmware-left-auto")<<2;
    }
    void keylightPreloadsBeforeAutoWithoutRepeatedFanTransition_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("firmware-auto")<<0;
        QTest::newRow("manual-with-independent-fan-targets")<<2;
    }
    void keylightPreloadsBeforeAutoWithoutRepeatedFanTransition() {
        using namespace n14::reference;
        QFETCH(int,mode);
        class TraceTransport final : public EcRegisterTransport {
        public:
            FakeAcpiEcTransport delegate;
            QStringList trace;
            bool open(const EcOwnershipDecision &decision) override { return delegate.open(decision); }
            void close() override { delegate.close(); }
            int read(const QString &name) override { trace.append("read "+name); return delegate.read(name); }
            void write(const QString &name,int value) override {
                trace.append(QString("write %1=%2").arg(name).arg(value)); delegate.write(name,value);
            }
        } transport;
        transport.delegate.seed("0x50",mode); transport.delegate.seed("0x54",40);
        transport.delegate.seed("0x55",50); transport.delegate.seed("0x5b",175);
        FakeEcTransactionQueue queue(transport,{},[&](int milliseconds) {
            transport.trace.append(QString("settle %1").arg(milliseconds));
        });
        QVERIFY(queue.start({true,{}}));
        EcCommandProvenance p;
        p.samplerSession="session"; p.epochSession="session"; p.samplerGeneration=1;
        p.epochGeneration=1; p.sampleId=1; p.sampledAtUnixMs=100000;
        p.sampledAtBoottimeSeconds=100; p.calculationEpoch=1; p.brokerOwnerGeneration=1; p.sleepAllowed=true;
        const auto result=queue.submit({EcCommandTarget::Keylight,25,p},{100500,100.5});
        QVERIFY2(result.committed,qPrintable(result.reason));
        // Physically verified source deviation: preload before auto, not two
        // adjacent writes after auto; still one settle and grouped fan return.
        QStringList expected{"read 0x50"};
        if (mode==2) expected.append({"read 0x54","read 0x55","write 0x5b=132","write 0x50=0"});
        expected.append({"settle 20","read 0x50","write 0x5b=132"});
        if (mode==2) expected.append({"write 0x50=2","write 0x54=40","write 0x55=50",
                                     "read 0x50","read 0x54","read 0x55"});
        expected.append("read 0x5b");
        QCOMPARE(transport.trace,expected);
        QCOMPARE(result.writeAttempts,mode==2 ? 6 : 1);
    }
    void keylightConfirmsAutoBeforeMainLightWrite() {
        using namespace n14::reference;
        QFETCH(int,modeAfterSettle);
        FakeAcpiEcTransport transport;
        transport.seed("0x50",2); transport.seed("0x54",40);
        transport.seed("0x55",50); transport.seed("0x5b",175);
        int eventsAfterSettle=-1;
        FakeEcTransactionQueue queue(transport,{},[&](int milliseconds) {
            QCOMPARE(milliseconds,20); transport.seed("0x50",modeAfterSettle);
            eventsAfterSettle=transport.events().size();
        });
        QVERIFY(queue.start({true,{}}));
        EcCommandProvenance p;
        p.samplerSession="session"; p.epochSession="session"; p.samplerGeneration=1;
        p.epochGeneration=1; p.sampleId=1; p.sampledAtUnixMs=100000;
        p.sampledAtBoottimeSeconds=100; p.calculationEpoch=1; p.brokerOwnerGeneration=1; p.sleepAllowed=true;
        const auto result=queue.submit({EcCommandTarget::Keylight,25,p},{100500,100.5});
        QVERIFY(eventsAfterSettle>=0);
        QVERIFY(transport.events().size()>eventsAfterSettle);
        QCOMPARE(transport.events()[eventsAfterSettle].operation,QString("read"));
        QCOMPARE(transport.events()[eventsAfterSettle].registerName,QString("0x50"));
        QCOMPARE(result.committed,modeAfterSettle==0);
        if (modeAfterSettle!=0) {
            QVERIFY(!queue.active()); QCOMPARE(result.writeAttempts,2);
            QCOMPARE(result.actualRaw,2);
            QCOMPARE(result.reason,QString("automatic fan mode not confirmed for keyboard lighting"));
            QCOMPARE(transport.events().size(),eventsAfterSettle+1);
            // Only the preload has happened; no MAIN light write, manual
            // restore, retry or recovery write follows failed auto verification.
        }
    }
    void keylightPreloadMatchesSyntheticAutoEntryLatch() {
        using namespace n14::reference;
        // Hypothesis model only; actual physical evidence is in VALIDATION.md.
        class AutoLatchTransport final : public EcRegisterTransport {
        public:
            FakeAcpiEcTransport delegate;
            int physical=175;
            bool open(const EcOwnershipDecision &d) override { return delegate.open(d); }
            void close() override { delegate.close(); }
            int read(const QString &name) override { return delegate.read(name); }
            void write(const QString &name,int value) override {
                if (name=="0x50" && value==0) physical=delegate.read("0x5b");
                delegate.write(name,value);
            }
        } transport;
        transport.delegate.seed("0x50",2); transport.delegate.seed("0x54",40);
        transport.delegate.seed("0x55",50); transport.delegate.seed("0x5b",175);
        FakeEcTransactionQueue queue(transport); QVERIFY(queue.start({true,{}}));
        EcCommandProvenance p;
        p.samplerSession="session"; p.epochSession="session"; p.samplerGeneration=1;
        p.epochGeneration=1; p.sampleId=1; p.sampledAtUnixMs=100000;
        p.sampledAtBoottimeSeconds=100; p.calculationEpoch=1; p.brokerOwnerGeneration=1; p.sleepAllowed=true;
        for (int percent : {0,100,0,25,75,50,0}) {
            const auto result=queue.submit({EcCommandTarget::Keylight,percent,p},{100500,100.5});
            QVERIFY(result.committed); QCOMPARE(result.writeAttempts,6);
            QCOMPARE(transport.physical,keylightPercentToRaw(percent));
            QCOMPARE(transport.delegate.read("0x50"),2);
            QCOMPARE(transport.delegate.read("0x54"),40); QCOMPARE(transport.delegate.read("0x55"),50);
            ++p.calculationEpoch; ++p.sampleId;
        }
    }
    void keylightContinuationGuardStopsBeforeEachPhase_data() {
        QTest::addColumn<int>("failGuard"); QTest::addColumn<int>("attempts");
        QTest::newRow("preload")<<1<<0;
        QTest::newRow("main-light-write")<<2<<2;
        QTest::newRow("manual-return")<<3<<3;
    }
    void keylightContinuationGuardStopsBeforeEachPhase() {
        using namespace n14::reference;
        QFETCH(int,failGuard); QFETCH(int,attempts);
        FakeAcpiEcTransport transport;
        transport.seed("0x50",2); transport.seed("0x54",40);
        transport.seed("0x55",50); transport.seed("0x5b",175);
        FakeEcTransactionQueue queue(transport); QVERIFY(queue.start({true,{}}));
        EcCommandProvenance p;
        p.samplerSession="session"; p.epochSession="session"; p.samplerGeneration=1;
        p.epochGeneration=1; p.sampleId=1; p.sampledAtUnixMs=100000;
        p.sampledAtBoottimeSeconds=100; p.calculationEpoch=1; p.brokerOwnerGeneration=1; p.sleepAllowed=true;
        int guards=0;
        const auto result=queue.submit({EcCommandTarget::Keylight,50,p},{100500,100.5},[&] {
            if (++guards==failGuard) throw std::runtime_error("original sample/epoch invalidated");
        });
        QVERIFY(!result.committed); QVERIFY(!queue.active());
        QCOMPARE(guards,failGuard); QCOMPARE(result.writeAttempts,attempts);
        QCOMPARE(result.stage,EcTransactionStage::Observation);
        QVERIFY(std::none_of(transport.events().cbegin(),transport.events().cend(),[](const auto &e) {
            return e.operation=="write" && e.registerName=="0x50" && e.value==2;
        }));
        const auto events=transport.events().size();
        const auto denied=queue.submit({EcCommandTarget::Keylight,50,p},{100600,100.6});
        QVERIFY(!denied.committed); QCOMPARE(denied.writeAttempts,0);
        QCOMPARE(transport.events().size(),events);
    }
    void fakeTransactionQueueRevokesOnStaleReplayAndPartialFailure() {
        using namespace n14::reference;
        const auto provenance=[](quint64 sampleId, quint64 calculationEpoch,
                                 qint64 sampledAtUnixMs=100000) {
            EcCommandProvenance value;
            value.samplerSession="12345678-1234-4234-8234-123456789abc";
            value.samplerGeneration=1;
            value.sampleId=sampleId;
            value.epochSession=value.samplerSession;
            value.epochGeneration=1;
            value.sampledAtUnixMs=sampledAtUnixMs;
            value.sampledAtBoottimeSeconds=100.0;
            value.calculationEpoch=calculationEpoch;
            value.brokerOwnerGeneration=1;
            value.sleepAllowed=true;
            return value;
        };
        EcTransactionPolicy policy;
        policy.expectedSamplerSession="12345678-1234-4234-8234-123456789abc";
        policy.expectedSamplerGeneration=1;
        policy.expectedBrokerOwnerGeneration=1;

        FakeAcpiEcTransport staleTransport;
        staleTransport.seed("0x50",2); staleTransport.seed("0x54",70); staleTransport.seed("0x55",70);
        FakeEcTransactionQueue staleQueue(staleTransport,policy);
        QVERIFY(staleQueue.start({true,{}}));
        const auto stale=staleQueue.submit({EcCommandTarget::CpuFan,50,provenance(1,1,97000)},
                                            {100500,100.5});
        QVERIFY(!stale.committed);
        QVERIFY(stale.reason.contains("stale"));
        QCOMPARE(stale.stage,EcTransactionStage::Validation);
        QCOMPARE(stale.writeAttempts,0);
        QVERIFY(!ecTransactionDiagnostics(stale)["hardware_state_uncertain"].toBool());
        QVERIFY(!staleQueue.active());
        QCOMPARE(staleTransport.events().size(),1);

        FakeAcpiEcTransport failureTransport;
        failureTransport.seed("0x50",2); failureTransport.seed("0x54",70); failureTransport.seed("0x55",70);
        failureTransport.failNextWrite("0x55");
        FakeEcTransactionQueue failureQueue(failureTransport,policy);
        QVERIFY(failureQueue.start({true,{}}));
        const auto partial=failureQueue.submit({EcCommandTarget::BothFans,50,provenance(1,1)},
                                                {100500,100.5});
        QVERIFY(!partial.committed);
        QVERIFY(partial.reason.contains("failure"));
        QVERIFY(!failureQueue.active());
        QCOMPARE(failureTransport.events().size(),5);
        QCOMPARE(failureTransport.events().last().registerName,QString("0x54"));
        QVERIFY(std::none_of(failureTransport.events().cbegin(),failureTransport.events().cend(),
                             [](const FakeTransportEvent &event) {
                                 return event.operation=="write" && event.registerName=="0x55";
                             }));

        FakeAcpiEcTransport replayTransport;
        replayTransport.seed("0x50",2); replayTransport.seed("0x54",70); replayTransport.seed("0x55",70);
        FakeEcTransactionQueue replayQueue(replayTransport,policy);
        QVERIFY(replayQueue.start({true,{}}));
        QVERIFY(replayQueue.submit({EcCommandTarget::CpuFan,40,provenance(1,1)},
                                   {100500,100.5}).committed);
        QVERIFY(replayQueue.submit({EcCommandTarget::CpuFan,40,provenance(1,2)},
                                   {100600,100.6}).committed);
        const auto replay=replayQueue.submit({EcCommandTarget::CpuFan,40,provenance(1,2)},
                                             {100600,100.6});
        QVERIFY(!replay.committed);
        QVERIFY(replay.reason.contains("replayed"));
        QCOMPARE(replay.stage,EcTransactionStage::Validation);
        QCOMPARE(replay.writeAttempts,0);
        QVERIFY(!replayQueue.active());
    }
    void transactionFailureDiagnostics_data() {
        QTest::addColumn<bool>("keylight"); QTest::addColumn<int>("operation");
        QTest::addColumn<QString>("fault"); QTest::addColumn<QString>("stage");
        QTest::addColumn<int>("attempts"); QTest::addColumn<QString>("registerName");
        struct Operation { const char *stage; const char *name; int attempts; };
        const QList<Operation> fan{{"write","0x50",1},{"readback","0x50",1},
            {"write","0x54",2},{"readback","0x54",2},{"write","0x55",3},{"readback","0x55",3}};
        const QList<Operation> light{{"observation","0x50",0},{"observation","0x54",0},
            {"observation","0x55",0},{"write","0x5b",1},{"write","0x50",2},
            {"readback","0x50",2},{"write","0x5b",3},{"write","0x50",4},{"write","0x54",5},
            {"write","0x55",6},{"readback","0x50",6},{"readback","0x54",6},
            {"readback","0x55",6},{"readback","0x5b",6}};
        for (bool keylight : {false,true}) {
            const auto &operations=keylight ? light : fan;
            for (int i=0;i<operations.size();++i) {
                const auto &op=operations.at(i);
                QStringList faults{"throw"};
                if (QString(op.stage)=="write") faults.append("after-write");
                if (QString(op.stage)=="readback") faults.append("mismatch");
                for (const auto &fault : faults) {
                    const auto name=QString("%1-%2-%3").arg(keylight ? "keylight" : "fan").arg(i+1).arg(fault).toUtf8();
                    QTest::newRow(name.constData())<<keylight<<(i+1)<<fault<<QString(op.stage)<<op.attempts<<QString(op.name);
                }
            }
        }
        QTest::newRow("keylight-settle")<<true<<5<<QString("settle")<<QString("settle")<<2<<QString();
    }
    void transactionFailureDiagnostics() {
        using namespace n14::reference;
        QFETCH(bool,keylight); QFETCH(int,operation); QFETCH(QString,fault);
        QFETCH(QString,stage); QFETCH(int,attempts); QFETCH(QString,registerName);
        class FaultTransport final : public EcRegisterTransport {
        public:
            FakeAcpiEcTransport delegate;
            int failAt=0; int operations=0; QString fault;
            bool open(const EcOwnershipDecision &decision) override { return delegate.open(decision); }
            void close() override { delegate.close(); }
            int read(const QString &name) override {
                ++operations;
                if (operations==failAt && fault=="throw") throw std::runtime_error("injected read failure");
                const int value=delegate.read(name);
                return operations==failAt && fault=="mismatch" ? 255 : value;
            }
            void write(const QString &name,int value) override {
                ++operations;
                if (operations==failAt && fault=="throw") throw std::runtime_error("injected write failure");
                delegate.write(name,value);
                if (operations==failAt && fault=="after-write") throw std::runtime_error("injected failure after write");
            }
        } transport;
        transport.failAt=operation; transport.fault=fault;
        transport.delegate.seed("0x50",2); transport.delegate.seed("0x54",70);
        transport.delegate.seed("0x55",70); transport.delegate.seed("0x5b",88);
        EcCommandProvenance p;
        p.samplerSession="session"; p.epochSession="session"; p.samplerGeneration=1;
        p.epochGeneration=1; p.sampleId=1; p.sampledAtUnixMs=100000;
        p.sampledAtBoottimeSeconds=100; p.calculationEpoch=1; p.brokerOwnerGeneration=1; p.sleepAllowed=true;
        FakeEcTransactionQueue queue(transport,{},[&](int milliseconds) {
            QCOMPARE(milliseconds,20);
            if (fault=="settle") throw std::runtime_error("injected settle failure");
        });
        QVERIFY(queue.start({true,{}}));
        const EcLogicalCommand command{keylight ? EcCommandTarget::Keylight : EcCommandTarget::BothFans,50,p};
        const auto result=queue.submit(command,{100500,100.5});
        QVERIFY(!result.committed); QVERIFY(!result.reason.isEmpty()); QVERIFY(!queue.active());
        QCOMPARE(result.rawReadback,-1); QCOMPARE(transport.operations,operation);
        const auto diagnostics=ecTransactionDiagnostics(result);
        QCOMPARE(diagnostics["stage"].toString(),stage);
        QCOMPARE(diagnostics["write_attempts"].toInt(),attempts);
        QCOMPARE(diagnostics["register"].toString(),registerName);
        QCOMPARE(diagnostics["hardware_state_uncertain"].toBool(),attempts>0);
        QCOMPARE(diagnostics["actual_raw"].toInt(),fault=="mismatch" ? 255 : -1);
        if (stage=="write" || stage=="readback") QVERIFY(diagnostics["expected_raw"].toInt()>=0);
        else QCOMPARE(diagnostics["expected_raw"].toInt(),-1);
        const auto successfulWrites=std::count_if(transport.delegate.events().cbegin(),transport.delegate.events().cend(),
            [](const auto &event) { return event.operation=="write"; });
        QCOMPARE(successfulWrites,stage=="write" && fault=="throw" ? attempts-1 : attempts);
        const int eventCount=transport.delegate.events().size();
        const auto inactive=queue.submit(command,{100600,100.6});
        QCOMPARE(inactive.stage,EcTransactionStage::Validation); QCOMPARE(inactive.writeAttempts,0);
        QCOMPARE(transport.delegate.events().size(),eventCount); // no retry, observation or compensation
    }
    void telemetryPollerKeepsEventLoopResponsiveAndCoalesces() {
        std::atomic_int reads=0;
        std::atomic_bool workerWasSeparate=false;
        const auto owner=QThread::currentThread();
        TelemetryPoller poller(nullptr,[&] {
            workerWasSeparate=QThread::currentThread()!=owner;
            ++reads;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            Snapshot snapshot; snapshot.percent=42; return snapshot;
        });
        int completions=0,heartbeats=0;
        QTimer heartbeat; heartbeat.setInterval(5);
        connect(&heartbeat,&QTimer::timeout,this,[&] { ++heartbeats; }); heartbeat.start();
        const auto completion=[&](Snapshot snapshot,QString error) {
            QCOMPARE(QThread::currentThread(),owner);
            QCOMPARE(snapshot.percent,42); QVERIFY(error.isEmpty()); ++completions;
        };
        QVERIFY(poller.request(completion)); QVERIFY(poller.pending());
        for (int i=0;i<1000;++i) QVERIFY(!poller.request(completion));
        QTRY_COMPARE_WITH_TIMEOUT(completions,1,2000);
        QVERIFY(heartbeats>=5); QVERIFY(workerWasSeparate.load()); QCOMPARE(reads.load(),1);
        QVERIFY(!poller.pending()); QVERIFY(poller.request(completion));
        QTRY_COMPARE_WITH_TIMEOUT(completions,2,2000); QCOMPARE(reads.load(),2);
    }
    void telemetryPollerRecoversAfterReadFailureWithoutQueueGrowth() {
        std::atomic_int reads=0;
        TelemetryPoller poller(nullptr,[&] {
            if (++reads==1) throw std::runtime_error("injected slow sysfs failure");
            return Snapshot{};
        });
        int completed=0; QString firstError;
        QVERIFY(poller.request([&](Snapshot,QString error) { firstError=error; ++completed; }));
        QTRY_COMPARE_WITH_TIMEOUT(completed,1,2000);
        QVERIFY(firstError.contains("injected")); QVERIFY(!poller.pending());
        QVERIFY(poller.request([&](Snapshot,QString error) { QVERIFY(error.isEmpty()); ++completed; }));
        QTRY_COMPARE_WITH_TIMEOUT(completed,2,2000);
        QCOMPARE(reads.load(),2);
    }
    void telemetryPollerDropsCompletionAfterDestruction() {
        int completed=0;
        {
            TelemetryPoller poller(nullptr,[] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); return Snapshot{};
            });
            QVERIFY(poller.request([&](Snapshot,QString) { ++completed; }));
            QTest::qSleep(5);
        }
        QCoreApplication::processEvents();
        QCOMPARE(completed,0);
    }
    void validatedThermalInputFeedsReadOnlyGraphAndStandardLayers() {
        using namespace n14::reference;
        RuntimeThermalSampler sampler;
        const auto sample=sampler.accept(referenceSnapshotJson(),1800000001000LL,1001.0);
        const auto graphs=defaultCoolerControlGraphs();
        const auto demand=curveDemand(graphs.cpu,graphs.gpu,sample.controlInput(),100,100);
        QVERIFY(demand.has_value());
        StandardGraphProcessor cpu(graphs.cpu,100,defaultCpuStandardFunction());
        StandardGraphProcessor gpu(graphs.gpu,100,defaultGpuStandardFunction());
        const auto cpuResult=cpu.update({sample.cpuC});
        const auto gpuResult=gpu.update({*sample.gpuC});
        QVERIFY(cpuResult.duty.has_value()); QVERIFY(gpuResult.duty.has_value());
        QCOMPARE(*demand,std::max(*cpuResult.duty,*gpuResult.duty));
        // This test intentionally stops at calculated duties: no writer or
        // runtime service is reachable from the sampler/pipeline layer.
    }
    void curveInterpolation() {
        Curve curve{{0,0},{60,0},{80,100}};
        QCOMPARE(curveDuty(curve,-10),0.0); QCOMPARE(curveDuty(curve,60),0.0);
        QCOMPARE(curveDuty(curve,70),50.0); QCOMPARE(curveDuty(curve,100),100.0);
    }
    void invalidCurves_data() {
        QTest::addColumn<Curve>("curve");
        QTest::newRow("empty")<<Curve{};
        QTest::newRow("one")<<Curve{{10,100}};
        QTest::newRow("duplicate")<<Curve{{10,0},{10,100}};
        QTest::newRow("unsorted")<<Curve{{20,0},{10,100}};
        QTest::newRow("decreasing-duty")<<Curve{{10,70},{20,60},{90,100}};
        QTest::newRow("negative")<<Curve{{-1,0},{90,100}};
        QTest::newRow("no-full")<<Curve{{0,0},{90,99}};
        QTest::newRow("nan")<<Curve{{0,0},{std::numeric_limits<double>::quiet_NaN(),100}};
        QTest::newRow("infinity")<<Curve{{0,0},{90,std::numeric_limits<double>::infinity()}};
    }
    void invalidCurves() { QFETCH(Curve,curve); QVERIFY(!validateCurve(curve).isEmpty()); }
    void sleepingGpuExcluded() {
        Curve curve{{0,0},{100,100}};
        QCOMPARE(*mixDuty(curve,curve,40,std::nullopt,true,"Max"),40.0);
        QVERIFY(!mixDuty(curve,curve,40,std::nullopt,false,"Max"));
        QVERIFY(!mixDuty(curve,curve,std::nullopt,70,true,"Max"));
    }
    void mixFunctions() {
        Curve curve{{0,0},{100,100}};
        QCOMPARE(*mixDuty(curve,curve,40,80,false,"Max"),80.0);
        QCOMPARE(*mixDuty(curve,curve,40,80,false,"Min"),40.0);
        QCOMPARE(*mixDuty(curve,curve,40,80,false,"Mean"),60.0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,mixDuty(curve,curve,40,80,false,"wrong"));
    }
    void defaultSettingsOwnsPowerProfile() {
        const Settings s; QVERIFY(s.autoProfile); QVERIFY(!s.autoRefresh);
        QCOMPARE(Settings::fromJson(s.toJson()).toJson(),s.toJson());
    }
    void settingsValidation_data() {
        QTest::addColumn<QString>("key"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("schema")<<QString("schema")<<QJsonValue(4);
        QTest::newRow("bool-schema")<<QString("schema")<<QJsonValue(true);
        QTest::newRow("bool")<<QString("auto_profile")<<QJsonValue(1);
        QTest::newRow("profile")<<QString("ac_profile")<<QJsonValue("arbitrary-command");
        QTest::newRow("rate")<<QString("ac_hz")<<QJsonValue(144);
        QTest::newRow("fraction-rate")<<QString("ac_hz")<<QJsonValue(60.1);
        QTest::newRow("threshold")<<QString("low_percent")<<QJsonValue(15.1);
        QTest::newRow("low-threshold")<<QString("low_percent")<<QJsonValue(0);
        QTest::newRow("mix")<<QString("mix")<<QJsonValue("bad");
        QTest::newRow("function")<<QString("cpu_function")<<QJsonValue(QJsonObject{{"kind","Standard"},{"deviance",2},{"window",5}});
    }
    void settingsValidation() {
        QFETCH(QString,key); QFETCH(QJsonValue,value);
        auto j=Settings{}.toJson(); j[key]=value;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
    }
    void settingsUnknownMissingKeys() {
        auto j=Settings{}.toJson(); j["unknown"]=1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
        j.remove("unknown"); j.remove("cpu_curve");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
    }
    void settingsAtomicRoundTrip() {
        QTemporaryDir dir; const QString path=dir.path()+"/nested/config.json";
        Settings s; s.autoProfile=true; s.cpuFunction={"Hysteresis",5,8}; s.save(path);
        QCOMPARE(Settings::load(path).toJson(),s.toJson());
        const auto before=Settings::load(path).toJson(); s.cpu.clear();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,s.save(path));
        QCOMPARE(Settings::load(path).toJson(),before);
        QVERIFY(!(QFileInfo(path).permissions()&QFileDevice::ReadOther));
    }
    void nativeCoolerControlSettingsRoundTripAndStaySeparateFromPreview() {
        using namespace n14::reference;
        Settings settings;
        settings.cpuFunction={"Hysteresis",7,9};
        settings.coolerCpuFunction=defaultCpuStandardFunction();
        settings.coolerCpuFunction.responseDelay=4;
        settings.coolerCpuFunction.stepMinimum=3;
        settings.coolerCpuFunction.onlyDownward=true;
        settings.coolerCpuFunction.bypassMinimumAtExtremes=true;
        settings.coolerGpuFunction=defaultGpuStandardFunction();
        settings.coolerGpuFunction.thresholdHopping=false;
        const auto json=settings.toJson();
        QCOMPARE(json["schema"].toInt(),3);
        QVERIFY(json["coolercontrol"].isObject());
        QVERIFY(json["coolercontrol"].toObject()["cpu_function"].isObject());
        QVERIFY(json["cpu_function"].toObject()["kind"].toString()=="Hysteresis");
        QVERIFY(json["coolercontrol"].toObject()["cpu_function"].toObject()["duty_minimum"].toInt()==3);
        const auto loaded=Settings::fromJson(json);
        QCOMPARE(loaded.toJson(),json);
        QVERIFY(loaded.cpuFunction.kind=="Hysteresis");
        QCOMPARE(loaded.coolerCpuFunction.responseDelay,4);
        QCOMPARE(loaded.coolerCpuFunction.stepMinimum,3);
        QVERIFY(loaded.coolerCpuFunction.onlyDownward);
        QVERIFY(loaded.coolerCpuFunction.bypassMinimumAtExtremes);
        QVERIFY(!loaded.coolerGpuFunction.thresholdHopping);
    }
    void schemaTwoSettingsGainNativeDefaultsDuringMigration() {
        Settings settings;
        auto old=settings.toJson();
        old["schema"]=2;
        old.remove("coolercontrol");
        const auto migrated=Settings::fromJson(old);
        QCOMPARE(migrated.toJson()["schema"].toInt(),3);
        QCOMPARE(migrated.coolerCpuFunction.deviance,5.0);
        QCOMPARE(migrated.coolerGpuFunction.deviance,1.0);
    }
    void nativeCoolerControlSettingsRejectUnknownOrInvalidFields() {
        Settings settings;
        auto json=settings.toJson();
        auto native=json["coolercontrol"].toObject();
        auto cpu=native["cpu_function"].toObject();
        cpu["unknown"]=1;
        native["cpu_function"]=cpu; json["coolercontrol"]=native;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(json));
        json=settings.toJson(); native=json["coolercontrol"].toObject(); cpu=native["cpu_function"].toObject();
        cpu["duty_minimum"]=101; native["cpu_function"]=cpu; json["coolercontrol"]=native;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(json));
        json=settings.toJson(); native=json["coolercontrol"].toObject(); cpu=native["cpu_function"].toObject();
        cpu["only_downward"]=1; native["cpu_function"]=cpu; json["coolercontrol"]=native;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(json));
    }
    void settingsRejectCorruptionAndSymlink() {
        QTemporaryDir dir; write(dir.path(),"/bad.json","{");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::load(dir.path()+"/bad.json"));
        QVERIFY(QFile::link(dir.path()+"/bad.json",dir.path()+"/link.json"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings{}.save(dir.path()+"/link.json"));
    }
    void rulesRequireStablePower() {
        RuleEngine r; Settings s; s.autoProfile=s.autoRefresh=true;
        QVERIFY(!r.observe(true,80,s));
        const auto p=r.observe(true,80,s); QVERIFY(p); QCOMPARE(*p->profile,QString("performance")); QCOMPARE(*p->hz,165);
        QVERIFY(!r.observe(false,80,s)); const auto b=r.observe(false,80,s); QCOMPARE(*b->profile,QString("power-saver")); QCOMPARE(*b->hz,60);
    }
    void unknownPowerNeverActs() {
        RuleEngine r; Settings s; s.autoProfile=true;
        QVERIFY(!r.observe(std::nullopt,10,s)); QVERIFY(!r.observe(false,10,s));
        QVERIFY(!r.observe(std::nullopt,10,s)); QVERIFY(!r.observe(false,10,s));
        QVERIFY(r.observe(false,10,s));
    }
    void transientPowerDebounced() {
        RuleEngine r; Settings s; s.autoProfile=true;
        QVERIFY(!r.observe(true,80,s)); QVERIFY(!r.observe(false,80,s)); QVERIFY(!r.observe(true,80,s));
        QVERIFY(r.observe(true,80,s));
    }
    void lowBatteryPriority() {
        RuleEngine r; Settings s; s.autoProfile=true; s.batteryProfile="balanced";
        r.observe(false,15,s); QCOMPARE(*r.observe(false,15,s)->profile,QString("power-saver"));
        r.reset(); r.observe(false,-1,s); QCOMPARE(*r.observe(false,-1,s)->profile,QString("balanced"));
        r.reset(); r.observe(true,5,s); QCOMPARE(*r.observe(true,5,s)->profile,QString("performance"));
    }
    void independentAutomation() {
        RuleEngine r; Settings s; s.autoProfile=false; r.observe(false,80,s); auto p=r.observe(false,80,s);
        QVERIFY(p); QVERIFY(!p->profile); QVERIFY(!p->hz);
        s.autoRefresh=true; p=r.observe(false,80,s); QVERIFY(!p->profile); QCOMPARE(*p->hz,60);
    }
    void hysteresisFilter() {
        TemperatureFilter f;
        QCOMPARE(*f.update(50,"Hysteresis",5,5),50.0);
        QCOMPARE(*f.update(54,"Hysteresis",5,5),50.0);
        QCOMPARE(*f.update(55,"Hysteresis",5,5),55.0);
        QCOMPARE(*f.update(51,"Hysteresis",5,5),55.0);
        QVERIFY(!f.update(std::nullopt,"Hysteresis",5,5));
        QCOMPARE(*f.update(51,"Hysteresis",5,5),51.0);
    }
    void movingAverageFilter() {
        TemperatureFilter f;
        QCOMPARE(*f.update(10,"MovingAverage",2,2),10.0);
        QCOMPARE(*f.update(20,"MovingAverage",2,2),15.0);
        QCOMPARE(*f.update(30,"MovingAverage",2,2),25.0);
        QVERIFY(!f.update(std::numeric_limits<double>::quiet_NaN(),"MovingAverage",2,2));
        QCOMPARE(*f.update(40,"MovingAverage",2,2),40.0);
    }
    void filterInvalidParameters() {
        TemperatureFilter f;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,f.update(50,"Standard",2,2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,f.update(50,"MovingAverage",2,0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,f.update(50,"Hysteresis",-1,5));
    }
    void screenPreservesResolution() {
        const auto p=parsePanel(screenJson()); QVERIFY(p); QCOMPARE(p->name,QString("eDP-1"));
        const auto mode=p->modeForHz(60); QVERIFY(mode); QCOMPARE(mode->id,QString("2"));
        QVERIFY(!p->modeForHz(144)); QCOMPARE(p->current()->hz,165.0);
    }
    void screenMissingPanel() {
        QVERIFY(!parsePanel("{\"outputs\":[]}"));
        auto b=screenJson(); b.replace("eDP-1","HDMI-1"); QVERIFY(!parsePanel(b));
        b=screenJson(); b.replace("\"connected\":true","\"connected\":false"); QVERIFY(!parsePanel(b));
    }
    void screenRejectsInvalidModes() {
        auto b=screenJson(); b.replace("\"id\":\"2\"","\"id\":\"2;evil\"");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,parsePanel(b));
        b=screenJson(); b.replace("\"currentModeId\":\"1\"","\"currentModeId\":\"99\"");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,parsePanel(b));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,parsePanel("{"));
    }
    void screenRejectsMultiplePanels() {
        auto j=QJsonDocument::fromJson(screenJson()).object(); auto outputs=j["outputs"].toArray(); outputs.append(outputs[0]); j["outputs"]=outputs;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,parsePanel(QJsonDocument(j).toJson()));
    }
    void peripheralPowerSuppliesDoNotAffectLaptopRules_data() {
        QTest::addColumn<bool>("laptopPresent");
        QTest::newRow("laptop-and-mouse")<<true;
        QTest::newRow("only-peripherals-is-unknown")<<false;
    }
    void peripheralPowerSuppliesDoNotAffectLaptopRules() {
        QFETCH(bool,laptopPresent); QTemporaryDir d;
        if (laptopPresent) {
            write(d.path(),"/class/power_supply/BAT0/type","Battery");
            write(d.path(),"/class/power_supply/BAT0/scope","System");
            write(d.path(),"/class/power_supply/BAT0/status","Discharging");
            write(d.path(),"/class/power_supply/BAT0/capacity","12");
            write(d.path(),"/class/power_supply/BAT0/power_now","7000000");
        }
        write(d.path(),"/class/power_supply/mouse/type","Battery");
        write(d.path(),"/class/power_supply/mouse/scope","Device");
        write(d.path(),"/class/power_supply/mouse/status","Discharging");
        write(d.path(),"/class/power_supply/mouse/capacity","95");
        write(d.path(),"/class/power_supply/mouse/power_now","100000");
        write(d.path(),"/class/power_supply/peripheral-usb/type","USB");
        write(d.path(),"/class/power_supply/peripheral-usb/scope","Device");
        write(d.path(),"/class/power_supply/peripheral-usb/online","1");
        const auto sample=TelemetryReader(d.path()).read();
        QCOMPARE(sample.batteryPresent,laptopPresent);
        QCOMPARE(sample.percent,laptopPresent ? 12 : -1);
        QCOMPARE(sample.externalPower.has_value(),laptopPresent);
        if (laptopPresent) {
            QVERIFY(!*sample.externalPower); QVERIFY(sample.powerW); QCOMPARE(*sample.powerW,7.0);
            RuleEngine rules; Settings settings; settings.autoProfile=true;
            QVERIFY(!rules.observe(sample.externalPower,sample.percent,settings));
            const auto desired=rules.observe(sample.externalPower,sample.percent,settings);
            QVERIFY(desired && desired->profile); QCOMPARE(*desired->profile,settings.lowProfile);
        } else QVERIFY(!sample.powerW);
    }
    void batteryCurrentTimesVoltage() {
        QTemporaryDir d;
        write(d.path(),"/class/power_supply/BAT0/type","Battery");
        write(d.path(),"/class/power_supply/BAT0/status","Discharging");
        write(d.path(),"/class/power_supply/BAT0/current_now","-1200000");
        write(d.path(),"/class/power_supply/BAT0/voltage_now","12000000");
        write(d.path(),"/class/power_supply/BAT0/capacity","70");
        const auto s=TelemetryReader(d.path()).read();
        QVERIFY(s.batteryPresent); QVERIFY(s.externalPower.has_value()); QVERIFY(!*s.externalPower);
        QCOMPARE(*s.powerW,14.4); QCOMPARE(s.percent,70);
    }
    void batteryPowerPrecedenceAndCharging() {
        QTemporaryDir d;
        write(d.path(),"/class/power_supply/BAT0/type","Battery"); write(d.path(),"/class/power_supply/BAT0/status","Charging");
        write(d.path(),"/class/power_supply/BAT0/power_now","2500000"); write(d.path(),"/class/power_supply/BAT0/current_now","1000000");
        write(d.path(),"/class/power_supply/BAT0/voltage_now","12000000");
        const auto s=TelemetryReader(d.path()).read(); QVERIFY(*s.externalPower); QCOMPARE(*s.powerW,2.5); QCOMPARE(s.batteryStatus,QString("Charging"));
    }
    void batteryMissingIsNotZero() {
        QTemporaryDir d; write(d.path(),"/class/power_supply/BAT0/type","Battery");
        write(d.path(),"/class/power_supply/BAT0/status","Not charging");
        const auto s=TelemetryReader(d.path()).read(); QVERIFY(!s.powerW); QVERIFY(!s.externalPower); QCOMPARE(s.percent,-1);
        QVERIFY(s.toJson()["battery_power_w"].isNull());
    }
    void chargerOnlineOverridesBatteryFallback() {
        QTemporaryDir d;
        write(d.path(),"/class/power_supply/BAT0/type","Battery"); write(d.path(),"/class/power_supply/BAT0/status","Not charging");
        write(d.path(),"/class/power_supply/AC0/type","Mains"); write(d.path(),"/class/power_supply/AC0/online","1");
        write(d.path(),"/class/power_supply/USB0/type","USB"); write(d.path(),"/class/power_supply/USB0/online","0");
        QVERIFY(*TelemetryReader(d.path()).read().externalPower);
    }
    void gpuCountersAreMilliseconds() {
        QTemporaryDir d;
        const QString p="/bus/pci/devices/0000:01:00.0";
        write(d.path(),p+"/vendor","0x10de"); write(d.path(),p+"/class","0x030000");
        write(d.path(),p+"/power/runtime_status","suspended"); write(d.path(),p+"/power_state","D3cold");
        write(d.path(),p+"/power/runtime_suspended_time","12500");
        const auto s=TelemetryReader(d.path()).read(); QCOMPARE(s.gpuPower,QString("D3cold")); QCOMPARE(*s.gpuSuspendedSeconds,12.5);
    }
    void gpuHwmonNeverReadAsCpu() {
        QTemporaryDir d; write(d.path(),"/class/hwmon/hwmon0/name","nvidia"); write(d.path(),"/class/hwmon/hwmon0/temp1_input","90000");
        QVERIFY(!TelemetryReader(d.path()).read().cpuC);
        write(d.path(),"/class/hwmon/hwmon1/name","coretemp"); write(d.path(),"/class/hwmon/hwmon1/temp1_input","48000");
        QCOMPARE(*TelemetryReader(d.path()).read().cpuC,48.0);
    }
    void emptySysfsIsSupported() {
        QTemporaryDir d; const auto s=TelemetryReader(d.path()).read(); QVERIFY(!s.batteryPresent); QVERIFY(!s.externalPower); QVERIFY(s.gpuPower.isEmpty());
    }
    void schemaOneMigrationPreservesExistingRules() {
        Settings s; s.autoProfile=true; s.autoRefresh=true; s.batteryProfile="balanced";
        auto old=s.toJson(); old["schema"]=1;
        old.remove("coolercontrol");
        for (const auto &k : {"auto_presets","presets","ac_preset","battery_preset","low_preset"}) old.remove(k);
        const auto next=Settings::fromJson(old);
        QVERIFY(next.autoProfile); QVERIFY(next.autoRefresh); QVERIFY(!next.autoPresets);
        QCOMPARE(next.batteryProfile,QString("balanced")); QCOMPARE(next.toJson()["schema"].toInt(),3);
        QCOMPARE(next.cpuFunction.toJson(),s.cpuFunction.toJson());
        old["unexpected"]=true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(old));
    }
    void presetsRoundTripAndKeepValues() {
        Settings s; s.presets.append({"custom","Мой профиль","",0,{"keep",2400,-1}});
        const auto restored=Settings::fromJson(s.toJson());
        QCOMPARE(restored.preset("custom").toJson(),s.preset("custom").toJson());
        QVERIFY(!restored.preset("custom").cpu.changesHardware());
        QVERIFY(!restored.autoPresets);
    }
    void schemaOneBackupOnFirstSave() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        auto legacy=Settings{}.toJson(); legacy["schema"]=1;
        legacy.remove("coolercontrol");
        for (const auto &k : {"auto_presets","presets","ac_preset","battery_preset","low_preset"}) legacy.remove(k);
        const auto bytes=QJsonDocument(legacy).toJson(); write("",path,bytes);
        auto s=Settings::load(path); s.save(path);
        QFile backup(path+".schema1.bak"); QVERIFY(backup.open(QIODevice::ReadOnly)); QCOMPARE(backup.readAll(),bytes);
        QVERIFY(!(backup.permissions()&QFileDevice::ReadOther));
        QCOMPARE(Settings::load(path).toJson()["schema"].toInt(),3);
        s.presets[0].name="Другое имя"; s.save(path);
        backup.seek(0); QCOMPARE(backup.readAll(),bytes);
    }
    void invalidPresetReferencesAndDuplicates() {
        auto j=Settings{}.toJson(); j["ac_preset"]="missing";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
        j=Settings{}.toJson(); auto list=j["presets"].toArray(); list.append(list.first()); j["presets"]=list;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
        j=Settings{}.toJson(); j["auto_presets"]=true; j["auto_refresh"]=true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
        j=Settings{}.toJson(); j["presets"]=QJsonArray{};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Settings::fromJson(j));
    }
    void invalidPresetName() {
        auto j=defaultPresets().first().toJson(); j["name"]="   ";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Preset::fromJson(j));
        j=defaultPresets().first().toJson(); j["id"]="../policy0";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,Preset::fromJson(j));
    }
    void compositeRulesDebounceAndLowPriority() {
        RuleEngine r; Settings s; s.autoPresets=true;
        QVERIFY(!r.observe(false,50,s)); auto p=r.observe(false,50,s);
        QVERIFY(p && p->preset); QCOMPARE(p->preset->id,QString("battery"));
        QVERIFY(!p->profile); QVERIFY(!p->hz); QCOMPARE(p->preset->hz,60); QCOMPARE(p->preset->cpu.maxMHz,2400);
        QVERIFY(!r.observe(false,10,s)); QCOMPARE(r.observe(false,10,s)->preset->id,QString("low"));
        QVERIFY(!r.observe(true,10,s)); QCOMPARE(r.observe(true,10,s)->preset->id,QString("ac"));
        QVERIFY(!r.observe(std::nullopt,10,s)); QVERIFY(!r.observe(false,-1,s));
        QCOMPARE(r.observe(false,-1,s)->preset->id,QString("battery"));
    }
    void cpuRequestValidation_data() {
        QTest::addColumn<QString>("key"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("mode")<<QString("mode")<<QJsonValue("../../etc/passwd");
        QTest::newRow("too-low")<<QString("max_mhz")<<QJsonValue(100);
        QTest::newRow("too-high")<<QString("max_mhz")<<QJsonValue(10001);
        QTest::newRow("fraction")<<QString("max_mhz")<<QJsonValue(2400.5);
        QTest::newRow("bool")<<QString("max_mhz")<<QJsonValue(true);
        QTest::newRow("turbo")<<QString("turbo")<<QJsonValue(2);
        QTest::newRow("unknown")<<QString("path")<<QJsonValue("/sys/anything");
    }
    void cpuRequestValidation() {
        QFETCH(QString,key); QFETCH(QJsonValue,value);
        auto j=CpuRequest{}.toJson(); j[key]=value;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,CpuRequest::fromJson(j));
    }
    void cpuEmptyIsUnsupported() {
        QTemporaryDir d; CpuController cpu(d.path());
        QVERIFY(!cpu.inspect()["supported"].toBool());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,cpu.apply({"limit",2400,-1}));
    }
    void cpuLimitPerPolicyHardwareMaximum() {
        QTemporaryDir d; cpuTree(d.path()); CpuController cpu(d.path());
        const auto result=cpu.apply({"limit",4000,-1}); QVERIFY(result["supported"].toBool());
        const auto list=cpu.policies(); QCOMPARE(list[0].currentMax,4000000); QCOMPARE(list[1].currentMax,3600000);
        cpu.apply({"hardware",2400,-1}); QCOMPARE(cpu.policies()[0].currentMax,4700000);
    }
    void cpuConfirmsAsynchronousPolicyUpdates_data() {
        QTest::addColumn<int>("scenario");
        QTest::newRow("limit-with-delayed-kernel-policy")<<0;
        QTest::newRow("remove-cap-with-delayed-kernel-policy")<<1;
        QTest::newRow("rollback-with-delayed-kernel-policy")<<2;
    }
    void cpuConfirmsAsynchronousPolicyUpdates() {
        QFETCH(int,scenario); QTemporaryDir d; cpuTree(d.path());
        if (scenario==1) for (int i=0;i<2;++i)
            write(d.path(),"/devices/system/cpu/cpufreq/policy"+QString::number(i)+"/scaling_max_freq","2400000");
        // A successful sysfs write queues ordered work; the policy readback is
        // deliberately unchanged until the simulated kernel worker completes.
        std::jthread kernelWork;
        int writes=0;
        CpuController cpu(d.path(),[&](const QString &path,const QByteArray &data) {
            ++writes;
            if (scenario==2 && writes==2) throw std::runtime_error("Injected second-policy denial");
            if (kernelWork.joinable()) kernelWork.join();
            kernelWork=std::jthread([path,data] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly|QIODevice::Truncate) || file.write(data)!=data.size() || !file.flush())
                    std::terminate();
            });
        });
        QString failure;
        try { cpu.apply({scenario==1 ? "hardware" : "limit",2400,-1}); }
        catch (const std::runtime_error &e) { failure=QString::fromUtf8(e.what()); }
        if (kernelWork.joinable()) kernelWork.join();
        if (scenario==2) {
            QVERIFY(failure.contains("Injected second-policy denial"));
            QVERIFY2(!failure.contains("НЕПОЛНЫЙ ОТКАТ"),qPrintable(failure));
            QCOMPARE(writes,4);
        } else { QVERIFY2(failure.isEmpty(),qPrintable(failure)); QCOMPARE(writes,2); }
        const auto policies=cpu.policies();
        QCOMPARE(policies[0].currentMax,scenario==0 ? 2400000 : 4700000);
        QCOMPARE(policies[1].currentMax,scenario==0 ? 2400000 : 3600000);
    }
    void cpuReadbackTimeoutReportsValuesWithoutWriteRetry() {
        QTemporaryDir d; cpuTree(d.path()); int writes=0;
        CpuController cpu(d.path(),[&](const QString &path,const QByteArray &data) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
            const auto value=++writes==1 ? QByteArray("2400001\n") : data;
            QCOMPARE(file.write(value),value.size()); QVERIFY(file.flush());
        });
        QElapsedTimer timer; timer.start(); QString failure;
        try { cpu.apply({"limit",2400,-1}); }
        catch (const std::runtime_error &e) { failure=QString::fromUtf8(e.what()); }
        QVERIFY2(failure.contains("ожидалось 2400000, прочитано 2400001"),qPrintable(failure));
        QVERIFY(failure.contains("выполнен откат")); QVERIFY(!failure.contains("НЕПОЛНЫЙ ОТКАТ"));
        QCOMPARE(writes,2); QVERIFY(timer.elapsed()>=250); QVERIFY(timer.elapsed()<2000);
        QCOMPARE(cpu.policies()[0].currentMax,4700000);
        QCOMPARE(cpu.policies()[1].currentMax,3600000);
    }
    void cpuPreflightDoesNotPartiallyWrite() {
        QTemporaryDir d; cpuTree(d.path());
        write(d.path(),"/devices/system/cpu/cpufreq/policy1/scaling_min_freq","3000000");
        int writes=0; CpuController cpu(d.path(),[&](const QString &,const QByteArray &){ ++writes; });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,cpu.apply({"limit",2400,-1})); QCOMPARE(writes,0);
    }
    void cpuKeepDoesNotWrite() {
        QTemporaryDir d; cpuTree(d.path()); int writes=0;
        CpuController cpu(d.path(),[&](const QString &,const QByteArray &){ ++writes; });
        cpu.apply({}); QCOMPARE(writes,0);
    }
    void cpuTurboAndLimit() {
        QTemporaryDir d; cpuTree(d.path()); CpuController cpu(d.path());
        QVERIFY(cpu.inspect()["turbo_allowed"].toBool());
        cpu.apply({"limit",2400,0}); QVERIFY(!cpu.inspect()["turbo_allowed"].toBool());
        QCOMPARE(cpu.policies()[0].currentMax,2400000);
        cpu.apply({"hardware",2400,1}); QVERIFY(cpu.inspect()["turbo_allowed"].toBool());
    }
    void cpuRollbackOnPartialWriteFailure() {
        QTemporaryDir d; cpuTree(d.path()); bool failOnce=true;
        CpuController cpu(d.path(),[&](const QString &path,const QByteArray &data){
            write("",path,data);
            if (path.contains("policy1/") && failOnce) { failOnce=false; throw std::runtime_error("Injected failure after write"); }
        });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,cpu.apply({"limit",2400,0}));
        const auto list=cpu.policies(); QCOMPARE(list[0].currentMax,4700000); QCOMPARE(list[1].currentMax,3600000);
        QVERIFY(cpu.inspect()["turbo_allowed"].toBool());
    }
    void cpuRollbackOnReadbackMismatch() {
        QTemporaryDir d; cpuTree(d.path()); int writes=0;
        CpuController cpu(d.path(),[&](const QString &,const QByteArray &){ ++writes; });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,cpu.apply({"limit",2400,-1}));
        QCOMPARE(writes,2); QCOMPARE(cpu.policies()[0].currentMax,4700000);
    }
    void cpuReportsRollbackFailure() {
        QTemporaryDir d; cpuTree(d.path());
        CpuController cpu(d.path(),[](const QString &,const QByteArray &){ throw std::runtime_error("Injected denial"); });
        try { cpu.apply({"limit",2400,-1}); QFAIL("Expected failure"); }
        catch (const std::runtime_error &e) { QVERIFY(QString::fromUtf8(e.what()).contains("НЕПОЛНЫЙ ОТКАТ")); }
    }
private:
    static void cpuTree(const QString &root) {
        for (int n=0;n<2;++n) {
            const QString p="/devices/system/cpu/cpufreq/policy"+QString::number(n)+"/";
            write(root,p+"scaling_driver","intel_pstate"); write(root,p+"cpuinfo_min_freq","400000");
            write(root,p+"cpuinfo_max_freq",n ? "3600000" : "4700000"); write(root,p+"scaling_min_freq","400000");
            write(root,p+"scaling_max_freq",n ? "3600000" : "4700000");
        }
        write(root,"/devices/system/cpu/intel_pstate/no_turbo","0");
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"