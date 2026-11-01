#include "window.h"
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace n14 {
static QLabel *label(const QString &s) {
    auto *l=new QLabel(s); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l;
}
static QPushButton *action(const QString &s,const QString &name,QLayout *layout) {
    auto *b=new QPushButton(s); b->setObjectName(name); layout->addWidget(b); return b;
}
void Window::buildPresets() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page);
    layout->addWidget(label("Пресет N14 объединяет системный профиль, экран и CPU. «Не изменять» сохраняет текущее значение, а не возвращает прежнее. Пресеты не управляют вентиляторами или подсветкой в этой версии."));
    auto *editor=new QGroupBox("Редактор пользовательских пресетов"); auto *form=new QFormLayout(editor);
    presetChoice_=new QComboBox; presetChoice_->setObjectName("presetChoice"); form->addRow("Пресет",presetChoice_);
    presetName_=new QLineEdit; presetName_->setMaxLength(80); presetName_->setObjectName("presetName"); form->addRow("Название",presetName_);
    presetPower_=new QComboBox; presetPower_->setObjectName("presetPower"); presetPower_->addItem("Не изменять","");
    for (const auto &p : {QString("power-saver"),QString("balanced"),QString("performance")}) presetPower_->addItem(profileLabel(p),p);
    form->addRow("Системный профиль",presetPower_);
    presetHz_=new QComboBox; presetHz_->setObjectName("presetHz");
    presetHz_->addItem("Не изменять",0); presetHz_->addItem("60 Гц",60); presetHz_->addItem("165 Гц",165); form->addRow("Экран",presetHz_);
    presetCpuMode_=new QComboBox; presetCpuMode_->setObjectName("presetCpuMode");
    presetCpuMode_->addItem("Не изменять","keep"); presetCpuMode_->addItem("Ограничить сверху","limit");
    presetCpuMode_->addItem("Убрать дополнительный предел (максимум CPU)","hardware"); form->addRow("Частота CPU",presetCpuMode_);
    presetMaxMHz_=new QSpinBox; presetMaxMHz_->setObjectName("presetMaxMHz"); presetMaxMHz_->setRange(400,10000);
    presetMaxMHz_->setSingleStep(100); presetMaxMHz_->setSuffix(" МГц"); form->addRow("Верхний предел",presetMaxMHz_);
    presetTurbo_=new QComboBox; presetTurbo_->setObjectName("presetTurbo");
    presetTurbo_->addItem("Не изменять",-1); presetTurbo_->addItem("Запретить",0); presetTurbo_->addItem("Разрешить",1); form->addRow("Turbo Boost",presetTurbo_);
    auto *buttons=new QHBoxLayout;
    connect(action("Сохранить","savePreset",buttons),&QPushButton::clicked,this,&Window::savePreset);
    connect(action("Создать копию","duplicatePreset",buttons),&QPushButton::clicked,this,&Window::duplicatePreset);
    connect(action("Удалить","deletePreset",buttons),&QPushButton::clicked,this,&Window::deletePreset); form->addRow(buttons);
    auto *apply=new QHBoxLayout;
    connect(action("Применить сохранённый пресет","applyPreset",apply),&QPushButton::clicked,this,&Window::manualPreset); form->addRow(apply);
    layout->addWidget(editor);
    layout->addWidget(label("Редактирование не меняет железо. Сначала сохраните пресет; выбор другого пресета или изменение правил сбрасывает несохранённые правки. Предел — потолок частоты, не фиксированная частота и не разгон. На P/E-ядрах он ограничивается аппаратным максимумом каждой policy. CPU требует отдельно установленного аппаратного сервиса."));
    auto *rules=new QGroupBox("Автоматический выбор пресета"); auto *rf=new QFormLayout(rules);
    autoPresets_=new QCheckBox("Включить автоматику составных пресетов"); autoPresets_->setObjectName("autoPresets"); rf->addRow(autoPresets_);
    acPreset_=new QComboBox; batteryPreset_=new QComboBox; lowPreset_=new QComboBox;
    acPreset_->setObjectName("acPreset"); batteryPreset_->setObjectName("batteryPreset"); lowPreset_->setObjectName("lowPreset");
    rf->addRow("От сети",acPreset_); rf->addRow("От аккумулятора",batteryPreset_); rf->addRow("Низкий заряд",lowPreset_);
    presetLowPercent_=new QSpinBox; presetLowPercent_->setRange(5,50); presetLowPercent_->setSuffix(" %"); rf->addRow("Порог низкого заряда",presetLowPercent_);
    layout->addWidget(rules);
    presetStatus_=label("—"); presetStatus_->setObjectName("presetStatus"); layout->addWidget(presetStatus_);
    layout->addWidget(label("Автоматика пресетов и простые правила вкладки питания взаимоисключающие. Ручное применение пресета отключает обе автоматики. Два наблюдения питания подтверждают переход; неизвестное питание не запускает правил. Фоновая автоматика не показывает запросов пароля: для CPU нужны заранее выданные права. После ошибки нет бесконечных повторов."));
    layout->addWidget(label("Порядок применения: системный профиль → CPU → экран. CPU имеет собственный проверяемый откат, но операция между разными системными сервисами не атомарна: при ошибке уже выполненные предыдущие этапы могут сохраниться. Это явно отмечается в журнале."));
    layout->addStretch(); auto *scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setWidget(page); tabs_->addTab(scroll,"Пользовательские профили");
    syncPresetUi();
    connect(presetChoice_,qOverload<int>(&QComboBox::currentIndexChanged),this,&Window::loadPresetEditor);
    connect(presetCpuMode_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ presetMaxMHz_->setEnabled(presetCpuMode_->currentData()=="limit"); });
    connect(autoPresets_,&QCheckBox::toggled,this,&Window::savePresetRules);
    for (auto *c : {acPreset_,batteryPreset_,lowPreset_}) connect(c,qOverload<int>(&QComboBox::currentIndexChanged),this,&Window::savePresetRules);
    connect(presetLowPercent_,qOverload<int>(&QSpinBox::valueChanged),this,&Window::savePresetRules);
}
void Window::syncPresetUi() {
    const QString selected=presetChoice_->currentData().toString();
    const QSignalBlocker a(presetChoice_),b(autoPresets_),c(acPreset_),d(batteryPreset_),e(lowPreset_),f(presetLowPercent_);
    for (auto *box : {presetChoice_,acPreset_,batteryPreset_,lowPreset_}) {
        box->clear(); for (const auto &p : settings_.presets) box->addItem(p.name,p.id);
    }
    presetChoice_->setCurrentIndex(std::max(0,presetChoice_->findData(selected)));
    acPreset_->setCurrentIndex(acPreset_->findData(settings_.acPreset)); batteryPreset_->setCurrentIndex(batteryPreset_->findData(settings_.batteryPreset));
    lowPreset_->setCurrentIndex(lowPreset_->findData(settings_.lowPreset)); autoPresets_->setChecked(settings_.autoPresets);
    presetLowPercent_->setValue(settings_.lowPercent); loadPresetEditor();
}
void Window::loadPresetEditor() {
    if (presetChoice_->currentIndex()<0) return;
    const auto &p=settings_.preset(presetChoice_->currentData().toString());
    presetName_->setText(p.name); presetPower_->setCurrentIndex(presetPower_->findData(p.powerProfile)); presetHz_->setCurrentIndex(presetHz_->findData(p.hz));
    presetCpuMode_->setCurrentIndex(presetCpuMode_->findData(p.cpu.mode)); presetMaxMHz_->setValue(p.cpu.maxMHz);
    presetMaxMHz_->setEnabled(p.cpu.mode=="limit"); presetTurbo_->setCurrentIndex(presetTurbo_->findData(p.cpu.turbo));
}
bool Window::commitSettings(const Settings &next,const QString &message) {
    try {
        next.save(settingsPath_); settings_=next; rules_.reset();
        attemptedProfile_.clear(); attemptedHz_.clear(); attemptedPreset_.clear(); syncRulesUi(); syncPresetUi();
        log(message); return true;
    } catch (const std::exception &e) { syncRulesUi(); syncPresetUi(); log(QString::fromUtf8(e.what()),true); return false; }
}
void Window::savePreset() {
    Settings next=settings_;
    const QString id=presetChoice_->currentData().toString();
    for (auto &p : next.presets) if (p.id==id) {
        p.name=presetName_->text().trimmed(); p.powerProfile=presetPower_->currentData().toString(); p.hz=presetHz_->currentData().toInt();
        p.cpu={presetCpuMode_->currentData().toString(),presetMaxMHz_->value(),presetTurbo_->currentData().toInt()};
    }
    commitSettings(next,"Пресет сохранён; аппаратные настройки не изменены");
}
void Window::savePresetRules() {
    Settings next=settings_; next.autoPresets=autoPresets_->isChecked();
    if (next.autoPresets) next.autoProfile=next.autoRefresh=false;
    next.acPreset=acPreset_->currentData().toString(); next.batteryPreset=batteryPreset_->currentData().toString(); next.lowPreset=lowPreset_->currentData().toString();
    next.lowPercent=presetLowPercent_->value(); commitSettings(next,"Правила пресетов сохранены");
}
void Window::duplicatePreset() {
    if (settings_.presets.size()>=16) { log("Допускается до 16 пресетов",true); return; }
    Settings next=settings_; auto p=settings_.preset(presetChoice_->currentData().toString());
    p.id=QUuid::createUuid().toString(QUuid::WithoutBraces); p.name=p.name.left(72)+" · копия"; next.presets.append(p);
    if (commitSettings(next,"Создана копия пресета")) presetChoice_->setCurrentIndex(presetChoice_->findData(p.id));
}
void Window::deletePreset() {
    const QString id=presetChoice_->currentData().toString();
    if (settings_.presets.size()==1 || id==settings_.acPreset || id==settings_.batteryPreset || id==settings_.lowPreset) {
        log("Нельзя удалить последний пресет или пресет, назначенный правилу; сначала измените назначения",true); return;
    }
    Settings next=settings_;
    next.presets.erase(std::remove_if(next.presets.begin(),next.presets.end(),[&](const Preset &p){ return p.id==id; }),next.presets.end());
    commitSettings(next,"Пресет удалён");
}
void Window::manualPreset() {
    if (pendingPreset_) { log("Пресет уже применяется",true); return; }
    const auto p=settings_.preset(presetChoice_->currentData().toString());
    Settings next=settings_; next.autoPresets=next.autoProfile=next.autoRefresh=false;
    if (commitSettings(next,"Ручной пресет: автоматизация отключена")) startPreset(p,true);
}
void Window::startPreset(const Preset &input,bool interactive) {
    if (sleeping_) { log("Применение пресета отклонено: подготовка ко сну",true); return; }
    if (pendingPreset_) { log("Пресет уже применяется",true); return; }
    const auto p=Preset::fromJson(input.toJson());
    if (p.cpu.changesHardware() && (!hardware_.available() || !hardware_.state()["supported"].toBool())) {
        log("Пресет не начат: CPU-сервис не установлен/недоступен. Другие настройки пресета не изменены",true); return;
    }
    if (p.cpu.turbo!=-1 && !hardware_.state()["turbo_supported"].toBool()) {
        log("Пресет не начат: интерфейс Turbo Boost недоступен",true); return;
    }
    if (!p.powerProfile.isEmpty() && !backend_.powerAvailable() && !demo_) { log("Пресет не начат: сервис питания недоступен",true); return; }
    if (p.hz && (!backend_.panel() || !backend_.panel()->modeForHz(p.hz) || backend_.screenBusy())) {
        log("Пресет не начат: режим экрана недоступен или KScreen занят",true); return;
    }
    pendingPreset_=p; presetInteractive_=interactive;
    presetStatus_->setText("Применяется: "+p.name);
    if (!p.powerProfile.isEmpty()) { presetStage_=PresetStage::Power; backend_.setProfile(p.powerProfile); }
    else applyPresetCpu();
}
void Window::applyPresetCpu() {
    if (!pendingPreset_) return;
    if (sleeping_) { finishPreset(false); return; }
    presetStage_=PresetStage::Cpu;
    if (pendingPreset_->cpu.changesHardware()) hardware_.applyCpu(pendingPreset_->cpu,presetInteractive_);
    else applyPresetScreen();
}
void Window::applyPresetScreen() {
    if (!pendingPreset_) return;
    if (sleeping_) { finishPreset(false); return; }
    presetStage_=PresetStage::Screen;
    if (pendingPreset_->hz) backend_.setHz(pendingPreset_->hz); else finishPreset(true);
}
void Window::finishPreset(bool success) {
    if (!pendingPreset_) return;
    const QString name=pendingPreset_->name; pendingPreset_.reset(); presetStage_=PresetStage::Idle;
    const QString message=success ? "Пресет подтверждён: "+name : "Пресет не завершён: "+name+". Предыдущие этапы могли примениться; проверьте журнал и фактические настройки";
    presetStatus_->setText(message); log(message,!success);
}
void Window::prepareForSleep(bool sleeping) {
    ++telemetryRevision_;
    sleeping_=sleeping; rules_.reset(); attemptedProfile_.clear(); attemptedHz_.clear(); attemptedPreset_.clear();
    if (sleeping) {
        sampleTimer_.stop(); previewTimer_.stop();
        if (pendingPreset_) finishPreset(false);
        log("Сон: новые правила приостановлены; незавершённые внешние операции могут иметь частичный результат");
    } else {
        // AC may have changed while the machine was asleep.  Do not use the
        // first post-resume sysfs read as the decision: it can still contain
        // the pre-suspend value.  Refresh the power service immediately, then
        // confirm the new source twice after a short settle interval.
        rules_.reset();
        backend_.refreshPower(); backend_.refreshPanel(); hardware_.refresh(); sampleTimer_.start();
        QTimer::singleShot(1200,this,[this]{
            if (sleeping_) return;
            rules_.reset(); sample();
            QTimer::singleShot(100,this,[this]{ if (!sleeping_) sample(); });
        });
        if (isVisible() && tabs_->currentIndex()==2) previewTimer_.start();
        log("Возобновление: питание перечитывается после стабилизации; профиль приложения будет применён заново");
    }
}
void Window::buildHardware() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page);
    hardwareInfo_=label("—"); hardwareInfo_->setObjectName("hardwareInfo"); hardwareInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(hardwareInfo_);
    connect(action("Перепроверить доступность и состояние","refreshHardware",layout),&QPushButton::clicked,&hardware_,&HardwareBackend::refresh);
    auto *ecGroup=new QGroupBox("N14 EC · lifecycle управляется сервисом"); auto *ecLayout=new QVBoxLayout(ecGroup);
    auto *ecStatus=label("EC broker: состояние ещё не подтверждено"); ecStatus->setObjectName("ecStatus"); ecLayout->addWidget(ecStatus);
    auto *keylight=new QHBoxLayout;
    keylight->addWidget(label("Подсветка клавиатуры"));
    keylightSlider_=new QSlider(Qt::Horizontal); keylightSlider_->setObjectName("keylightSlider");
    keylightSlider_->setRange(0,100); keylightSlider_->setValue(50); keylightSlider_->setSingleStep(1);
    keylightSlider_->setPageStep(10); keylightSlider_->setTracking(false); keylight->addWidget(keylightSlider_,1);
    keylightValue_=label("50%"); keylightValue_->setMinimumWidth(45); keylight->addWidget(keylightValue_);
    // With tracking off Qt emits sliderReleased BEFORE committing value().
    // valueChanged covers the committed mouse value, keyboard and wheel alike.
    connect(keylightSlider_,&QSlider::valueChanged,this,[this](int value){
        keylightValue_->setText(QString::number(value)+"%"); fan_.setKeylight(value);
    });
    ecLayout->addLayout(keylight); layout->addWidget(ecGroup);
    layout->addWidget(label("Аппаратный сервис — отдельный root-процесс; GUI не запускается от root. Fan curve/manual и keylight проходят через одну сериализованную EC transaction queue с identity, freshness и readback проверками."));
    layout->addWidget(label("Установка сервиса — отдельное явное действие. Она не останавливает CoolerControl/NBFC и не выполняет hardware write; новый broker нельзя запускать параллельно с действующим EC owner."));
    layout->addWidget(label("RCU Lazy: состояние читается из ядра. Если параметр только для чтения, переключатель во время работы недоступен; изменения загрузчика здесь не выполняются.\n\nMSHybrid / iGPU / dGPU: программный интерфейс переключения MUX пока не подтверждён. Runtime-PM и выбор GPU для приложения не выдаются за переключение MUX."));
    layout->addStretch(); auto *scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setWidget(page); tabs_->addTab(scroll,"Оборудование"); refreshHardwareLabel();
}
void Window::refreshFanLabels() {
    const auto state=fan_.state();
    const bool available=!state.isEmpty();
    const bool manual=state["manual"].toBool();
    const bool failed=state["failed"].toBool();
    if (keylightSlider_) keylightSlider_->setEnabled(demo_ || (available && state["ready"].toBool() && !failed && !state["sleeping"].toBool()));
    if (manualFans_) {
        const QSignalBlocker blocker(manualFans_); manualFans_->setChecked(manual);
        manualFans_->setEnabled(demo_ || (available && state["ready"].toBool() && !state["sleeping"].toBool()));
    }
    if (autoEc_) {
        autoEc_->setChecked(!manual && !failed && available);
        autoEc_->setText(!available ? "Контроллер не запущен: кривая не исполняется" : failed ? "Контур остановлен: "+state["reason"].toString()
            : state["waiting_for_sample"].toBool() ? "Ожидание согласованного GPU snapshot: EC-записей нет"
            : manual ? "Кривая отключена ручным режимом" : state["sleeping"].toBool() ? "Кривая приостановлена на время сна" : "Кривая: фоновое управление, цикл 1 с");
    }
    if (fanSlider_) fanSlider_->setEnabled(manual && available && !failed);
    if (auto *ec=findChild<QLabel *>("ecStatus")) ec->setText(available ? "EC-клиент: единый фоновый fan controller · "+state["mode"].toString() : "Фоновый fan controller недоступен; GUI не перехватывает EC");
    const auto status=state["status"].toObject();
    if (status.isEmpty()) {
        if (fanStatus_) fanStatus_->setText("CPU RPM: нет подтверждённого readback; GPU RPM: неизвестны");
        return;
    }
    try {
        const int cpu=reference::fanRawToDuty(status["cpu_target_raw"].toInt(-1));
        const int gpu=reference::fanRawToDuty(status["gpu_target_raw"].toInt(-1));
        const int light=reference::keylightRawToPercent(status["keylight_raw"].toInt(-1));
        const QString prefix=failed ? "Последний readback (контур остановлен): " : "";
        if (fanStatus_) fanStatus_->setText(prefix+QString("CPU: %1 RPM nominal; GPU RPM: неизвестны · targets %2/%3%% · EC mode %4")
            .arg(status["cpu_tach_raw"].toInt()*100).arg(cpu).arg(gpu).arg(status["fan_mode"].toInt()));
        if (fanSlider_ && !fanSlider_->isSliderDown()) {
            const QSignalBlocker blocker(fanSlider_); fanSlider_->setValue(std::max(cpu,gpu));
            fanValue_->setText(QString::number(fanSlider_->value())+"%");
        }
        if (keylightSlider_ && !keylightSlider_->isSliderDown() && !fan_.keylightPending()) {
            const QSignalBlocker blocker(keylightSlider_); keylightSlider_->setValue(light);
            keylightValue_->setText(QString::number(light)+"%");
        }
    } catch (const std::exception &) {
        if (fanStatus_) fanStatus_->setText("Некорректный fan readback; RPM не подтверждены");
    }
}
void Window::refreshHardwareLabel() {
    if (!hardwareInfo_) return;
    if (hardware_.available() && !hardwareWasAvailable_) { rules_.reset(); attemptedPreset_.clear(); }
    hardwareWasAvailable_=hardware_.available();
    const auto cpu=hardware_.available() ? hardware_.state() : CpuController().inspect();
    QStringList lines{demo_ ? "CPU-сервис: ДЕМО" : hardware_.available() ? "CPU-сервис: подключён" : "CPU-сервис: не установлен или недоступен; ниже — только чтение sysfs"};
    lines.append(cpu["supported"].toBool() ? "Ограничение частоты: интерфейс обнаружен" : "Ограничение частоты: "+cpu["reason"].toString());
    lines.append(cpu["turbo_supported"].toBool() ? QString("Turbo Boost разрешён: ")+(cpu["turbo_allowed"].toBool() ? "да" : "нет") : "Turbo Boost: интерфейс недоступен");
    for (const auto &v : cpu["policies"].toArray()) {
        const auto p=v.toObject(); lines.append(QString("%1 · %2 · предел %3 МГц · аппаратный максимум %4 МГц")
            .arg(p["name"].toString(),p["driver"].toString()).arg(p["max_khz"].toInt()/1000).arg(p["hardware_max_khz"].toInt()/1000));
    }
    if (!demo_) {
        QFile rcu("/sys/module/rcutree/parameters/enable_rcu_lazy");
        if (rcu.open(QIODevice::ReadOnly)) lines.append("RCU Lazy: "+QString::fromLatin1(rcu.read(32).trimmed())+
            (QFileInfo(rcu).permissions()&QFileDevice::WriteOwner ? " (атрибут допускает запись; управление ещё не подключено)" : " (параметр ядра только для чтения)"));
        else lines.append("RCU Lazy: параметр недоступен");
    } else lines.append("RCU Lazy: демонстрационный режим; ядро не изменяется");
    lines.append(QString("EC broker: %1").arg(hardware_.ecActive() ? "состояние подтверждено" :
                                                  hardware_.ecAvailable() ? "доступен, lifecycle управляется сервисом" : "недоступен или старый EC protocol"));
    lines.append("Показана последняя проверка, не непрерывный контроль фактической частоты."); hardwareInfo_->setText(lines.join('\n'));
}
} // namespace n14