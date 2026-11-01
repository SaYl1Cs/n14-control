#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace n14::reference {

// These limits and row semantics are the published Colorful N14 profile
// contract. They are deliberately separate from the legacy preview Curve type
// in core.h, which is not CoolerControl-compatible.
constexpr double CPU_LIMIT_C = 90.0;
constexpr double GPU_LIMIT_C = 80.0;
constexpr int MAX_THRESHOLD_ROWS = 16;
constexpr double NBFC_MIN_FAN_SPEED = 40.0;
constexpr double MAX_FAN_SPEED = 100.0;

struct ThresholdRow {
    double upThreshold = 0.0;
    double downThreshold = 0.0;
    double fanSpeed = 0.0;
};
using ThresholdCurve = QVector<ThresholdRow>;

struct FanCurves {
    ThresholdCurve cpu;
    ThresholdCurve gpu;
};

QString validateThresholdCurve(const ThresholdCurve &curve, double temperatureLimit);
QString validateFanCurves(const FanCurves &curves);
FanCurves defaultFanCurves();

QJsonArray thresholdCurveJson(const ThresholdCurve &curve);
QJsonObject fanCurvesJson(const FanCurves &curves);
ThresholdCurve parseThresholdCurve(const QJsonValue &json, double temperatureLimit);
FanCurves parseFanCurves(const QJsonValue &json);

// Published NBFC selection semantics: Down moves back from the current row,
// Up moves to the next row. The selector owns the current row across samples;
// this is not stateless interpolation.
class ThresholdSelector {
public:
    explicit ThresholdSelector(ThresholdCurve curve);

    void reset();
    int row() const { return row_; }
    double update(double temperature);

private:
    ThresholdCurve curve_;
    int row_ = 0;
};

struct SpeedPoint {
    double temperature = 0.0;
    int duty = 0;
};
using SpeedProfile = QVector<SpeedPoint>;

// CoolerControl Graph profiles are a separate contract from the published
// NBFC threshold table above. Their duty range is the full 0..100%, including
// an intentional zero-duty point.
struct CoolerControlGraphs {
    SpeedProfile cpu;
    SpeedProfile gpu;
};

QString validateCoolerControlGraph(const SpeedProfile &profile);
QString validateCoolerControlGraphs(const CoolerControlGraphs &graphs);
CoolerControlGraphs defaultCoolerControlGraphs();

// This is the producer's graph normalization, not the NBFC threshold table.
// It adds the source device's failsafe point, sorts by temperature (higher duty
// wins ties), removes duplicate temperatures, prevents duty from decreasing,
// and stops at the first subsequent 100% point. The first sorted point is kept
// verbatim, matching the producer even when that point already requests 100%.
SpeedProfile normalizeSpeedProfile(const SpeedProfile &profile, double temperatureLimit);
int interpolateSpeedProfile(const SpeedProfile &normalized, double temperature);

enum class GpuState {
    Unknown,
    Active,
    Suspended,
};

struct ThermalInput {
    std::optional<double> cpuC;
    std::optional<double> gpuC;
    GpuState gpuState = GpuState::Unknown;
};

// The transition/producer floor used by the pinned source. Missing CPU data,
// or missing active-GPU data, returns no duty. A confirmed suspended GPU is
// omitted; its temperature is never replaced with CPU temperature.
std::optional<int> curveDemand(const SpeedProfile &cpu, const SpeedProfile &gpu,
                               const ThermalInput &input, double cpuTemperatureLimit,
                               double gpuTemperatureLimit);

// Exact integer conversions from coolercontrol_adapter.py. Python round() is
// ties-to-even; using std::round here would change duty 50 at the EC boundary.
constexpr int FAN_MIN_COMMAND = 1;
constexpr int FAN_MAX_RAW = 70;
constexpr int KEYLIGHT_RAW_MAX = 175;

int fanDutyToRaw(int duty);
int fanRawToDuty(int raw);
int keylightPercentToRaw(int percent);
int keylightRawToPercent(int raw);

} // namespace n14::reference