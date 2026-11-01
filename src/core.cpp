#include "core.h"
#include "reference_json.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <numeric>

namespace n14 {
static void require(bool condition, const QString &error) {
    if (!condition) throw std::runtime_error(error.toStdString());
}
static bool profileValid(const QString &p) {
    return QStringList{"power-saver", "balanced", "performance"}.contains(p);
}
QString profileLabel(const QString &p) {
    if (p == "power-saver") return "Энергосбережение";
    if (p == "balanced") return "Сбалансированный";
    if (p == "performance") return "Производительность";
    return p.isEmpty() ? "Недоступен" : p;
}
QString validateCurve(const Curve &curve) {
    if (curve.size() < 2 || curve.size() > 64) return "Нужно от 2 до 64 точек";
    double previousTemp = -1, previousDuty = -1;
    for (const auto &p : curve) {
        if (!std::isfinite(p.temperature) || !std::isfinite(p.duty) ||
            p.temperature < 0 || p.temperature > 120 || p.duty < 0 || p.duty > 100)
            return "Температура: 0–120 °C, скорость: 0–100%";
        if (p.temperature <= previousTemp) return "Температуры должны строго возрастать";
        if (p.duty < previousDuty) return "Скорость не должна уменьшаться с температурой";
        previousTemp = p.temperature; previousDuty = p.duty;
    }
    if (curve.last().duty != 100) return "Последняя точка должна задавать 100%";
    return {};
}
double curveDuty(const Curve &curve, double temperature) {
    require(validateCurve(curve).isEmpty() && std::isfinite(temperature), "Некорректная кривая/температура");
    if (temperature <= curve.first().temperature) return curve.first().duty;
    for (qsizetype i = 1; i < curve.size(); ++i) {
        const auto a = curve[i-1], b = curve[i];
        if (temperature <= b.temperature)
            return a.duty + (b.duty-a.duty)*(temperature-a.temperature)/(b.temperature-a.temperature);
    }
    return curve.last().duty;
}
std::optional<double> mixDuty(const Curve &cpu, const Curve &gpu,
                             std::optional<double> c, std::optional<double> g,
                             bool asleep, const QString &mix) {
    require(QStringList{"Max","Min","Mean"}.contains(mix), "Неизвестная функция Mix");
    if (!c || !std::isfinite(*c)) return {};
    const double a = curveDuty(cpu, *c);
    if (asleep) return a; // Missing active GPU is NOT the same as sleeping GPU.
    if (!g || !std::isfinite(*g)) return {};
    const double b = curveDuty(gpu, *g);
    if (mix == "Min") return std::min(a,b);
    if (mix == "Mean") return (a+b)/2;
    return std::max(a,b);
}
QJsonArray curveJson(const Curve &curve) {
    QJsonArray out;
    for (const auto &p : curve) out.append(QJsonArray{p.temperature,p.duty});
    return out;
}
Curve parseCurve(const QJsonValue &json) {
    require(json.isArray(), "Кривая должна быть массивом");
    Curve curve;
    for (const auto &v : json.toArray()) {
        require(v.isArray() && v.toArray().size() == 2, "Некорректная точка");
        const auto a = v.toArray();
        require(a[0].isDouble() && a[1].isDouble(), "Точка должна содержать числа");
        curve.append({a[0].toDouble(),a[1].toDouble()});
    }
    require(validateCurve(curve).isEmpty(),validateCurve(curve));
    return curve;
}
QJsonObject FunctionSettings::toJson() const {
    return {{"kind",kind},{"deviance",deviance},{"window",window}};
}
FunctionSettings FunctionSettings::fromJson(const QJsonValue &json) {
    require(json.isObject(),"Функция должна быть объектом");
    const auto j=json.toObject();
    FunctionSettings f;
    require(j.keys()==f.toJson().keys() && j["kind"].isString() &&
            QStringList{"Identity","Hysteresis","MovingAverage"}.contains(j["kind"].toString()),"Некорректная функция");
    require(j["deviance"].isDouble() && j["deviance"].toDouble()>=0 && j["deviance"].toDouble()<=30,
            "Гистерезис: 0–30 °C");
    require(j["window"].isDouble() && j["window"].toDouble()==j["window"].toInt() &&
            j["window"].toInt()>=1 && j["window"].toInt()<=60,"Окно: 1–60 отсчётов");
    f.kind=j["kind"].toString(); f.deviance=j["deviance"].toDouble(); f.window=j["window"].toInt();
    return f;
}
QJsonObject Settings::toJson() const {
    QJsonArray list; for (const auto &p : presets) list.append(p.toJson());
    return {{"schema",3},{"auto_profile",autoProfile},{"auto_refresh",autoRefresh},
            {"ac_profile",acProfile},{"battery_profile",batteryProfile},{"low_profile",lowProfile},
            {"ac_hz",acHz},{"battery_hz",batteryHz},{"low_percent",lowPercent},
            {"cpu_curve",curveJson(cpu)},{"gpu_curve",curveJson(gpu)},{"mix",mix},
            {"cpu_function",cpuFunction.toJson()},{"gpu_function",gpuFunction.toJson()},
            {"coolercontrol", QJsonObject{
                {"cpu_function", reference::standardFunctionJson(coolerCpuFunction)},
                {"gpu_function", reference::standardFunctionJson(coolerGpuFunction)}}},
            {"auto_presets",autoPresets},{"presets",list},{"ac_preset",acPreset},
            {"battery_preset",batteryPreset},{"low_preset",lowPreset}};
}
Settings Settings::fromJson(const QJsonObject &input) {
    Settings s;
    QJsonObject j=input;
    if (j["schema"].isDouble() && j["schema"].toDouble()==1) {
        auto legacy=s.toJson();
        legacy.remove("coolercontrol");
        for (const auto &key : {"auto_presets","presets","ac_preset","battery_preset","low_preset"}) legacy.remove(key);
        require(j.keys()==legacy.keys(),"Неполная или неизвестная старая схема настроек");
        j["schema"]=3;
        for (const auto &key : {"auto_presets","presets","ac_preset","battery_preset","low_preset"}) j[key]=s.toJson()[key];
        j["coolercontrol"] = s.toJson()["coolercontrol"];
    } else if (j["schema"].isDouble() && j["schema"].toDouble()==2) {
        auto legacy=s.toJson();
        legacy.remove("coolercontrol"); legacy["schema"]=2;
        require(j.keys()==legacy.keys(),"Неполная или неизвестная схема настроек 2");
        j["schema"]=3;
        j["coolercontrol"] = s.toJson()["coolercontrol"];
    }
    require(j.keys() == s.toJson().keys(), "Неполная или неизвестная схема настроек");
    require(j["schema"].isDouble() && j["schema"].toDouble() == 3, "Неизвестная версия настроек");
    require(j["auto_profile"].isBool() && j["auto_refresh"].isBool(), "Автоматизация должна быть bool");
    s.autoProfile=j["auto_profile"].toBool(); s.autoRefresh=j["auto_refresh"].toBool();
    for (const QString k : {"ac_profile","battery_profile","low_profile"})
        require(j[k].isString() && profileValid(j[k].toString()), "Неизвестный профиль питания");
    s.acProfile=j["ac_profile"].toString(); s.batteryProfile=j["battery_profile"].toString();
    s.lowProfile=j["low_profile"].toString();
    for (const QString k : {"ac_hz","battery_hz"})
        require(j[k].isDouble() && (j[k].toDouble()==60 || j[k].toDouble()==165), "Частота: только 60 или 165 Гц");
    require(j["low_percent"].isDouble() && j["low_percent"].toDouble()==j["low_percent"].toInt()
            && j["low_percent"].toInt()>=5 && j["low_percent"].toInt()<=50, "Порог: 5–50%");
    s.acHz=j["ac_hz"].toInt(); s.batteryHz=j["battery_hz"].toInt(); s.lowPercent=j["low_percent"].toInt();
    s.cpu=parseCurve(j["cpu_curve"]); s.gpu=parseCurve(j["gpu_curve"]);
    require(j["mix"].isString() && QStringList{"Max","Min","Mean"}.contains(j["mix"].toString()), "Некорректный Mix");
    s.mix=j["mix"].toString();
    s.cpuFunction=FunctionSettings::fromJson(j["cpu_function"]);
    s.gpuFunction=FunctionSettings::fromJson(j["gpu_function"]);
    require(j["coolercontrol"].isObject() &&
                j["coolercontrol"].toObject().keys() == QStringList{"cpu_function", "gpu_function"},
            "Некорректный native CoolerControl contract");
    const auto native = j["coolercontrol"].toObject();
    s.coolerCpuFunction=reference::parseStandardFunction(native["cpu_function"]);
    s.coolerGpuFunction=reference::parseStandardFunction(native["gpu_function"]);
    require(j["auto_presets"].isBool() && j["presets"].isArray(),"Некорректные настройки пресетов");
    s.autoPresets=j["auto_presets"].toBool();
    require(!s.autoPresets || (!s.autoProfile && !s.autoRefresh),"Нельзя одновременно включить две системы автоматизации");
    const auto list=j["presets"].toArray();
    require(!list.isEmpty() && list.size()<=16,"Нужно от 1 до 16 пресетов");
    s.presets.clear(); QStringList ids;
    for (const auto &v : list) {
        require(v.isObject(),"Пресет должен быть объектом");
        const auto p=Preset::fromJson(v.toObject());
        require(!ids.contains(p.id),"Повторяющийся ID пресета"); ids.append(p.id); s.presets.append(p);
    }
    for (const auto &key : {"ac_preset","battery_preset","low_preset"})
        require(j[key].isString() && ids.contains(j[key].toString()),"Правило ссылается на отсутствующий пресет");
    s.acPreset=j["ac_preset"].toString(); s.batteryPreset=j["battery_preset"].toString(); s.lowPreset=j["low_preset"].toString();
    return s;
}
Settings Settings::load(const QString &path) {
    QFile f(path);
    if (!QFileInfo::exists(path)) return {};
    require(f.size() <= 65536 && f.open(QIODevice::ReadOnly), "Не удалось прочитать настройки");
    const QByteArray raw=f.readAll();
    require(!reference::hasDuplicateJsonMembers(raw), "Повторяющееся поле JSON настроек");
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(raw,&error);
    require(error.error==QJsonParseError::NoError && doc.isObject(), "Повреждённый JSON настроек");
    return fromJson(doc.object());
}
void Settings::save(const QString &path) const {
    fromJson(toJson());
    require(!QFileInfo(path).isSymLink(),"Отказ записи через символическую ссылку");
    require(QDir().mkpath(QFileInfo(path).absolutePath()),"Не удалось создать каталог настроек");
    QFile previous(path);
    if (previous.exists() && previous.open(QIODevice::ReadOnly)) {
        require(previous.size()<=65536,"Существующая конфигурация слишком большая");
        const auto data=previous.readAll(); const auto old=QJsonDocument::fromJson(data);
        if (old.isObject() && old.object()["schema"].toInt()==1 && !QFileInfo::exists(path+".schema1.bak")) {
            require(!QFileInfo(path+".schema1.bak").isSymLink(),"Отказ записи резервной копии через ссылку");
            QSaveFile backup(path+".schema1.bak"); require(backup.open(QIODevice::WriteOnly),"Не удалось создать копию старой конфигурации");
            backup.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
            require(backup.write(data)==data.size() && backup.commit(),"Не удалось сохранить копию старой конфигурации");
        }
    }
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly),"Не удалось сохранить настройки");
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    const QByteArray data=QJsonDocument(toJson()).toJson();
    require(file.write(data)==data.size() && file.commit(),"Не удалось завершить запись настроек");
}
void RuleEngine::reset() { candidate_.clear(); confirmations_=0; }
std::optional<DesiredPolicy> RuleEngine::observe(std::optional<bool> ac, int percent, const Settings &s) {
    if (!ac) { reset(); return {}; }
    const bool low=!*ac && percent>=0 && percent<=s.lowPercent;
    const QString key=*ac ? "ac" : low ? "low" : "battery";
    if (candidate_!=key) { candidate_=key; confirmations_=1; return {}; }
    confirmations_=2;
    DesiredPolicy p;
    p.reason=*ac ? "Питание от сети" : low ? "Низкий заряд аккумулятора" : "Питание от аккумулятора";
    if (s.autoPresets) {
        p.preset=s.preset(*ac ? s.acPreset : low ? s.lowPreset : s.batteryPreset);
        return p;
    }
    if (s.autoProfile) p.profile=*ac ? s.acProfile : low ? s.lowProfile : s.batteryProfile;
    if (s.autoRefresh) p.hz=*ac ? s.acHz : s.batteryHz;
    return p;
}
void TemperatureFilter::reset() { accepted_.reset(); history_.clear(); }
std::optional<double> TemperatureFilter::update(std::optional<double> v, const QString &kind,
                                               double deviance, int window) {
    require(QStringList{"Identity","Hysteresis","MovingAverage"}.contains(kind),"Неизвестный фильтр");
    require(std::isfinite(deviance) && deviance>=0 && deviance<=30 && window>=1 && window<=60,
            "Некорректные параметры фильтра");
    if (!v || !std::isfinite(*v)) { reset(); return {}; }
    if (kind=="Identity") return v;
    if (kind=="Hysteresis") {
        if (!accepted_ || std::abs(*v-*accepted_)>=deviance) accepted_=v;
        return accepted_;
    }
    history_.append(*v);
    while (history_.size()>window) history_.removeFirst();
    return std::accumulate(history_.cbegin(),history_.cend(),0.0)/double(history_.size());
}
std::optional<ScreenMode> Panel::current() const {
    for (const auto &m : modes) if (m.id==currentMode) return m;
    return {};
}
std::optional<ScreenMode> Panel::modeForHz(int hz) const {
    const auto now=current();
    if (!now || (hz!=60 && hz!=165)) return {};
    for (const auto &m : modes)
        if (m.width==now->width && m.height==now->height && std::abs(m.hz-hz)<0.6) return m;
    return {};
}
std::optional<Panel> parsePanel(const QByteArray &json) {
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(json,&error);
    require(error.error==QJsonParseError::NoError && doc.isObject() && doc.object()["outputs"].isArray(),
            "Некорректный ответ KScreen");
    std::optional<Panel> panel;
    for (const auto &value : doc.object()["outputs"].toArray()) {
        const auto o=value.toObject();
        const QString name=o["name"].toString();
        if (!name.startsWith("eDP-") || !o["connected"].toBool() || !o["enabled"].toBool()) continue;
        require(!panel,"Несколько встроенных экранов: выбор неоднозначен");
        Panel p; p.id=o["id"].toInt(); p.name=name; p.currentMode=o["currentModeId"].toString();
        require(p.id>0,"Некорректный ID экрана");
        for (const auto &mv : o["modes"].toArray()) {
            const auto m=mv.toObject(); const auto size=m["size"].toObject();
            ScreenMode mode{m["id"].toString(),size["width"].toInt(),size["height"].toInt(),m["refreshRate"].toDouble()};
            require(QRegularExpression("^[0-9]+$").match(mode.id).hasMatch() && mode.width>0
                    && mode.height>0 && mode.hz>0,"Некорректный режим экрана");
            p.modes.append(mode);
        }
        require(p.current().has_value(),"Текущий режим экрана не найден");
        panel=p;
    }
    return panel;
}
} // namespace n14