#include "reference_control.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace n14::reference {
namespace {

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

bool finite(double value) { return std::isfinite(value); }

bool validNumber(const QJsonValue &value, double low, double high) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return finite(number) && low <= number && number <= high;
}

bool exactKeys(const QJsonObject &object, const QStringList &expected) {
    const QStringList keys = object.keys();
    if (keys.size() != expected.size()) return false;
    for (const auto &key : expected) {
        if (!keys.contains(key)) return false;
    }
    return true;
}

double roundTiesToEven(double value) {
    require(finite(value) && value >= 0.0, "Некорректное неотрицательное число для округления");
    const double lower = std::floor(value);
    const double fraction = value - lower;
    if (fraction < 0.5) return lower;
    if (fraction > 0.5) return lower + 1.0;
    return std::fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0;
}

int boundedInteger(double value, int low, int high, const QString &message) {
    const double rounded = roundTiesToEven(value);
    require(rounded >= low && rounded <= high, message);
    return static_cast<int>(rounded);
}

int roundHalfUpInteger(double value, int low, int high, const QString &message) {
    require(finite(value) && value >= 0.0, "Некорректное неотрицательное число для округления");
    const double rounded = std::floor(value + 0.5);
    require(rounded >= low && rounded <= high, message);
    return static_cast<int>(rounded);
}

} // namespace

QString validateThresholdCurve(const ThresholdCurve &curve, double temperatureLimit) {
    if (curve.size() < 2 || curve.size() > MAX_THRESHOLD_ROWS)
        return "Нужно от 2 до 16 строк порогов";
    if (!finite(temperatureLimit) || temperatureLimit < 0.0)
        return "Некорректный предел температуры";

    for (qsizetype i = 0; i < curve.size(); ++i) {
        const auto &row = curve[i];
        if (!finite(row.upThreshold) || !finite(row.downThreshold) || !finite(row.fanSpeed) ||
            row.upThreshold < 0.0 || row.upThreshold > temperatureLimit ||
            row.downThreshold < 0.0 || row.downThreshold > temperatureLimit ||
            row.fanSpeed < NBFC_MIN_FAN_SPEED || row.fanSpeed > MAX_FAN_SPEED)
            return QString("Строка %1: число вне допустимого диапазона").arg(i + 1);
        if (row.downThreshold >= row.upThreshold)
            return QString("Строка %1: DownThreshold должен быть ниже UpThreshold").arg(i + 1);
        if (i == 0) {
            if (row.downThreshold != 0.0)
                return "Первая строка должна иметь DownThreshold = 0";
            continue;
        }
        const auto &previous = curve[i - 1];
        if (row.upThreshold <= previous.upThreshold || row.downThreshold <= previous.downThreshold)
            return QString("Строка %1: оба температурных столбца должны строго возрастать").arg(i + 1);
        if (row.downThreshold >= previous.upThreshold)
            return QString("Строка %1: DownThreshold должен быть ниже предыдущего UpThreshold").arg(i + 1);
        if (row.fanSpeed < previous.fanSpeed)
            return QString("Строка %1: скорости должны не убывать").arg(i + 1);
    }
    if (curve.last().fanSpeed != MAX_FAN_SPEED)
        return "Последняя строка должна задавать 100%";
    return {};
}

QString validateFanCurves(const FanCurves &curves) {
    const QString cpuError = validateThresholdCurve(curves.cpu, CPU_LIMIT_C);
    if (!cpuError.isEmpty()) return "CPU: " + cpuError;
    const QString gpuError = validateThresholdCurve(curves.gpu, GPU_LIMIT_C);
    if (!gpuError.isEmpty()) return "GPU: " + gpuError;
    return {};
}

QString validateCoolerControlGraph(const SpeedProfile &profile) {
    if (profile.size() < 2 || profile.size() > 256)
        return "CoolerControl Graph должен содержать от 2 до 256 точек";
    double previousTemperature = -1.0;
    for (qsizetype i = 0; i < profile.size(); ++i) {
        const auto &point = profile[i];
        if (!finite(point.temperature) || point.temperature < 0.0 || point.temperature > 120.0 ||
            point.duty < 0 || point.duty > 100)
            return QString("Точка %1: температура 0–120 °C, duty 0–100%").arg(i + 1);
        if (point.temperature <= previousTemperature)
            return "Температуры CoolerControl Graph должны строго возрастать";
        previousTemperature = point.temperature;
    }
    return {};
}

