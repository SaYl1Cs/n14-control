#pragma once
#include "backend.h"
#include "hardware.h"
#include "fan_client.h"
#include "telemetry.h"
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QSlider>
#include <QLineEdit>

namespace n14 {
class HistoryPlot;
class CurvePlot;
class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(QString settingsPath,bool demo,QWidget *parent=nullptr);
    bool trayAvailable() const;
    void showWindow();
private slots:
    void prepareForSleep(bool sleeping);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void buildOverview();
    void buildPower();
    void buildCooling();
    void buildSystem();
    void buildPresets();
    void buildHardware();
    void loadPresetEditor();
    void syncPresetUi();
    void savePreset();
    void savePresetRules();
    void duplicatePreset();
    void deletePreset();
    void manualPreset();
    void startPreset(const Preset &preset,bool interactive);
    void applyPresetCpu();
    void applyPresetScreen();
    void finishPreset(bool success);
    void refreshHardwareLabel();
    void refreshFanLabels();
    bool commitSettings(const Settings &next,const QString &message);
    void buildTray();
    void sample();
    void acceptSample(Snapshot snapshot);
    void applyRules();
    void saveRules();
    void syncRulesUi();
    void saveCurves();
    void preview();
    void exportCsv();
    void refreshBackendLabels();
    void log(const QString &message,bool error=false);
    void manualProfile(const QString &profile);
    void manualHz(int hz);
    QTableWidget *curveTable(const Curve &curve,QWidget *parent);
    Curve curveFromTable(QTableWidget *table) const;
    QString settingsPath_;
    Settings settings_;
    bool demo_;
    bool sleeping_=false;
    bool hardwareWasAvailable_=false;
    DesktopBackend backend_;
    HardwareBackend hardware_;
    FanClient fan_;
    TelemetryPoller telemetry_{this};
    quint64 telemetryRevision_=0;
    bool telemetryFailed_=false;
    RuleEngine rules_;
    Snapshot snapshot_;
    QVector<Snapshot> history_;
    QTimer sampleTimer_,previewTimer_;
    QString attemptedProfile_,attemptedHz_;
    QString attemptedPreset_;
    std::optional<Preset> pendingPreset_;
    bool presetInteractive_=false;
    enum class PresetStage { Idle,Power,Cpu,Screen };
    PresetStage presetStage_=PresetStage::Idle;
    QTabWidget *tabs_;
    QSystemTrayIcon *tray_=nullptr;
    QLabel *batteryValue_,*powerValue_,*gpuValue_,*cpuValue_,*average_,*profileValue_,*screenValue_,*diagnostics_;
    HistoryPlot *historyPlot_;
    QPlainTextEdit *journal_=nullptr;
    QCheckBox *autoProfile_,*autoHz_;
    QComboBox *acProfile_,*batteryProfile_,*lowProfile_,*acHz_,*batteryHz_;
    QSpinBox *lowPercent_;
    QTableWidget *cpuTable_,*gpuTable_;
    QComboBox *mix_;
    QComboBox *filterKind_[2];
    QDoubleSpinBox *deviance_[2];
    QSpinBox *window_[2];
    QSpinBox *standardStepMinimum_[2], *standardStepMaximum_[2];
    QSpinBox *standardStepMinimumDecreasing_[2], *standardStepMaximumDecreasing_[2];
    QSpinBox *standardResponseDelay_[2];
    QDoubleSpinBox *standardDeviance_[2];
    QCheckBox *standardOnlyDownward_[2], *standardThresholdHopping_[2], *standardExtremeBypass_[2];
    TemperatureFilter filters_[2];
    QSlider *cpuInput_,*gpuInput_;
    QCheckBox *gpuAsleep_;
    QLabel *previewValue_=nullptr;
    CurvePlot *curvePlot_=nullptr;
    QComboBox *presetChoice_,*presetPower_,*presetHz_,*presetCpuMode_,*presetTurbo_;
    QComboBox *acPreset_,*batteryPreset_,*lowPreset_;
    QLineEdit *presetName_;
    QSpinBox *presetMaxMHz_,*presetLowPercent_;
    QCheckBox *autoPresets_;
    QCheckBox *autoEc_=nullptr;
    QCheckBox *manualFans_=nullptr;
    QLabel *hardwareInfo_=nullptr,*presetStatus_=nullptr;
    QSlider *keylightSlider_=nullptr;
    QLabel *keylightValue_=nullptr;
    QSlider *fanSlider_=nullptr;
    QLabel *fanValue_=nullptr,*fanStatus_=nullptr;
};
} // namespace n14