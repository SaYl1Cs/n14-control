#include "fan_client.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonDocument>
#include <QTimer>

namespace n14 {
FanClient::FanClient(bool demo,QObject *parent) : QObject(parent),demo_(demo),watcher_(this) {
    keylightTimer_.setSingleShot(true); keylightTimer_.setInterval(120);
    connect(&keylightTimer_,&QTimer::timeout,this,[this] {
        if (!keylightRequest_ || keylightCallPending_) return;
        const int percent=*keylightRequest_; keylightRequest_.reset();
        if (demo_) emit notice(QString("Демо: подсветка %1% изменена только в симуляции").arg(percent),false);
        else call("SetKeylight",{percent});
    });
    if (demo_) {
        state_={{"schema",1},{"mode","curve"},{"failed",false},{"manual",false},{"status",QJsonObject{}}};
        return;
    }
    auto bus=QDBusConnection::systemBus();
    watcher_.setConnection(bus); watcher_.setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    watcher_.addWatchedService(FanService);
    connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this] {
        keylightTimer_.stop(); keylightRequest_.reset(); keylightCallPending_=false;
        ++revision_; state_={}; emit changed(); refresh();
    });
    bus.connect(FanService,FanPath,FanInterface,"StateChanged",this,SLOT(acceptState(QString)));
    QTimer::singleShot(0,this,&FanClient::refresh);
}
void FanClient::acceptState(const QString &json) {
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(json.toUtf8(),&error);
    if (error.error!=QJsonParseError::NoError || !document.isObject() || document.object()["schema"]!=1) return;
    state_=document.object();
    if (state_["failed"].toBool() || state_["sleeping"].toBool()) {
        keylightTimer_.stop(); keylightRequest_.reset();
    }
    emit changed();
}
void FanClient::call(const QString &method,const QVariantList &arguments) {
    if (method=="SetKeylight") keylightCallPending_=true;
    auto message=QDBusMessage::createMethodCall(FanService,FanPath,FanInterface,method);
    message.setArguments(arguments);
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,3000),this);
    const auto revision=revision_;
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,revision,method] {
        if (revision!=revision_) { watcher->deleteLater(); return; }
        if (method=="SetKeylight") keylightCallPending_=false;
        const QDBusPendingReply<QString> reply=*watcher;
        if (reply.isError()) {
            if (method=="SetKeylight") { keylightTimer_.stop(); keylightRequest_.reset(); }
            if (method=="Status") { state_={}; emit changed(); }
            else { emit notice("Fan controller: "+reply.error().message(),true); emit changed(); }
        } else {
            const auto object=QJsonDocument::fromJson(reply.value().toUtf8()).object();
            if (object.contains("request_error")) {
                if (method=="SetKeylight") { keylightTimer_.stop(); keylightRequest_.reset(); }
                emit notice(object["request_error"].toString(),true);
            }
            acceptState(reply.value());
        }
        if (method=="SetKeylight" && keylightRequest_ && !keylightTimer_.isActive()) keylightTimer_.start();
        watcher->deleteLater();
    });
}
void FanClient::refresh() { if (demo_) emit changed(); else call("Status"); }
void FanClient::setMode(bool manual,int duty) {
    if (demo_) {
        state_["manual"]=manual; state_["mode"]=manual ? "manual" : "curve"; emit changed(); return;
    }
    call("SetMode",{manual,duty});
}
void FanClient::setKeylight(int percent) {
    if (percent<0 || percent>100) { emit notice("Яркость должна быть от 0 до 100%",true); return; }
    keylightRequest_=percent; keylightTimer_.start();
}
bool FanClient::keylightPending() const {
    return keylightTimer_.isActive() || keylightCallPending_ || state_["keylight_pending_percent"].isDouble();
}
}