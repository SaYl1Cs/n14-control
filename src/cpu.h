#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace n14 {
struct CpuRequest {
    QString mode = "keep"; // keep, limit, hardware (explicitly remove the cap)
    int maxMHz = 2400;
    int turbo = -1; // -1 keep, 0 disable, 1 allow; never guarantees actual boost
    bool changesHardware() const { return mode!="keep" || turbo!=-1; }
    QJsonObject toJson() const;
    static CpuRequest fromJson(const QJsonObject &json);
};
struct CpuPolicy {
    QString name, driver;
    int hardwareMin = 0, hardwareMax = 0, currentMin = 0, currentMax = 0;
    QJsonObject toJson() const;
};
class CpuController {
public:
    using Writer = std::function<void(const QString &,const QByteArray &)>;
    explicit CpuController(QString sysRoot="/sys",Writer writer={});
    QVector<CpuPolicy> policies() const;
    QJsonObject inspect() const;
    QJsonObject apply(const CpuRequest &request);
private:
    QString root_;
    Writer writer_;
};
} // namespace n14