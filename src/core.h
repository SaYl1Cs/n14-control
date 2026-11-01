#pragma once
#include "cpu.h"
#include "reference_standard.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QVector>
#include <optional>

namespace n14 {

struct CurvePoint { double temperature; double duty; };
using Curve = QVector<CurvePoint>;
QString validateCurve(const Curve &curve);
double curveDuty(const Curve &curve, double temperature);
std::optional<double> mixDuty(const Curve &cpu, const Curve &gpu,
                             std::optional<double> cpuTemperature,
                             std::optional<double> gpuTemperature, bool gpuAsleep,
                             const QString &mix);
QJsonArray curveJson(const Curve &curve);
Curve parseCurve(const QJsonValue &json);

struct FunctionSettings {
    QString kind = "Identity";
    double deviance = 2;
    int window = 5;
    QJsonObject toJson() const;
    static FunctionSettings fromJson(const QJsonValue &json);
};
struct Preset {
    QString id, name;
    QString powerProfile; // empty = do not modify
    int hz = 0; // 0 = do not modify
    CpuRequest cpu;
    QJsonObject toJson() const;
    static Preset fromJson(const QJsonObject &json);
};
QVector<Preset> defaultPresets();
struct Settings {
    bool autoProfile = true;
    bool autoRefresh = false;
    QString acProfile = "performance";
    QString batteryProfile = "power-saver";
    QString lowProfile = "power-saver";
    int acHz = 165;
    int batteryHz = 60;
    int lowPercent = 15;
    bool autoPresets = false;
    QVector<Preset> presets = defaultPresets();
    QString acPreset = "ac", batteryPreset = "battery", lowPreset = "low";
    const Preset &preset(const QString &id) const;
    // Read-only copy of the current CoolerControl Graph profile.
    Curve cpu{{0,0},{60,0},{61,10},{75,54},{90,100},{100,100}};
    Curve gpu{{0,0},{50,0},{65,55},{75,100},{100,100}};
    QString mix = "Max";
    // Native CoolerControl contract. These are deliberately separate from the
    // legacy preview FunctionSettings above and are still reference-only.
    reference::StandardFunction coolerCpuFunction = reference::defaultCpuStandardFunction();
    reference::StandardFunction coolerGpuFunction = reference::defaultGpuStandardFunction();
    FunctionSettings cpuFunction;
    FunctionSettings gpuFunction;
    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &json);
    static Settings load(const QString &path);
    void save(const QString &path) const;
};

struct DesiredPolicy {
    std::optional<QString> profile;
    std::optional<int> hz;
    QString reason;
    std::optional<Preset> preset;
};

// Two consecutive observations are required, including after an unknown state.
class RuleEngine {
public:
    std::optional<DesiredPolicy> observe(std::optional<bool> externalPower,
                                        int percent, const Settings &settings);
    void reset();
private:
    QString candidate_;
    int confirmations_ = 0;
};

// Independent preview filters, NOT a claim of CoolerControl Standard parity.
class TemperatureFilter {
public:
    std::optional<double> update(std::optional<double> value, const QString &kind,
                                 double deviance, int window);
    void reset();
private:
    std::optional<double> accepted_;
    QVector<double> history_;
};

struct ScreenMode { QString id; int width = 0; int height = 0; double hz = 0; };
struct Panel {
    int id = 0;
    QString name;
    QString currentMode;
    QVector<ScreenMode> modes;
    std::optional<ScreenMode> modeForHz(int hz) const;
    std::optional<ScreenMode> current() const;
};
std::optional<Panel> parsePanel(const QByteArray &json);
QString profileLabel(const QString &profile);

} // namespace n14