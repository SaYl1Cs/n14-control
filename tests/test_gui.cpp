#include "window.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QStyleOptionSlider>
using namespace n14;

class GuiTests : public QObject {
    Q_OBJECT
private slots:
    void demoDoesNotWriteOnStartup() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Window window(path,true); window.show(); QTest::qWait(30);
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(window.findChild<QTabWidget *>()->count(),6);
        const auto boxes=window.findChildren<QCheckBox *>();
        QVERIFY(boxes.size()>=3);
        QVERIFY(window.findChild<QCheckBox *>("autoProfile")->isChecked());
        QVERIFY(!window.findChild<QCheckBox *>("autoHz")->isChecked());
        QVERIFY(!window.findChild<QCheckBox *>("autoPresets")->isChecked());
        QVERIFY(window.findChild<QCheckBox *>("autoEc")->isChecked());
    }
    void rulesPersistIndependently() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Window w(path,true);
        w.findChild<QCheckBox *>("autoProfile")->setChecked(true);
        auto s=Settings::load(path); QVERIFY(s.autoProfile); QVERIFY(!s.autoRefresh);
        w.findChild<QCheckBox *>("autoHz")->setChecked(true);
        s=Settings::load(path); QVERIFY(s.autoProfile); QVERIFY(s.autoRefresh);
    }
    void manualProfileDisablesOnlyProfileAutomation() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Window w(path,true);
        w.findChild<QCheckBox *>("autoProfile")->setChecked(true);
        w.findChild<QCheckBox *>("autoHz")->setChecked(true);
        w.findChild<QPushButton *>("manual-balanced")->click();
        const auto s=Settings::load(path); QVERIFY(!s.autoProfile); QVERIFY(s.autoRefresh);
    }
    void resumeSelectsBatteryPresetInsteadOfPreSleepAcPreset() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Settings settings; settings.autoProfile=false; settings.autoRefresh=false;
        settings.autoPresets=false; settings.save(path);
        Window w(path,true);
        auto *choice=w.findChild<QComboBox *>("presetChoice");
        auto *status=w.findChild<QLabel *>("presetStatus");
        auto *automatic=w.findChild<QCheckBox *>("autoPresets");
        QVERIFY(choice && status && automatic);
        choice->setCurrentIndex(choice->findData("ac"));
        w.findChild<QPushButton *>("applyPreset")->click();
        const QString acConfirmed="Пресет подтверждён: "+settings.preset("ac").name;
        const QString batteryConfirmed="Пресет подтверждён: "+settings.preset("battery").name;
        QCOMPARE(status->text(),acConfirmed);
        QVERIFY(QMetaObject::invokeMethod(&w,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        automatic->setChecked(true);
        QTest::qWait(50); QCOMPARE(status->text(),acConfirmed);
        QVERIFY(QMetaObject::invokeMethod(&w,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        // Demo telemetry is now on battery. The old AC result must not be
        // replayed, and the first post-resume read alone grants no transition.
        QTest::qWait(1000); QCOMPARE(status->text(),acConfirmed);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(),batteryConfirmed,2500);
        const auto saved=Settings::load(path);
        QVERIFY(saved.autoPresets); QVERIFY(!saved.autoProfile); QVERIFY(!saved.autoRefresh);
    }
    void invalidCurveCannotBeSaved() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Window w(path,true);
        auto *table=w.findChild<QTableWidget *>("cpuCurve"); QVERIFY(table);
        table->item(0,0)->setText("not a number");
        w.findChild<QPushButton *>("saveCurves")->click();
        QVERIFY(!QFileInfo::exists(path));
        table->item(0,0)->setText("0");
        w.findChild<QPushButton *>("saveCurves")->click();
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(Settings::load(path).cpu.first().temperature,0.0);
    }
    void nativeCoolerControlFunctionSettingsPersistSeparately() {
        QTemporaryDir d; const QString path=d.path()+"/config.json";
        Window w(path,true);
        auto *step=w.findChild<QSpinBox *>("coolerCpuStepMinimum"); QVERIFY(step);
        auto *delay=w.findChild<QSpinBox *>("coolerCpuResponseDelay"); QVERIFY(delay);
        auto *onlyDown=w.findChild<QCheckBox *>("coolerCpuOnlyDownward"); QVERIFY(onlyDown);
        auto *hopping=w.findChild<QCheckBox *>("coolerGpuThresholdHopping"); QVERIFY(hopping);
        step->setValue(7); delay->setValue(4); onlyDown->setChecked(true); hopping->setChecked(false);
        w.findChild<QPushButton *>("saveCurves")->click();
        const auto settings=Settings::load(path);
        QCOMPARE(settings.coolerCpuFunction.stepMinimum,7);
        QCOMPARE(settings.coolerCpuFunction.responseDelay,4);
        QVERIFY(settings.coolerCpuFunction.onlyDownward);
        QVERIFY(!settings.coolerGpuFunction.thresholdHopping);
        QVERIFY(settings.cpuFunction.kind=="Identity");
        QCOMPARE(settings.toJson()["schema"].toInt(),3);
    }
    void demoBackendDoesNotIssueRealRequests() {
        DesktopBackend backend(true); QSignalSpy profile(&backend,&DesktopBackend::profileChanged);
        QSignalSpy screen(&backend,&DesktopBackend::panelChanged);
        backend.setProfile("power-saver"); QCOMPARE(backend.activeProfile(),QString("power-saver")); QCOMPARE(profile.count(),1);
        backend.setHz(60); QCOMPARE(backend.panel()->current()->hz,60.0); QCOMPARE(screen.count(),1);
        backend.setHz(165); QCOMPARE(backend.panel()->current()->hz,165.0);
        backend.setHz(144); QCOMPARE(backend.panel()->current()->hz,165.0);
        backend.setProfile("bogus"); QCOMPARE(backend.activeProfile(),QString("power-saver"));
    }
    void demoBackendTracksOnlyApplicationProfile() {
        DesktopBackend backend(true);
        QVERIFY(backend.profileMatches("balanced"));
        backend.setProfile("performance");
        QVERIFY(backend.profileMatches("performance"));
        QVERIFY(!backend.profileMatches("balanced"));
    }
    void keylightIsControlledBySlider() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        auto *slider=w.findChild<QSlider *>("keylightSlider"); QVERIFY(slider);
        QCOMPARE(slider->minimum(),0); QCOMPARE(slider->maximum(),100);
        slider->setValue(73); QCOMPARE(slider->value(),73);
        QVERIFY(w.findChildren<QPushButton *>("keylight50").isEmpty());
        QVERIFY(w.findChildren<QPushButton *>("keylight100").isEmpty());
    }
    void keylightMouseReleaseUsesCommittedValueNotPreviousValue() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        auto *slider=w.findChild<QSlider *>("keylightSlider");
        auto *client=w.findChild<FanClient *>(); QVERIFY(slider); QVERIFY(client);
        QSignalSpy notices(client,&FanClient::notice);
        const int previous=slider->value();
        slider->setSliderDown(true); slider->setSliderPosition(73);
        QCOMPARE(slider->value(),previous); // Tracking is off until release.
        slider->setSliderDown(false);
        QCOMPARE(slider->value(),73);
        QTRY_COMPARE(notices.count(),1);
        QVERIFY2(notices.first()[0].toString().contains("73%"),qPrintable(notices.first()[0].toString()));
    }
    void keylightKeyboardAndBurstsCoalesceWithoutMouseRelease() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        auto *slider=w.findChild<QSlider *>("keylightSlider");
        auto *client=w.findChild<FanClient *>(); QVERIFY(slider); QVERIFY(client);
        QSignalSpy notices(client,&FanClient::notice);
        slider->setValue(20); slider->setValue(30); slider->setValue(40);
        slider->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        QCOMPARE(slider->value(),41);
        QTRY_COMPARE(notices.count(),1);
        QVERIFY(notices.first()[0].toString().contains("41%"));
        QTest::qWait(200); QCOMPARE(notices.count(),1);
    }
    void keylightRealMouseSequenceUsesEveryNewCommittedValueDespiteCachedReadback() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        w.findChild<QTabWidget *>()->setCurrentIndex(4); w.show(); QTest::qWait(30);
        auto *slider=w.findChild<QSlider *>("keylightSlider");
        auto *client=w.findChild<FanClient *>(); QVERIFY(slider); QVERIFY(client);
        { const QSignalBlocker blocker(slider); slider->setValue(0); }
        QSignalSpy notices(client,&FanClient::notice);
        const auto handle=[slider](int value) {
            QStyleOptionSlider option; option.initFrom(slider);
            option.orientation=Qt::Horizontal; option.minimum=0; option.maximum=100;
            option.sliderPosition=value; option.sliderValue=value;
            return slider->style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,slider).center();
        };
        int count=0;
        for (int value : {100,0,100,25,75,0}) {
            const int previous=slider->value();
            QTest::mousePress(slider,Qt::LeftButton,Qt::NoModifier,handle(previous));
            QVERIFY(slider->isSliderDown());
            QTest::mouseMove(slider,handle(value));
            QCOMPARE(slider->value(),previous);
            const auto stale=QString::fromUtf8(QJsonDocument(QJsonObject{
                {"schema",1},{"ready",true},{"mode","curve"},{"status",QJsonObject{
                    {"fan_mode",2},{"cpu_target_raw",42},{"gpu_target_raw",42},{"cpu_tach_raw",35},
                    {"keylight_raw",reference::keylightPercentToRaw(previous)}}}
            }).toJson(QJsonDocument::Compact));
            QVERIFY(QMetaObject::invokeMethod(client,"acceptState",Qt::DirectConnection,Q_ARG(QString,stale)));
            QTest::mouseRelease(slider,Qt::LeftButton,Qt::NoModifier,handle(value));
            QCOMPARE(slider->value(),value);
            QVERIFY(QMetaObject::invokeMethod(client,"acceptState",Qt::DirectConnection,Q_ARG(QString,stale)));
            QCOMPARE(slider->value(),value);
            ++count; QTRY_COMPARE(notices.count(),count);
            QCOMPARE(notices.last()[0].toString(),QString("Демо: подсветка %1% изменена только в симуляции").arg(value));
        }
        QTest::qWait(200); QCOMPARE(notices.count(),count);
    }
    void keylightCachedReadbackCannotOverwritePendingUserValue() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        auto *slider=w.findChild<QSlider *>("keylightSlider");
        auto *client=w.findChild<FanClient *>(); QVERIFY(slider); QVERIFY(client);
        QSignalSpy notices(client,&FanClient::notice);
        slider->setValue(37); QVERIFY(client->keylightPending());
        const auto json=QString::fromUtf8(QJsonDocument(QJsonObject{
            {"schema",1},{"ready",true},{"mode","curve"},{"status",QJsonObject{
                {"fan_mode",2},{"cpu_target_raw",42},{"gpu_target_raw",42},{"cpu_tach_raw",35},{"keylight_raw",0}}}
        }).toJson(QJsonDocument::Compact));
        QVERIFY(QMetaObject::invokeMethod(client,"acceptState",Qt::DirectConnection,Q_ARG(QString,json)));
        QCOMPARE(slider->value(),37);
        QTRY_COMPARE(notices.count(),1); QVERIFY(notices.first()[0].toString().contains("37%"));
    }
    void manualFanModeOwnsSliderAndDisablesCurves() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true);
        auto *manual=w.findChild<QCheckBox *>("manualFans"); QVERIFY(manual);
        auto *slider=w.findChild<QSlider *>("fanSlider"); QVERIFY(slider);
        auto *curves=w.findChild<QCheckBox *>("autoEc"); QVERIFY(curves);
        QVERIFY(w.findChild<QPushButton *>("acquireEc")==nullptr);
        QVERIFY(w.findChild<QPushButton *>("releaseEc")==nullptr);
        QCOMPARE(slider->minimum(),0); QCOMPARE(slider->maximum(),100);
        QVERIFY(!manual->isChecked()); QVERIFY(!slider->isEnabled()); QVERIFY(curves->isChecked());
        manual->setChecked(true);
        QVERIFY(slider->isEnabled()); QVERIFY(!curves->isChecked());
        slider->setValue(72); QCOMPARE(slider->value(),72);
        manual->setChecked(false);
        QVERIFY(!slider->isEnabled()); QVERIFY(curves->isChecked());
    }
    void allTabsRender() {
        QTemporaryDir d; Window w(d.path()+"/config.json",true); w.show();
        auto *tabs=w.findChild<QTabWidget *>();
        for (int i=0;i<tabs->count();++i) { tabs->setCurrentIndex(i); QTest::qWait(20); QVERIFY(!w.grab().isNull()); }
    }
    void presetSaveDoesNotApplyHardware() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QLineEdit *>("presetName")->setText("Мой тихий");
        auto *mode=w.findChild<QComboBox *>("presetCpuMode"); mode->setCurrentIndex(mode->findData("limit"));
        w.findChild<QSpinBox *>("presetMaxMHz")->setValue(2100);
        w.findChild<QPushButton *>("savePreset")->click();
        const auto saved=Settings::load(path); QCOMPARE(saved.preset("ac").name,QString("Мой тихий"));
        QCOMPARE(saved.preset("ac").cpu.maxMHz,2100); QVERIFY(!saved.autoPresets);
        QVERIFY(w.findChild<QLabel *>("hardwareInfo")->text().contains("4700"));
    }
    void presetAutomationExclusiveWithLegacyRules() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QCheckBox *>("autoHz")->setChecked(true);
        w.findChild<QCheckBox *>("autoPresets")->setChecked(true);
        auto s=Settings::load(path); QVERIFY(s.autoPresets); QVERIFY(!s.autoRefresh); QVERIFY(!s.autoProfile);
        QVERIFY(!w.findChild<QCheckBox *>("autoHz")->isChecked());
        w.findChild<QCheckBox *>("autoProfile")->setChecked(true);
        s=Settings::load(path); QVERIFY(s.autoProfile); QVERIFY(!s.autoPresets);
        QVERIFY(!w.findChild<QCheckBox *>("autoPresets")->isChecked());
    }
    void presetDuplicateAndDelete() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QPushButton *>("duplicatePreset")->click(); QCOMPARE(Settings::load(path).presets.size(),4);
        w.findChild<QPushButton *>("deletePreset")->click(); QCOMPARE(Settings::load(path).presets.size(),3);
        w.findChild<QPushButton *>("deletePreset")->click(); QCOMPARE(Settings::load(path).presets.size(),3);
    }
    void manualPresetPipelineAndAutomationDisable() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        auto *choices=w.findChild<QComboBox *>("presetChoice"); choices->setCurrentIndex(choices->findData("battery"));
        w.findChild<QCheckBox *>("autoPresets")->setChecked(true);
        w.findChild<QPushButton *>("applyPreset")->click();
        const auto s=Settings::load(path); QVERIFY(!s.autoPresets && !s.autoProfile && !s.autoRefresh);
        QVERIFY(w.findChild<QLabel *>("presetStatus")->text().contains("Пресет подтверждён"));
        QVERIFY(w.findChild<QLabel *>("hardwareInfo")->text().contains("2400"));
    }
    void manualLegacyActionDisablesCompositeAutomation() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QCheckBox *>("autoPresets")->setChecked(true);
        w.findChild<QPushButton *>("manual-balanced")->click(); QVERIFY(!Settings::load(path).autoPresets);
    }
    void invalidPresetDoesNotOverwriteConfiguration() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QPushButton *>("savePreset")->click(); const auto before=Settings::load(path).toJson();
        w.findChild<QLineEdit *>("presetName")->setText("   "); w.findChild<QPushButton *>("savePreset")->click();
        QCOMPARE(Settings::load(path).toJson(),before);
    }
    void demoHardwareBackendIsIsolated() {
        HardwareBackend backend(true); QSignalSpy done(&backend,&HardwareBackend::cpuFinished);
        backend.applyCpu({"limit",1800,0},false); QCOMPARE(done.count(),1); QVERIFY(done.last()[0].toBool());
        QCOMPARE(backend.state()["policies"].toArray().first().toObject()["max_khz"].toInt(),1800000);
        QVERIFY(!backend.state()["turbo_allowed"].toBool());
        backend.applyCpu({"bad",1800,0},false); QCOMPARE(done.count(),2); QVERIFY(!done.last()[0].toBool());
    }
    void demoEcCommandsNeedNoOwnershipLifecycle() {
        HardwareBackend backend(true);
        QSignalSpy finished(&backend,&HardwareBackend::ecFinished);
        QVERIFY(backend.ecAvailable()); QVERIFY(!backend.ecActive());
        backend.applyEcTarget("both",40); QVERIFY(finished.count()>=1); QVERIFY(finished.last()[0].toBool());
        backend.applyEcTarget("keylight",50); QVERIFY(finished.count()>=2); QVERIFY(finished.last()[0].toBool());
        QVERIFY(!backend.ecActive());
    }
    void sleepHooksPreserveConfigurationAndAutomation() {
        QTemporaryDir d; const auto path=d.path()+"/config.json"; Window w(path,true);
        w.findChild<QCheckBox *>("autoPresets")->setChecked(true);
        const auto before=Settings::load(path).toJson();
        QVERIFY(QMetaObject::invokeMethod(&w,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,true)));
        QVERIFY(QMetaObject::invokeMethod(&w,"prepareForSleep",Qt::DirectConnection,Q_ARG(bool,false)));
        QCOMPARE(Settings::load(path).toJson(),before);
        QVERIFY(w.findChild<QCheckBox *>("autoPresets")->isChecked());
    }
};
QTEST_MAIN(GuiTests)
#include "test_gui.moc"