QString validateCoolerControlGraphs(const CoolerControlGraphs &graphs) {
    const QString cpuError = validateCoolerControlGraph(graphs.cpu);
    if (!cpuError.isEmpty()) return "CPU: " + cpuError;
    const QString gpuError = validateCoolerControlGraph(graphs.gpu);
    if (!gpuError.isEmpty()) return "GPU: " + gpuError;
    return {};
}

CoolerControlGraphs defaultCoolerControlGraphs() {
    // Read-only values from the current CoolerControl config.toml. Keep the
    // profile UIDs in the porting notes; this data is the graph itself, not an
    // NBFC threshold conversion.
    return {
        {{0.0, 0}, {60.0, 0}, {61.0, 10}, {75.0, 54}, {90.0, 100}, {100.0, 100}},
        {{0.0, 0}, {50.0, 0}, {65.0, 55}, {75.0, 100}, {100.0, 100}}
    };
}

FanCurves defaultFanCurves() {
    return {
        {
            {45.0, 0.0, 40.0}, {60.0, 40.0, 50.0}, {70.0, 55.0, 65.0},
            {80.0, 65.0, 80.0}, {90.0, 75.0, 100.0}
        },
        {
            {45.0, 0.0, 40.0}, {55.0, 40.0, 50.0}, {65.0, 50.0, 65.0},
            {72.0, 60.0, 80.0}, {80.0, 67.0, 100.0}
        }
    };
}

QJsonArray thresholdCurveJson(const ThresholdCurve &curve) {
    QJsonArray result;
    for (const auto &row : curve) {
        result.append(QJsonObject{
            {"UpThreshold", row.upThreshold},
            {"DownThreshold", row.downThreshold},
            {"FanSpeed", row.fanSpeed}
        });
    }
    return result;
}

QJsonObject fanCurvesJson(const FanCurves &curves) {
    return {{"CPU", thresholdCurveJson(curves.cpu)}, {"GPU", thresholdCurveJson(curves.gpu)}};
}

ThresholdCurve parseThresholdCurve(const QJsonValue &json, double temperatureLimit) {
    require(json.isArray(), "Кривая порогов должна быть массивом");
    ThresholdCurve result;
    for (const auto &value : json.toArray()) {
        require(value.isObject(), "Каждая строка порогов должна быть объектом");
        const auto object = value.toObject();
        require(exactKeys(object, {"UpThreshold", "DownThreshold", "FanSpeed"}),
                "Строка порогов содержит неизвестные или отсутствующие поля");
        require(validNumber(object["UpThreshold"], 0.0, temperatureLimit) &&
                    validNumber(object["DownThreshold"], 0.0, temperatureLimit) &&
                    validNumber(object["FanSpeed"], NBFC_MIN_FAN_SPEED, MAX_FAN_SPEED),
                "Строка порогов содержит некорректное число");
        result.append({object["UpThreshold"].toDouble(), object["DownThreshold"].toDouble(),
                       object["FanSpeed"].toDouble()});
    }
    const QString error = validateThresholdCurve(result, temperatureLimit);
    require(error.isEmpty(), error);
    return result;
}

FanCurves parseFanCurves(const QJsonValue &json) {
    require(json.isObject(), "Кривые должны быть объектом");
    const auto object = json.toObject();
    require(exactKeys(object, {"CPU", "GPU"}), "Кривые должны содержать только CPU и GPU");
    FanCurves result;
    result.cpu = parseThresholdCurve(object["CPU"], CPU_LIMIT_C);
    result.gpu = parseThresholdCurve(object["GPU"], GPU_LIMIT_C);
    return result;
}

ThresholdSelector::ThresholdSelector(ThresholdCurve curve) : curve_(std::move(curve)) {
    // The selector is used after role-specific validation. It intentionally
    // does not guess CPU/GPU limits from the rows: both limits are part of the
    // input contract, not runtime state.
    require(curve_.size() >= 2 && curve_.size() <= MAX_THRESHOLD_ROWS,
            "Некорректное количество строк порогов");
}

void ThresholdSelector::reset() { row_ = 0; }

double ThresholdSelector::update(double temperature) {
    require(finite(temperature), "Температура должна быть конечным числом");
    while (row_ > 0 && temperature <= curve_[row_].downThreshold) --row_;
    while (row_ < curve_.size() - 1 && temperature >= curve_[row_].upThreshold) ++row_;
    return curve_[row_].fanSpeed;
}

