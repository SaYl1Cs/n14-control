#pragma once
#include "core.h"
#include "hardware.h"
#include <QObject>
#include <QTimer>

namespace n14 {
class FanController final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.n14control.Fans1")
public:
    explicit FanController(QString configPath,QObject *parent=nullptr);
public slots:
    QString Status() const;
    QString SetMode(bool manual,int duty);
    QString SetKeylight(int percent);
signals:
    void StateChanged(const QString &json);
private slots:
    void prepareForSleep(bool sleeping);
private:
    void tick();
    void dispatchKeylight();
    void loadConfiguration();
    void fail(const QString &reason);
    void publish();
    QString reject(const QString &reason) const;
    QString configPath_,reason_,waitingReason_;
    Settings settings_;
    reference::CoolerControlGraphs graphs_;
    QByteArray coolingConfig_;
    HardwareBackend backend_{false,this};
    QTimer timer_;
    QJsonObject status_;
    bool started_=false,failed_=false,sleeping_=false,manual_=false,pending_=false;
    bool curveDue_=false;
    bool waitingForSample_=false;
    std::optional<int> queuedKeylight_,activeKeylight_,observingKeylight_,sleepKeylight_;
    quint64 cycles_=0,commands_=0;
};
}