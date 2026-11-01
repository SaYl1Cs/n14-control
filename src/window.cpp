#include "window.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDBusConnection>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace n14 {
static QLabel *text(const QString &s) { auto *l=new QLabel(s); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l; }
static QPushButton *button(const QString &s,QLayout *layout) { auto *b=new QPushButton(s); layout->addWidget(b); return b; }
static QComboBox *profiles() {
    auto *c=new QComboBox;
    for (const auto &p : {QString("power-saver"),QString("balanced"),QString("performance")}) c->addItem(profileLabel(p),p);
    return c;
}
static QComboBox *rates() { auto *c=new QComboBox; c->addItem("60 Гц",60); c->addItem("165 Гц",165); return c; }
static QString watts(std::optional<double> p) { return p ? QString::number(*p,'f',2)+" Вт" : "Нет данных"; }

class HistoryPlot : public QWidget {
public:
    const QVector<Snapshot> &samples;
    explicit HistoryPlot(const QVector<Snapshot> &history,QWidget *parent=nullptr)
        : QWidget(parent),samples(history) { setMinimumHeight(190); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF r=rect().adjusted(54,20,-18,-30);
        p.setPen(QColor("#344354"));
        double high=10;
        for (const auto &s : samples) if (s.batteryStatus=="Discharging" && s.powerW) high=std::max(high,*s.powerW*1.15);
        for (int i=0;i<=4;++i) {
            const double y=r.bottom()-r.height()*i/4;
            p.drawLine(QPointF(r.left(),y),QPointF(r.right(),y));
            p.setPen(QColor("#acb8c7")); p.drawText(QRectF(0,y-10,46,20),Qt::AlignRight|Qt::AlignVCenter,QString::number(high*i/4,'f',0)+" W");
            p.setPen(QColor("#344354"));
        }
        if (samples.isEmpty()) return;
        const qint64 end=samples.last().timestampMs, start=std::min(end-60000,samples.first().timestampMs);
        QPainterPath path; bool connected=false; qint64 previous=0; int valid=0;
        for (const auto &s : samples) {
            if (!s.powerW || s.batteryStatus!="Discharging") { connected=false; continue; }
            const QPointF pt(r.left()+r.width()*double(s.timestampMs-start)/double(end-start),
                             r.bottom()-r.height()*(*s.powerW)/high);
            if (connected && s.timestampMs-previous<20000 && s.timestampMs>previous) path.lineTo(pt);
            else { path.moveTo(pt); p.setPen(QColor("#72d4bf")); p.drawEllipse(pt,2,2); }
            connected=true; previous=s.timestampMs; ++valid;
        }
        p.setPen(QPen(QColor("#72d4bf"),2)); p.drawPath(path);
        p.setPen(QColor("#acb8c7"));
        p.drawText(QRectF(r.left(),r.bottom()+6,r.width(),20),Qt::AlignLeft,QDateTime::fromMSecsSinceEpoch(start).toString("HH:mm:ss"));
        p.drawText(QRectF(r.left(),r.bottom()+6,r.width(),20),Qt::AlignRight,QDateTime::fromMSecsSinceEpoch(end).toString("HH:mm:ss"));
        if (!valid) p.drawText(r,Qt::AlignCenter,"График появится при разряде аккумулятора");
    }
};
class CurvePlot : public QWidget {
public:
    Curve cpu,gpu;
    explicit CurvePlot(QWidget *parent=nullptr) : QWidget(parent) { setMinimumHeight(175); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF r=rect().adjusted(42,12,-18,-25);
        p.setPen(QColor("#344354"));
        for (int i=0;i<=4;++i) p.drawLine(QPointF(r.left(),r.top()+r.height()*i/4),QPointF(r.right(),r.top()+r.height()*i/4));
        p.setPen(QColor("#acb8c7")); p.drawText(2,20,"100%"); p.drawText(8,height()-25,"0%");
        p.drawText(42,height()-4,"0 °C"); p.drawText(width()-65,height()-4,"120 °C");
        for (int index=0;index<2;++index) {
            const auto &curve=index==0 ? cpu : gpu;
            if (!validateCurve(curve).isEmpty()) continue;
            const auto pos=[&r](double t,double d){ return QPointF(r.left()+r.width()*t/120,r.bottom()-r.height()*d/100); };
            QPainterPath path; path.moveTo(pos(0,curve.first().duty));
            for (const auto &point : curve) path.lineTo(pos(point.temperature,point.duty));
            path.lineTo(pos(120,curve.last().duty));
            p.setPen(QPen(QColor(index==0 ? "#72d4bf" : "#eeb873"),2)); p.drawPath(path);
            for (const auto &point : curve) p.drawEllipse(pos(point.temperature,point.duty),3,3);
        }
    }
};

