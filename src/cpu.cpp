#include "cpu.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include <algorithm>

namespace n14 {
static void check(bool ok,const QString &message) {
    if (!ok) throw std::runtime_error(message.toStdString());
}
static QByteArray read(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly),"Не удалось прочитать "+path);
    return file.read(256).trimmed();
}
static void confirm(const QString &path,const QByteArray &expected,
                    const QElapsedTimer &budget,const QString &context) {
    // cpufreq sysfs writes update freq QoS, which schedules policy work.
    // Wait only for exact readback, never issue a write retry. All changes
    // share ONE 250 ms phase budget, rather than 250 ms per CPU policy.
    QByteArray actual;
    for (;;) {
        actual=read(path);
        if (actual==expected) return;
        if (budget.elapsed()>=250) break;
        QThread::msleep(1);
    }
    throw std::runtime_error((context+" "+path+": ожидалось "+QString::fromLatin1(expected)+
        ", прочитано "+QString::fromLatin1(actual)+" (таймаут подтверждения CPU 250 мс)").toStdString());
}
static int number(const QString &path) {
    bool ok=false; const int n=read(path).toInt(&ok);
    check(ok && n>=0,"Некорректное значение "+path); return n;
}
QJsonObject CpuRequest::toJson() const { return {{"mode",mode},{"max_mhz",maxMHz},{"turbo",turbo}}; }
CpuRequest CpuRequest::fromJson(const QJsonObject &j) {
    CpuRequest r;
    check(j.keys()==r.toJson().keys(),"Неизвестные/отсутствующие параметры CPU");
    check(j["mode"].isString() && QStringList{"keep","limit","hardware"}.contains(j["mode"].toString()),"Неизвестный режим CPU");
    for (const auto &key : {QString("max_mhz"),QString("turbo")})
        check(j[key].isDouble() && j[key].toDouble()==j[key].toInt(),"Параметр CPU должен быть целым числом");
    r.mode=j["mode"].toString(); r.maxMHz=j["max_mhz"].toInt(); r.turbo=j["turbo"].toInt();
    check(r.maxMHz>=400 && r.maxMHz<=10000,"Предел CPU: 400–10000 МГц");
    check(r.turbo>=-1 && r.turbo<=1,"Недопустимый режим Turbo Boost");
    return r;
}
QJsonObject CpuPolicy::toJson() const {
    return {{"name",name},{"driver",driver},{"hardware_min_khz",hardwareMin},{"hardware_max_khz",hardwareMax},
            {"min_khz",currentMin},{"max_khz",currentMax}};
}
CpuController::CpuController(QString sysRoot,Writer writer) : root_(std::move(sysRoot)),writer_(std::move(writer)) {
    if (!writer_) writer_=[](const QString &path,const QByteArray &data){
        QFile file(path);
        check(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Нет доступа для записи "+path);
        check(file.write(data)==data.size() && file.flush(),"Ошибка записи "+path);
    };
}
QVector<CpuPolicy> CpuController::policies() const {
    QVector<CpuPolicy> out;
    const QDir dir(root_+"/devices/system/cpu/cpufreq");
    for (const auto &name : dir.entryList({"policy*"},QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
        check(QRegularExpression("^policy[0-9]+$").match(name).hasMatch(),"Некорректное имя cpufreq policy");
        const QString p=dir.filePath(name)+"/";
        CpuPolicy policy{name,QString::fromLatin1(read(p+"scaling_driver")),number(p+"cpuinfo_min_freq"),
                         number(p+"cpuinfo_max_freq"),number(p+"scaling_min_freq"),number(p+"scaling_max_freq")};
        check(policy.hardwareMin>0 && policy.hardwareMax>=policy.hardwareMin && policy.currentMin>=policy.hardwareMin
              && policy.currentMax>=policy.currentMin && policy.currentMax<=policy.hardwareMax,"Несогласованные диапазоны CPU "+name);
        out.append(policy);
    }
    check(!out.isEmpty(),"cpufreq policies недоступны"); return out;
}
QJsonObject CpuController::inspect() const {
    QJsonObject out{{"supported",false},{"turbo_supported",false}};
    try {
        QJsonArray list; for (const auto &p : policies()) list.append(p.toJson());
        out["policies"]=list; out["supported"]=true;
    } catch (const std::exception &e) { out["reason"]=QString::fromUtf8(e.what()); }
    try {
        const int disabled=number(root_+"/devices/system/cpu/intel_pstate/no_turbo");
        check(disabled==0 || disabled==1,"Неизвестное состояние Turbo Boost");
        out["turbo_supported"]=true; out["turbo_allowed"]=disabled==0;
    } catch (const std::exception &) { /* Unsupported is explicit, never assumed enabled. */ }
    return out;
}
QJsonObject CpuController::apply(const CpuRequest &input) {
    const auto r=CpuRequest::fromJson(input.toJson());
    if (!r.changesHardware()) return inspect();
    struct Change { QString path; QByteArray before,after; };
    QVector<Change> plan;
    // Validate every policy before the first write. No caller-supplied paths.
    const auto list=policies();
    if (r.turbo!=-1) {
        const QString path=root_+"/devices/system/cpu/intel_pstate/no_turbo";
        const auto before=read(path); check(before=="0" || before=="1","Turbo Boost недоступен");
        plan.append({path,before,r.turbo ? QByteArray("0") : QByteArray("1")});
    }
    if (r.mode!="keep") for (const auto &p : list) {
        const int target=r.mode=="hardware" ? p.hardwareMax : std::min(r.maxMHz*1000,p.hardwareMax);
        check(target>=p.currentMin && target>=p.hardwareMin,"Предел ниже текущего минимума "+p.name+"; настройки не изменены");
        plan.append({root_+"/devices/system/cpu/cpufreq/"+p.name+"/scaling_max_freq",QByteArray::number(p.currentMax),QByteArray::number(target)});
    }
    QVector<Change> attempted;
    QElapsedTimer readbackBudget; readbackBudget.start();
    try {
        for (const auto &c : plan) {
            if (c.before==c.after) continue;
            attempted.append(c); // Also rollback a write that failed after partial modification.
            writer_(c.path,c.after+'\n');
            confirm(c.path,c.after,readbackBudget,"CPU не подтвердил значение");
        }
        for (const auto &c : plan) confirm(c.path,c.after,readbackBudget,"CPU изменился после записи");
        for (const auto &c : plan) check(read(c.path)==c.after,"CPU изменился во время применения; конфликт с другим контроллером");
        const auto state=inspect();
        check(state["supported"].toBool(),"CPU недоступен после записи; состояние не подтверждено");
        if (r.turbo!=-1) check(state["turbo_supported"].toBool() && state["turbo_allowed"].toBool()==(r.turbo==1),"Turbo Boost не подтверждён после записи");
        return state;
    } catch (const std::exception &e) {
        QStringList failed;
        QElapsedTimer rollbackBudget; rollbackBudget.start();
        // Restore turbo first: it can affect the allowed policy bounds.
        auto undo=[&](const Change &c){
            try {
                writer_(c.path,c.before+'\n');
                confirm(c.path,c.before,rollbackBudget,"Проверка отката не пройдена");
            } catch (const std::exception &error) {
                failed.append(c.path+" ["+QString::fromUtf8(error.what())+"]");
            }
        };
        for (const auto &c : attempted) if (c.path.endsWith("/no_turbo")) undo(c);
        for (auto i=attempted.crbegin();i!=attempted.crend();++i) if (!i->path.endsWith("/no_turbo")) undo(*i);
        throw std::runtime_error((QString::fromUtf8(e.what())+(failed.isEmpty() ? "; выполнен откат записанных параметров" : "; НЕПОЛНЫЙ ОТКАТ: "+failed.join(", "))).toStdString());
    }
}
} // namespace n14