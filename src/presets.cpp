#include "core.h"
#include <QRegularExpression>
#include <stdexcept>

namespace n14 {
static void check(bool ok,const char *message) { if (!ok) throw std::runtime_error(message); }
QJsonObject Preset::toJson() const {
    return {{"id",id},{"name",name},{"power_profile",powerProfile},{"hz",hz},{"cpu",cpu.toJson()}};
}
Preset Preset::fromJson(const QJsonObject &j) {
    Preset p;
    check(j.keys()==p.toJson().keys(),"Неизвестные/отсутствующие параметры пресета");
    check(j["id"].isString() && QRegularExpression("^[a-zA-Z0-9_-]{1,64}$").match(j["id"].toString()).hasMatch(),"Некорректный ID пресета");
    check(j["name"].isString() && !j["name"].toString().trimmed().isEmpty() && j["name"].toString().size()<=80,"Название пресета: 1–80 символов");
    check(j["power_profile"].isString() && QStringList{"","power-saver","balanced","performance"}.contains(j["power_profile"].toString()),"Неизвестный системный профиль пресета");
    check(j["hz"].isDouble() && (j["hz"].toDouble()==0 || j["hz"].toDouble()==60 || j["hz"].toDouble()==165),"Частота пресета: 60/165 Гц или не изменять");
    check(j["cpu"].isObject(),"Отсутствуют параметры CPU пресета");
    p.id=j["id"].toString(); p.name=j["name"].toString().trimmed(); p.powerProfile=j["power_profile"].toString();
    p.hz=j["hz"].toInt(); p.cpu=CpuRequest::fromJson(j["cpu"].toObject()); return p;
}
QVector<Preset> defaultPresets() {
    // These are editable examples, NOT automatically enabled policies.
    return {{"ac","От сети","performance",165,{"hardware",2400,-1}},
            {"battery","От аккумулятора","power-saver",60,{"limit",2400,-1}},
            {"low","Низкий заряд","power-saver",60,{"limit",1600,-1}}};
}
const Preset &Settings::preset(const QString &id) const {
    for (const auto &p : presets) if (p.id==id) return p;
    throw std::runtime_error("Пресет не найден");
}
} // namespace n14