Window::Window(QString settingsPath,bool demo,QWidget *parent)
    : QMainWindow(parent),settingsPath_(std::move(settingsPath)),demo_(demo),backend_(demo,this),hardware_(demo,this),fan_(demo,this) {
    QString loadError;
    try { settings_=Settings::load(settingsPath_); }
    catch (const std::exception &e) { loadError=QString::fromUtf8(e.what()); }
    setWindowTitle("N14 Control · 0.2"); setWindowIcon(QIcon(":/n14-control.svg")); resize(1100,870);
    auto *root=new QWidget; auto *layout=new QVBoxLayout(root);
    auto *heading=text(demo_ ? "N14 Control — ДЕМО, без аппаратных изменений" : "N14 Control");
    heading->setStyleSheet("font-size: 25px; font-weight: 600; color: #72d4bf; padding: 8px;"); layout->addWidget(heading);
    tabs_=new QTabWidget; layout->addWidget(tabs_); setCentralWidget(root);
    buildOverview(); buildPower(); buildCooling(); buildPresets(); buildHardware(); buildSystem(); buildTray();
    connect(&backend_,&DesktopBackend::notice,this,&Window::log);
    connect(&backend_,&DesktopBackend::profileChanged,this,[this]{
        refreshBackendLabels();
    });
    connect(&backend_,&DesktopBackend::externalProfileChanged,this,[this]{
        if (!sleeping_ && !backend_.profileBusy() && !backend_.activeProfile().isEmpty()) {
            log("Системное изменение профиля проигнорировано; восстанавливается профиль приложения");
            backend_.setProfile(backend_.activeProfile());
        }
    });
    connect(&backend_,&DesktopBackend::panelChanged,this,&Window::refreshBackendLabels);
    connect(&hardware_,&HardwareBackend::notice,this,&Window::log);
    connect(&fan_,&FanClient::notice,this,&Window::log);
    connect(&fan_,&FanClient::changed,this,&Window::refreshFanLabels);
    fan_.refresh();
    connect(&hardware_,&HardwareBackend::changed,this,[this]{
        refreshHardwareLabel();
    });
    connect(&hardware_,&HardwareBackend::ecChanged,this,&Window::refreshHardwareLabel);
    connect(&backend_,&DesktopBackend::profileFinished,this,[this](bool ok){
        if (presetStage_==PresetStage::Power) { if (ok) applyPresetCpu(); else finishPreset(false); }
    });
    connect(&hardware_,&HardwareBackend::cpuFinished,this,[this](bool ok){
        if (presetStage_==PresetStage::Cpu) { if (ok) applyPresetScreen(); else finishPreset(false); }
    });
    connect(&backend_,&DesktopBackend::frequencyFinished,this,[this](int,bool ok){
        if (presetStage_==PresetStage::Screen) finishPreset(ok);
    });
    if (!demo_) QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(prepareForSleep(bool)));
    // Passive sysfs/ACPI telemetry does not need a 1 Hz UI cadence. Five
    // seconds matches the documented history resolution and avoids needless
    // wakeups while the window is hidden in the tray; EC authority and
    // sleep/resume paths remain event-driven.
    sampleTimer_.setInterval(5000); sampleTimer_.setTimerType(Qt::CoarseTimer);
    connect(&sampleTimer_,&QTimer::timeout,this,&Window::sample); sampleTimer_.start();
    previewTimer_.setInterval(1000);
    connect(&previewTimer_,&QTimer::timeout,this,&Window::preview);
    connect(tabs_,&QTabWidget::currentChanged,this,[this](int index){
        if (index==2) { filters_[0].reset(); filters_[1].reset(); preview(); previewTimer_.start(); }
        else previewTimer_.stop();
    });
    sample(); refreshBackendLabels();
    if (!loadError.isEmpty()) log("Настройки не загружены; автоматика выключена: "+loadError,true);
    log("Версия 0.2+: пользовательские пресеты и curve/manual control; EC lifecycle управляется сервисом, identity/provenance/readback gates сохранены.");
}
void Window::buildOverview() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page); auto *metrics=new QHBoxLayout;
    const QStringList names{"Аккумулятор","Мощность аккумулятора","NVIDIA GPU","CPU"};
    QLabel **values[]={&batteryValue_,&powerValue_,&gpuValue_,&cpuValue_};
    for (int i=0;i<4;++i) {
        auto *box=new QGroupBox(names[i]); auto *l=new QVBoxLayout(box);
        *values[i]=text("—"); (*values[i])->setStyleSheet("font-size: 20px; font-weight: 600;");
        l->addWidget(*values[i]); metrics->addWidget(box);
    }
    layout->addLayout(metrics);
    layout->addWidget(text("Потребление всего ноутбука при разряде, а не отдельной видеокарты. При питании от сети ток батареи не показывает мощность ноутбука."));
    historyPlot_=new HistoryPlot(history_); layout->addWidget(historyPlot_);
    average_=text("Средний разряд за последние 60 секунд: —"); layout->addWidget(average_);
    auto *controls=new QHBoxLayout;
    connect(button("Экспорт истории CSV…",controls),&QPushButton::clicked,this,&Window::exportCsv);
    controls->addStretch(); layout->addLayout(controls);
    layout->addWidget(text("Пассивный опрос раз в 5 секунд. История: до 720 отсчётов в памяти (~1 час). NVML и nvidia-smi не используются. Время GPU suspended — накопленное за загрузку, не обязательно непрерывное."));
    layout->addStretch(); tabs_->addTab(page,"Обзор");
}
void Window::buildPower() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page);
    auto *manual=new QGroupBox("Ручное управление"); auto *ml=new QVBoxLayout(manual);
    profileValue_=text("Профиль: —"); screenValue_=text("Экран: —"); ml->addWidget(profileValue_); ml->addWidget(screenValue_);
    auto *pl=new QHBoxLayout;
    for (const auto &p : {QString("power-saver"),QString("balanced"),QString("performance")}) {
        auto *b=button(profileLabel(p),pl); b->setObjectName("manual-"+p);
        connect(b,&QPushButton::clicked,this,[this,p]{ manualProfile(p); });
    }
    ml->addLayout(pl); auto *rl=new QHBoxLayout;
    connect(button("60 Гц",rl),&QPushButton::clicked,this,[this]{ manualHz(60); });
    connect(button("165 Гц",rl),&QPushButton::clicked,this,[this]{ manualHz(165); });
    connect(button("Обновить данные экрана",rl),&QPushButton::clicked,&backend_,&DesktopBackend::refreshPanel);
    ml->addLayout(rl); ml->addWidget(text("Ручное переключение отключает соответствующую автоматику. Частота меняется без изменения текущего разрешения, масштаба и внешних экранов.")); layout->addWidget(manual);
    auto *rules=new QGroupBox("Автоматические правила"); auto *form=new QFormLayout(rules);
    autoProfile_=new QCheckBox("Переключать профиль питания"); autoHz_=new QCheckBox("Переключать частоту экрана");
    autoProfile_->setObjectName("autoProfile"); autoHz_->setObjectName("autoHz");
    form->addRow(autoProfile_); form->addRow(autoHz_);
    acProfile_=profiles(); batteryProfile_=profiles(); lowProfile_=profiles(); acHz_=rates(); batteryHz_=rates();
    lowPercent_=new QSpinBox; lowPercent_->setRange(5,50); lowPercent_->setSuffix(" %");
    form->addRow("От сети",acProfile_); form->addRow("От аккумулятора",batteryProfile_);
    form->addRow("При низком заряде",lowProfile_); form->addRow("Порог низкого заряда",lowPercent_);
    form->addRow("Частота от сети",acHz_); form->addRow("Частота от аккумулятора",batteryHz_);
    layout->addWidget(rules); syncRulesUi();
    connect(autoProfile_,&QCheckBox::toggled,this,&Window::saveRules);
    connect(autoHz_,&QCheckBox::toggled,this,&Window::saveRules);
    for (auto *c : {acProfile_,batteryProfile_,lowProfile_,acHz_,batteryHz_})
        connect(c,qOverload<int>(&QComboBox::currentIndexChanged),this,&Window::saveRules);
    connect(lowPercent_,qOverload<int>(&QSpinBox::valueChanged),this,&Window::saveRules);
    layout->addWidget(text("Изменения сохраняются автоматически. Нужны два последовательных наблюдения источника питания (обычно 5–10 секунд). При неизвестном состоянии правила не применяются. После ошибки нет бесконечных повторов: измените правило или выключите/включите автоматику для повторной попытки."));
    layout->addStretch(); tabs_->addTab(page,"Питание и экран");
}
QTableWidget *Window::curveTable(const Curve &curve,QWidget *parent) {
    auto *t=new QTableWidget(int(curve.size()),2,parent); t->setHorizontalHeaderLabels({"Температура, °C","Скорость, %"});
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); t->verticalHeader()->hide();
    t->setMaximumHeight(200); t->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i=0;i<curve.size();++i) {
        t->setItem(i,0,new QTableWidgetItem(QString::number(curve[i].temperature)));
        t->setItem(i,1,new QTableWidgetItem(QString::number(curve[i].duty)));
    }
    return t;
}
Curve Window::curveFromTable(QTableWidget *table) const {
    Curve curve;
    for (int row=0;row<table->rowCount();++row) {
        bool a=false,b=false;
        const double temp=table->item(row,0) ? table->item(row,0)->text().toDouble(&a) : 0;
        const double duty=table->item(row,1) ? table->item(row,1)->text().toDouble(&b) : 0;
        if (!a || !b) throw std::runtime_error("Точки должны содержать числа; десятичный разделитель — точка");
        curve.append({temp,duty});
    }
    const QString error=validateCurve(curve);
    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
    return curve;
}
void Window::buildCooling() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page);
    auto *warning=text("Сохранённые кривые и Standard/Functions исполняет отдельный n14-fan-controller, даже без окна. GUI не запускает второй контур и не пишет EC напрямую.");
    warning->setStyleSheet("color: #eeb873;"); layout->addWidget(warning);
    auto *fanControl=new QGroupBox("Управление вентиляторами");
    auto *fanLayout=new QVBoxLayout(fanControl);
    autoEc_=new QCheckBox("Кривая: автоматическое управление");
    autoEc_->setObjectName("autoEc"); autoEc_->setChecked(true); autoEc_->setEnabled(false);
    fanLayout->addWidget(autoEc_);
    manualFans_=new QCheckBox("Ручной режим вентиляторов");
    manualFans_->setObjectName("manualFans"); manualFans_->setChecked(false); fanLayout->addWidget(manualFans_);
    fanStatus_=text("Обороты CPU: — RPM; GPU: readback недоступен");
    fanStatus_->setObjectName("fanStatus"); fanLayout->addWidget(fanStatus_);
    auto *fanManual=new QHBoxLayout;
    fanManual->addWidget(text("Ручная скорость вентиляторов"));
    fanSlider_=new QSlider(Qt::Horizontal); fanSlider_->setObjectName("fanSlider");
    fanSlider_->setRange(0,100); fanSlider_->setValue(0); fanSlider_->setSingleStep(1); fanSlider_->setPageStep(10);
    fanSlider_->setTracking(false); fanSlider_->setEnabled(false); fanManual->addWidget(fanSlider_,1);
    fanValue_=text("0%"); fanValue_->setMinimumWidth(45); fanManual->addWidget(fanValue_);
    connect(fanSlider_,&QSlider::valueChanged,this,[this](int value){ fanValue_->setText(QString::number(value)+"%"); });
    connect(fanSlider_,&QSlider::sliderReleased,this,[this]{ fan_.setMode(true,fanSlider_->value()); });
    fanLayout->addLayout(fanManual);
    connect(manualFans_,&QCheckBox::toggled,this,[this](bool manual){
        fanSlider_->setEnabled(manual);
        autoEc_->setChecked(!manual);
        autoEc_->setText(manual ? "Кривая: отключена ручным режимом" : "Кривая: автоматическое управление");
        if (manual) {
            log("Ручной режим включён: fan curve отключена; текущая скорость применяется сразу");
            fan_.setMode(true,fanSlider_->value());
        }
        else { log("Запрошен расчёт по кривой со свежей provenance"); fan_.setMode(false); }
    });
    auto *resume=button("Включить кривую / повторить явно после отказа",fanLayout);
    resume->setObjectName("resumeFanCurve");
    connect(resume,&QPushButton::clicked,this,[this]{ fan_.setMode(false); });
    layout->addWidget(fanControl);
    auto *columns=new QHBoxLayout;
    for (int role=0;role<2;++role) {
        auto *group=new QGroupBox(role==0 ? "CPU · бирюзовый" : "GPU · оранжевый"); auto *col=new QVBoxLayout(group);
        auto *table=curveTable(role==0 ? settings_.cpu : settings_.gpu,group);
        table->setObjectName(role==0 ? "cpuCurve" : "gpuCurve");
        if (role==0) cpuTable_=table; else gpuTable_=table;
        col->addWidget(table); auto *rows=new QHBoxLayout;
        connect(button("+ Точка",rows),&QPushButton::clicked,this,[this,table]{
            try {
                const auto curve=curveFromTable(table);
                if (curve.size()>=64) return;
                const int row=std::clamp(table->currentRow()+1,1,table->rowCount()-1);
                const auto a=curve[row-1], b=curve[row];
                const QSignalBlocker blocker(table); table->insertRow(row);
                table->setItem(row,0,new QTableWidgetItem(QString::number((a.temperature+b.temperature)/2,'g',12)));
                table->setItem(row,1,new QTableWidgetItem(QString::number((a.duty+b.duty)/2,'g',12)));
                preview();
            } catch (const std::exception &e) { log(QString::fromUtf8(e.what()),true); }
        });
        connect(button("− Точка",rows),&QPushButton::clicked,this,[this,table]{
            if (table->rowCount()>2 && table->currentRow()>=0) { table->removeRow(table->currentRow()); preview(); }
        }); col->addLayout(rows);
        auto *form=new QFormLayout;
        filterKind_[role]=new QComboBox; filterKind_[role]->addItems({"Identity","Hysteresis","MovingAverage"});
        deviance_[role]=new QDoubleSpinBox; deviance_[role]->setRange(0,30); deviance_[role]->setSuffix(" °C");
        window_[role]=new QSpinBox; window_[role]->setRange(1,60); window_[role]->setSuffix(" отсч.");
        const auto f=role==0 ? settings_.cpuFunction : settings_.gpuFunction;
        filterKind_[role]->setCurrentText(f.kind); deviance_[role]->setValue(f.deviance); window_[role]->setValue(f.window);
        form->addRow("Function",filterKind_[role]); form->addRow("Гистерезис",deviance_[role]); form->addRow("Окно среднего",window_[role]); col->addLayout(form);
        const auto reset=[this,role]{ filters_[role].reset(); preview(); };
        connect(filterKind_[role],qOverload<int>(&QComboBox::currentIndexChanged),this,reset);
        connect(deviance_[role],qOverload<double>(&QDoubleSpinBox::valueChanged),this,reset);
        connect(window_[role],qOverload<int>(&QSpinBox::valueChanged),this,reset);
        connect(table,&QTableWidget::cellChanged,this,[this]{ preview(); }); columns->addWidget(group);
    }
    layout->addLayout(columns);

    auto *nativeWarning=text("Native CoolerControl Standard · эти параметры использует фоновый контур после сохранения. Legacy Function и Mix ниже относятся к предпросмотру; live поддерживает только Max.");
    nativeWarning->setStyleSheet("color: #9fc7ef;"); layout->addWidget(nativeWarning);
    auto *nativeColumns=new QHBoxLayout;
    for (int role=0;role<2;++role) {
        auto *group=new QGroupBox(role==0 ? "CPU · CoolerControl Standard" : "GPU · CoolerControl Standard");
        auto *form=new QFormLayout(group);
        auto function=role==0 ? settings_.coolerCpuFunction : settings_.coolerGpuFunction;
        auto spin=[&](const QString &name, int value, int low, int high, const QString &suffix) {
            auto *control=new QSpinBox; control->setObjectName(name); control->setRange(low,high); control->setValue(value); control->setSuffix(suffix); return control;
        };
        standardStepMinimum_[role]=spin(role==0 ? "coolerCpuStepMinimum" : "coolerGpuStepMinimum",function.stepMinimum,0,100," %");
        standardStepMaximum_[role]=spin(role==0 ? "coolerCpuStepMaximum" : "coolerGpuStepMaximum",function.stepMaximum,0,100," %");
        standardStepMinimumDecreasing_[role]=spin(role==0 ? "coolerCpuStepMinimumDecreasing" : "coolerGpuStepMinimumDecreasing",function.stepMinimumDecreasing,0,100," %");
        standardStepMaximumDecreasing_[role]=spin(role==0 ? "coolerCpuStepMaximumDecreasing" : "coolerGpuStepMaximumDecreasing",function.stepMaximumDecreasing,0,100," %");
        standardResponseDelay_[role]=spin(role==0 ? "coolerCpuResponseDelay" : "coolerGpuResponseDelay",function.responseDelay,0,255," с");
        standardDeviance_[role]=new QDoubleSpinBox; standardDeviance_[role]->setObjectName(role==0 ? "coolerCpuDeviance" : "coolerGpuDeviance"); standardDeviance_[role]->setRange(0,120); standardDeviance_[role]->setDecimals(2); standardDeviance_[role]->setValue(function.deviance); standardDeviance_[role]->setSuffix(" °C");
        standardOnlyDownward_[role]=new QCheckBox("Только вниз"); standardOnlyDownward_[role]->setObjectName(role==0 ? "coolerCpuOnlyDownward" : "coolerGpuOnlyDownward"); standardOnlyDownward_[role]->setChecked(function.onlyDownward);
        standardThresholdHopping_[role]=new QCheckBox("Threshold hopping"); standardThresholdHopping_[role]->setObjectName(role==0 ? "coolerCpuThresholdHopping" : "coolerGpuThresholdHopping"); standardThresholdHopping_[role]->setChecked(function.thresholdHopping);
        standardExtremeBypass_[role]=new QCheckBox("Bypass 0/100"); standardExtremeBypass_[role]->setObjectName(role==0 ? "coolerCpuExtremeBypass" : "coolerGpuExtremeBypass"); standardExtremeBypass_[role]->setChecked(function.bypassMinimumAtExtremes);
        form->addRow("Минимальный шаг",standardStepMinimum_[role]); form->addRow("Максимальный шаг",standardStepMaximum_[role]);
        form->addRow("Мин. шаг вниз",standardStepMinimumDecreasing_[role]); form->addRow("Макс. шаг вниз",standardStepMaximumDecreasing_[role]);
        form->addRow("Response delay",standardResponseDelay_[role]); form->addRow("Deviance",standardDeviance_[role]);
        form->addRow(standardOnlyDownward_[role]); form->addRow(standardThresholdHopping_[role]); form->addRow(standardExtremeBypass_[role]);
        nativeColumns->addWidget(group);
    }
    layout->addLayout(nativeColumns);
    curvePlot_=new CurvePlot; layout->addWidget(curvePlot_);
    auto *simulation=new QHBoxLayout;
    cpuInput_=new QSlider(Qt::Horizontal); cpuInput_->setRange(0,120); cpuInput_->setValue(65);
    gpuInput_=new QSlider(Qt::Horizontal); gpuInput_->setRange(0,120); gpuInput_->setValue(55);
    gpuAsleep_=new QCheckBox("GPU в D3cold"); gpuAsleep_->setChecked(true);
    mix_=new QComboBox; mix_->addItems({"Max","Min","Mean"}); mix_->setCurrentText(settings_.mix);
    simulation->addWidget(text("CPU")); simulation->addWidget(cpuInput_); simulation->addWidget(text("GPU")); simulation->addWidget(gpuInput_);
    simulation->addWidget(gpuAsleep_); simulation->addWidget(text("Mix")); simulation->addWidget(mix_); layout->addLayout(simulation);
    previewValue_=text("—"); layout->addWidget(previewValue_);
    for (auto *slider : {cpuInput_,gpuInput_}) connect(slider,&QSlider::valueChanged,this,&Window::preview);
    connect(gpuAsleep_,&QCheckBox::toggled,this,[this]{ filters_[1].reset(); preview(); });
    connect(mix_,qOverload<int>(&QComboBox::currentIndexChanged),this,&Window::preview);
    auto *bottom=new QHBoxLayout;
    auto *save=button("Сохранить кривые и Functions в N14 Control",bottom); save->setObjectName("saveCurves");
    connect(save,&QPushButton::clicked,this,&Window::saveCurves);
    bottom->addStretch(); layout->addLayout(bottom);
    layout->addWidget(text("Фильтры предпросмотра: Identity, пороговый Hysteresis и скользящее среднее. Они не заменяют native CoolerControl Standard. Кривые и Standard-параметры сохранены в отдельном N14 Control contract; live loop работает автоматически при доступном broker ownership и validated sampler snapshot. GPU во сне исключается из Mix, а не подменяется температурой CPU или нулём."));
    auto *scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setWidget(page);
    tabs_->addTab(scroll,"Кривые · предпросмотр"); preview();
}
void Window::buildSystem() {
    auto *page=new QWidget; auto *layout=new QVBoxLayout(page);
    diagnostics_=text("—"); diagnostics_->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(diagnostics_);
    layout->addWidget(text("Доступность аппаратных функций показана на вкладке «Оборудование». CPU и EC writes выполняются только отдельным root service; runtime-PM NVIDIA и MUX здесь не меняются. D3cold — наблюдаемое состояние, а не гарантируемый переключатель."));
    layout->addWidget(text("Fan и keylight используют одну сериализованную EC transaction queue. Ownership и режим вентилятора выбираются автоматически; stale snapshot только откладывает команду до следующего sample, а ошибка физической записи/readback остаётся fail-closed."));
    layout->addWidget(text("Журнал приложения (последние 200 сообщений):"));
    journal_=new QPlainTextEdit; journal_->setReadOnly(true); journal_->setMaximumBlockCount(200); layout->addWidget(journal_);
    tabs_->addTab(page,"Система и журнал");
}
void Window::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_=new QSystemTrayIcon(QIcon(":/n14-control.svg"),this); auto *menu=new QMenu(this);
    connect(menu->addAction("Открыть N14 Control"),&QAction::triggered,this,&Window::showWindow);
    menu->addSeparator();
    for (const auto &p : {QString("power-saver"),QString("balanced"),QString("performance")})
        connect(menu->addAction(profileLabel(p)),&QAction::triggered,this,[this,p]{ manualProfile(p); });
    menu->addSeparator();
    connect(menu->addAction("Завершить (автоматика остановится)"),&QAction::triggered,qApp,&QApplication::quit);
    tray_->setContextMenu(menu);
    connect(tray_,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason r){
        if (r==QSystemTrayIcon::Trigger || r==QSystemTrayIcon::DoubleClick) showWindow();
    }); tray_->show();
}
bool Window::trayAvailable() const { return tray_ && tray_->isVisible(); }
void Window::showWindow() {
    historyPlot_->update();
    if (tabs_->currentIndex()==2) previewTimer_.start();
    showNormal(); raise(); activateWindow();
}
void Window::closeEvent(QCloseEvent *event) {
    if (trayAvailable()) { hide(); previewTimer_.stop(); event->ignore(); }
    else QMainWindow::closeEvent(event);
}
void Window::log(const QString &message,bool error) {
    if (journal_) journal_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ")+(error ? "ОШИБКА: " : "")+message);
    statusBar()->showMessage(message,error ? 15000 : 7000);
}
void Window::sample() {
    if (sleeping_) return;
    if (demo_) {
        Snapshot snapshot;
        snapshot.timestampMs=QDateTime::currentMSecsSinceEpoch(); snapshot.kernel="demo";
        snapshot.batteryPresent=true; snapshot.externalPower=false; snapshot.batteryStatus="Discharging";
        snapshot.percent=73; snapshot.powerW=12.5+std::sin(double(snapshot.timestampMs)/9000);
        snapshot.cpuC=48; snapshot.gpuRuntime="suspended"; snapshot.gpuPower="D3cold";
        acceptSample(std::move(snapshot)); return;
    }
    const auto revision=telemetryRevision_;
    telemetry_.request([this,revision](Snapshot snapshot,QString error) {
        if (revision!=telemetryRevision_ || sleeping_) return;
        if (!error.isEmpty()) {
            if (!telemetryFailed_) log("Пассивная телеметрия недоступна: "+error,true);
            telemetryFailed_=true; return;
        }
        telemetryFailed_=false;
        const auto age=QDateTime::currentMSecsSinceEpoch()-snapshot.timestampMs;
        if (age<0 || age>2000) return; // never automate from a stalled pre-sleep read
        acceptSample(std::move(snapshot));
    });
}
void Window::acceptSample(Snapshot snapshot) {
    snapshot_=std::move(snapshot);
    history_.append(snapshot_); while (history_.size()>720) history_.removeFirst();
    batteryValue_->setText(snapshot_.percent>=0 ? QString::number(snapshot_.percent)+" %" : "Нет данных");
    const QString state=snapshot_.batteryStatus=="Discharging" ? "Разряд" : snapshot_.batteryStatus=="Charging" ? "Заряд" : "Не разряжается";
    powerValue_->setText(watts(snapshot_.powerW)+"\n"+state);
    gpuValue_->setText(snapshot_.gpuPower.isEmpty() ? "Не найдена" : snapshot_.gpuPower+"\n"+snapshot_.gpuRuntime);
    cpuValue_->setText(snapshot_.cpuC ? QString::number(*snapshot_.cpuC,'f',1)+" °C" : "Нет данных");
    double sum=0; int count=0;
    for (const auto &s : history_) if (s.timestampMs>=snapshot_.timestampMs-60000 && s.batteryStatus=="Discharging" && s.powerW) { sum+=*s.powerW; ++count; }
    average_->setText(count ? QString("Средний разряд за последние 60 секунд: %1 Вт (%2 отсч.)").arg(sum/count,0,'f',2).arg(count)
                           : "Средний разряд за последние 60 секунд: нет отсчётов разряда");
    if (isVisible() && tabs_->currentIndex()==0) historyPlot_->update();
    diagnostics_->setText(QString("Ядро: %1\nRCU Lazy: %2\nNVIDIA: %3 / %4\nRuntime PM control: %5\nD3cold allowed: %6\nGPU suspended, суммарно: %7 сек.\nКонфигурация: %8")
        .arg(snapshot_.kernel,snapshot_.rcuLazy.isEmpty() ? "нет доступного параметра" : snapshot_.rcuLazy,
             snapshot_.gpuPower,snapshot_.gpuRuntime,snapshot_.gpuControl,snapshot_.d3coldAllowed,
             snapshot_.gpuSuspendedSeconds ? QString::number(*snapshot_.gpuSuspendedSeconds,'f',0) : "нет данных",settingsPath_));
    if (tray_) tray_->setToolTip(QString("N14 Control · %1% · %2 · GPU %3").arg(snapshot_.percent).arg(watts(snapshot_.powerW),snapshot_.gpuPower));
    applyRules();
}
void Window::applyRules() {
    if (pendingPreset_ || sleeping_) return;
    const auto p=rules_.observe(snapshot_.externalPower,snapshot_.percent,settings_);
    if (!p) return;
    if (p->preset) {
        const QString key=p->reason+QString::fromUtf8(QJsonDocument(p->preset->toJson()).toJson(QJsonDocument::Compact));
        if (attemptedPreset_!=key) { attemptedPreset_=key; log("Автоматика пресетов: "+p->reason); startPreset(*p->preset,false); }
        return;
    }
    if (p->profile && (backend_.powerAvailable() || demo_)) {
        const QString key=p->reason+*p->profile;
        if (attemptedProfile_!=key || !backend_.profileMatches(*p->profile)) {
            attemptedProfile_=key;
            log("Автоматика: "+p->reason);
            backend_.setProfile(*p->profile);
        }
    }
    if (p->hz && backend_.panel() && !backend_.screenBusy()) {
        const QString key=p->reason+QString::number(*p->hz);
        if (attemptedHz_!=key) { attemptedHz_=key; backend_.setHz(*p->hz); }
    }
}
void Window::syncRulesUi() {
    const QSignalBlocker a(autoProfile_),b(autoHz_),c(acProfile_),d(batteryProfile_),e(lowProfile_),f(acHz_),g(batteryHz_),h(lowPercent_);
    autoProfile_->setChecked(settings_.autoProfile); autoHz_->setChecked(settings_.autoRefresh);
    acProfile_->setCurrentIndex(acProfile_->findData(settings_.acProfile)); batteryProfile_->setCurrentIndex(batteryProfile_->findData(settings_.batteryProfile));
    lowProfile_->setCurrentIndex(lowProfile_->findData(settings_.lowProfile)); acHz_->setCurrentIndex(acHz_->findData(settings_.acHz));
    batteryHz_->setCurrentIndex(batteryHz_->findData(settings_.batteryHz)); lowPercent_->setValue(settings_.lowPercent);
}
void Window::saveRules() {
    Settings next=settings_; next.autoProfile=autoProfile_->isChecked(); next.autoRefresh=autoHz_->isChecked();
    if (next.autoProfile || next.autoRefresh) next.autoPresets=false;
    next.acProfile=acProfile_->currentData().toString(); next.batteryProfile=batteryProfile_->currentData().toString(); next.lowProfile=lowProfile_->currentData().toString();
    next.acHz=acHz_->currentData().toInt(); next.batteryHz=batteryHz_->currentData().toInt(); next.lowPercent=lowPercent_->value();
    try { next.save(settingsPath_); settings_=next; rules_.reset(); attemptedProfile_.clear(); attemptedHz_.clear(); attemptedPreset_.clear(); syncPresetUi(); log("Правила сохранены"); }
    catch (const std::exception &e) { syncRulesUi(); log(QString::fromUtf8(e.what()),true); }
}
void Window::manualProfile(const QString &profile) {
    if (sleeping_) { log("Подготовка ко сну: ручное переключение отложено; повторите после пробуждения",true); return; }
    if (pendingPreset_) { log("Дождитесь завершения применения пресета",true); return; }
    if (settings_.autoPresets) {
        Settings next=settings_; next.autoPresets=false;
        if (!commitSettings(next,"Автоматика пресетов отключена ручным выбором")) return;
    }
    autoProfile_->setChecked(false);
    if (!settings_.autoProfile) backend_.setProfile(profile);
}
void Window::manualHz(int hz) {
    if (sleeping_) { log("Подготовка ко сну: ручное переключение отложено; повторите после пробуждения",true); return; }
    if (pendingPreset_) { log("Дождитесь завершения применения пресета",true); return; }
    if (settings_.autoPresets) {
        Settings next=settings_; next.autoPresets=false;
        if (!commitSettings(next,"Автоматика пресетов отключена ручным выбором")) return;
    }
    autoHz_->setChecked(false);
    if (!settings_.autoRefresh) backend_.setHz(hz);
}
void Window::saveCurves() {
    try {
        Settings next=settings_; next.cpu=curveFromTable(cpuTable_); next.gpu=curveFromTable(gpuTable_); next.mix=mix_->currentText();
        FunctionSettings *out[]={&next.cpuFunction,&next.gpuFunction};
        for (int r=0;r<2;++r) *out[r]={filterKind_[r]->currentText(),deviance_[r]->value(),window_[r]->value()};
        reference::StandardFunction *standard[]={&next.coolerCpuFunction,&next.coolerGpuFunction};
        for (int r=0;r<2;++r) {
            standard[r]->stepMinimum=standardStepMinimum_[r]->value(); standard[r]->stepMaximum=standardStepMaximum_[r]->value();
            standard[r]->stepMinimumDecreasing=standardStepMinimumDecreasing_[r]->value(); standard[r]->stepMaximumDecreasing=standardStepMaximumDecreasing_[r]->value();
            standard[r]->responseDelay=standardResponseDelay_[r]->value(); standard[r]->deviance=standardDeviance_[r]->value();
            standard[r]->onlyDownward=standardOnlyDownward_[r]->isChecked(); standard[r]->thresholdHopping=standardThresholdHopping_[r]->isChecked();
            standard[r]->bypassMinimumAtExtremes=standardExtremeBypass_[r]->isChecked();
        }
        if (!demo_ && next.mix!="Max") throw std::runtime_error("Live Standard поддерживает только Max; Min/Mean доступны в предпросмотре");
        next.save(settingsPath_); settings_=next; log("Кривые и Standard сохранены; фоновый контур перечитает их на следующем цикле. После отказа требуется явное включение кривой");
    } catch (const std::exception &e) { log(QString::fromUtf8(e.what()),true); }
}
void Window::preview() {
    if (!curvePlot_ || !previewValue_) return;
    try {
        const auto cpu=curveFromTable(cpuTable_), gpu=curveFromTable(gpuTable_);
        curvePlot_->cpu=cpu; curvePlot_->gpu=gpu; curvePlot_->update();
        const auto c=filters_[0].update(cpuInput_->value(),filterKind_[0]->currentText(),deviance_[0]->value(),window_[0]->value());
        const std::optional<double> input=gpuAsleep_->isChecked() ? std::nullopt : std::optional<double>(gpuInput_->value());
        const auto g=filters_[1].update(input,filterKind_[1]->currentText(),deviance_[1]->value(),window_[1]->value());
        const auto result=mixDuty(cpu,gpu,c,g,gpuAsleep_->isChecked(),mix_->currentText());
        previewValue_->setText(QString("Симуляция: CPU %1 °C → %2 °C; GPU %3. Расчётная команда: %4%. Не измеренные обороты.")
            .arg(cpuInput_->value()).arg(c ? *c : 0,0,'f',1)
            .arg(g ? QString::number(*g,'f',1)+" °C" : "D3cold — исключена из Mix")
            .arg(result ? *result : 0,0,'f',1));
    } catch (const std::exception &e) { previewValue_->setText(QString::fromUtf8(e.what())); }
}
void Window::refreshBackendLabels() {
    profileValue_->setText("Профиль: "+profileLabel(backend_.activeProfile()));
    const auto panel=backend_.panel();
    const auto mode=panel ? panel->current() : std::nullopt;
    screenValue_->setText(panel && mode ? QString("Экран: %1 · %2×%3 · %4 Гц").arg(panel->name).arg(mode->width).arg(mode->height).arg(mode->hz,0,'f',2)
                                       : "Экран: нет подтверждённых данных KScreen");
}
void Window::exportCsv() {
    const QString path=QFileDialog::getSaveFileName(this,"Экспорт истории",QDir::homePath()+"/n14-power.csv","CSV (*.csv)");
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { log("Не удалось открыть CSV",true); return; }
    QByteArray out="timestamp,battery_status,capacity_percent,battery_power_w,external_power,gpu_power_state,gpu_runtime_status\n";
    auto csv=[](QString value){ value.replace('"',"\"\""); return "\""+value+"\""; };
    for (const auto &s : history_) {
        QStringList row{csv(QDateTime::fromMSecsSinceEpoch(s.timestampMs).toString(Qt::ISODateWithMs)),csv(s.batteryStatus),
            s.percent>=0 ? QString::number(s.percent) : "",s.powerW ? QString::number(*s.powerW,'f',3) : "",
            s.externalPower ? (*s.externalPower ? "1" : "0") : "",csv(s.gpuPower),csv(s.gpuRuntime)};
        out+=row.join(',').toUtf8()+'\n';
    }
    if (file.write(out)!=out.size() || !file.commit()) log("Ошибка записи CSV",true);
    else log("История сохранена: "+path);
}
} // namespace n14