SpeedProfile normalizeSpeedProfile(const SpeedProfile &profile, double temperatureLimit) {
    require(!profile.isEmpty() && finite(temperatureLimit) && temperatureLimit >= 0.0,
            "Пустой или некорректный speed profile");
    for (const auto &point : profile) {
        require(finite(point.temperature) && point.duty >= 0 && point.duty <= 255,
                "Некорректная точка speed profile");
    }
    QVector<SpeedPoint> sorted = profile;
    sorted.append({temperatureLimit, 100});
    std::sort(sorted.begin(), sorted.end(), [](const SpeedPoint &a, const SpeedPoint &b) {
        if (a.temperature != b.temperature) return a.temperature < b.temperature;
        return a.duty > b.duty;
    });

    // The producer keeps the first sorted point verbatim, then applies its
    // cleanup/break rules to subsequent points (even if the first is 100%).
    SpeedProfile result{sorted.first()};
    int previousDuty = sorted.first().duty;
    for (qsizetype i = 1; i < sorted.size(); ++i) {
        const auto &point = sorted[i];
        if (point.temperature == result.last().temperature) continue;
        const int duty = std::max(previousDuty, std::min(100, point.duty));
        result.append({point.temperature, duty});
        previousDuty = duty;
        if (duty == 100) break;
    }
    return result;
}

int interpolateSpeedProfile(const SpeedProfile &normalized, double temperature) {
    require(!normalized.isEmpty() && finite(temperature), "Некорректный профиль или температура");
    if (temperature <= normalized.first().temperature) return normalized.first().duty;
    for (qsizetype i = 1; i < normalized.size(); ++i) {
        const auto &a = normalized[i - 1];
        const auto &b = normalized[i];
        if (temperature <= b.temperature) {
            const double fraction = (temperature - a.temperature) / (b.temperature - a.temperature);
            return roundHalfUpInteger(a.duty + fraction * (b.duty - a.duty), 0, 100,
                                      "Интерполированная скорость вне диапазона");
        }
    }
    return normalized.last().duty;
}

std::optional<int> curveDemand(const SpeedProfile &cpu, const SpeedProfile &gpu,
                               const ThermalInput &input, double cpuTemperatureLimit,
                               double gpuTemperatureLimit) {
    if (!input.cpuC || !finite(*input.cpuC)) return {};
    const auto normalizedCpu = normalizeSpeedProfile(cpu, cpuTemperatureLimit);
    QVector<int> duties{interpolateSpeedProfile(normalizedCpu, *input.cpuC)};
    if (input.gpuState == GpuState::Unknown) return {};
    if (input.gpuState == GpuState::Active) {
        if (!input.gpuC || !finite(*input.gpuC)) return {};
        const auto normalizedGpu = normalizeSpeedProfile(gpu, gpuTemperatureLimit);
        duties.append(interpolateSpeedProfile(normalizedGpu, *input.gpuC));
    }
    return *std::max_element(duties.cbegin(), duties.cend());
}

int fanDutyToRaw(int duty) {
    require(duty >= 0 && duty <= 100, "Duty должен быть целым числом от 0 до 100");
    return FAN_MIN_COMMAND + static_cast<int>(roundTiesToEven(
        duty * static_cast<double>(FAN_MAX_RAW - FAN_MIN_COMMAND) / 100.0));
}

int fanRawToDuty(int raw) {
    // Readback accepts the firmware's raw 0, while writes are separately
    // restricted to FAN_MIN_COMMAND (1). The published adapter clamps the
    // resulting percentage before applying Python round().
    require(raw >= 0 && raw <= FAN_MAX_RAW, "EC duty вне разрешённого диапазона");
    const double percent = std::clamp(
        (raw - FAN_MIN_COMMAND) * 100.0 /
            static_cast<double>(FAN_MAX_RAW - FAN_MIN_COMMAND),
        0.0, 100.0);
    return boundedInteger(percent, 0, 100, "EC duty вне разрешённого диапазона");
}

int keylightPercentToRaw(int percent) {
    require(percent >= 0 && percent <= 100, "Яркость должна быть от 0 до 100%");
    // Keep the source's integer arithmetic: 175 - (percent * 175 // 100).
    return KEYLIGHT_RAW_MAX - (percent * KEYLIGHT_RAW_MAX / 100);
}

int keylightRawToPercent(int raw) {
    require(raw >= 0 && raw <= KEYLIGHT_RAW_MAX, "Raw яркости вне разрешённого диапазона");
    // ceil((175 - raw) * 100 / 175), exactly as the Python integer formula.
    return ((KEYLIGHT_RAW_MAX - raw) * 100 + KEYLIGHT_RAW_MAX - 1) /
           KEYLIGHT_RAW_MAX;
}

} // namespace n14::reference