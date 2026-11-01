#include "backend.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

namespace n14 {
DesktopBackend::DesktopBackend(bool demo, QObject *parent) : QObject(parent),demo_(demo) {
    watcher_=new QDBusServiceWatcher(this);
    watcher_->setConnection(QDBusConnection::systemBus());
    watcher_->setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    watcher_->addWatchedService("net.hadess.PowerProfiles");
    watcher_->addWatchedService("org.freedesktop.UPower.PowerProfiles");
    connect(watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,&DesktopBackend::refreshPower);
    timeout_.setSingleShot(true); timeout_.setInterval(5000);
    connect(&timeout_,&QTimer::timeout,this,[this]{
        screen_.kill();
        emit notice("KScreen не ответил за 5 секунд",true);
    });
    connect(&screen_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&DesktopBackend::screenFinished);
    connect(&screen_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if (error==QProcess::FailedToStart) {
            timeout_.stop(); finishHz(false);
            emit notice("Не удалось запустить /usr/bin/kscreen-doctor",true);
        }
    });
    if (demo_) {
        profile_="balanced";
        systemProfile_=profile_;
        panel_=Panel{1,"eDP-1","1",{{"1",2560,1600,165},{"2",2560,1600,60}}};
    } else {
        QTimer::singleShot(0,this,&DesktopBackend::refreshPower);
        QTimer::singleShot(0,this,&DesktopBackend::refreshPanel);
    }
}
void DesktopBackend::refreshPower() {
    if (demo_) return;
    if (!service_.isEmpty()) QDBusConnection::systemBus().disconnect(service_,path_,"org.freedesktop.DBus.Properties",
        "PropertiesChanged",this,SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    service_.clear(); systemProfile_.clear();
    auto *bus=QDBusConnection::systemBus().interface();
    if (!bus) { emit profileChanged(); return; }
    for (const auto &name : {QString("org.freedesktop.UPower.PowerProfiles"),QString("net.hadess.PowerProfiles")}) {
        const QDBusReply<bool> registered=bus->isServiceRegistered(name);
        if (registered.isValid() && registered.value()) {
            service_=name;
            path_=name=="net.hadess.PowerProfiles" ? "/net/hadess/PowerProfiles" : "/org/freedesktop/UPower/PowerProfiles";
            interface_=name;
            break;
        }
    }
    if (service_.isEmpty()) { emit notice("Системный power-profiles-daemon недоступен",true); emit profileChanged(); return; }
    QDBusConnection::systemBus().connect(service_,path_,"org.freedesktop.DBus.Properties",
        "PropertiesChanged",this,SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    QDBusInterface props(service_,path_,"org.freedesktop.DBus.Properties",QDBusConnection::systemBus());
    props.setTimeout(2000);
    auto *call=new QDBusPendingCallWatcher(props.asyncCall("Get",interface_,"ActiveProfile"),this);
    connect(call,&QDBusPendingCallWatcher::finished,this,[this,call]{
        const QDBusPendingReply<QDBusVariant> reply=*call;
        bool external=false;
        if (!reply.isError()) {
            systemProfile_=reply.value().variant().toString();
            if (profile_.isEmpty()) {
                profile_=systemProfile_;
                profileDrifted_=false;
            } else if (systemProfile_!=profile_) {
                profileDrifted_=true;
                external=true;
                emit notice("Внешнее изменение профиля питания проигнорировано; приложение восстановит свой профиль",true);
            } else profileDrifted_=false;
        } else emit notice("Не удалось прочитать профиль питания: "+reply.error().message(),true);
        call->deleteLater(); emit profileChanged();
        if (external && !profileBusy_) emit externalProfileChanged();
    });
}
void DesktopBackend::propertiesChanged(const QString &iface,const QVariantMap &changed,const QStringList &invalidated) {
    if (iface!=interface_) return;
    if (changed.contains("ActiveProfile")) {
        systemProfile_=changed["ActiveProfile"].toString();
        if (profile_.isEmpty()) profile_=systemProfile_;
        profileDrifted_=systemProfile_!=profile_;
        if (profileDrifted_ && !profileBusy_) {
            emit notice("Внешнее изменение профиля питания проигнорировано; приложение восстановит свой профиль",true);
            emit externalProfileChanged();
        }
        emit profileChanged();
    }
    else if (invalidated.contains("ActiveProfile")) refreshPower();
}
void DesktopBackend::setProfile(const QString &profile) {
    if (!QStringList{"power-saver","balanced","performance"}.contains(profile)) {
        emit notice("Недопустимый профиль питания",true); emit profileFinished(false); return;
    }
    if (demo_) {
        profile_=profile; systemProfile_=profile; profileDrifted_=false;
        emit profileChanged(); emit notice("Демо: профиль "+profileLabel(profile),false); emit profileFinished(true); return;
    }
    if (!powerAvailable() || profileBusy_) { emit notice("Сервис питания недоступен или выполняет запрос",true); emit profileFinished(false); return; }
    profileBusy_=true;
    QDBusInterface props(service_,path_,"org.freedesktop.DBus.Properties",QDBusConnection::systemBus());
    props.setTimeout(3000);
    auto *call=new QDBusPendingCallWatcher(props.asyncCall("Set",interface_,"ActiveProfile",
        QVariant::fromValue(QDBusVariant(profile))),this);
    connect(call,&QDBusPendingCallWatcher::finished,this,[this,call,profile]{
        const QDBusPendingReply<> reply=*call;
        call->deleteLater();
        if (reply.isError()) {
            profileBusy_=false; emit notice("Профиль не изменён: "+reply.error().message(),true); emit profileFinished(false); return;
        }
        QDBusInterface props(service_,path_,"org.freedesktop.DBus.Properties",QDBusConnection::systemBus());
        props.setTimeout(3000);
        auto *verify=new QDBusPendingCallWatcher(props.asyncCall("Get",interface_,"ActiveProfile"),this);
        connect(verify,&QDBusPendingCallWatcher::finished,this,[this,verify,profile]{
            const QDBusPendingReply<QDBusVariant> actual=*verify; profileBusy_=false;
            const bool ok=!actual.isError() && actual.value().variant().toString()==profile;
            if (!actual.isError()) {
                systemProfile_=actual.value().variant().toString();
                if (ok) {
                    profile_=profile;
                    profileDrifted_=false;
                } else profileDrifted_=true;
            } else profileDrifted_=true;
            emit profileChanged();
            emit notice(ok ? "Профиль подтверждён: "+profileLabel(profile) : "Новый профиль не подтверждён сервисом питания",!ok);
            verify->deleteLater(); emit profileFinished(ok);
        });
    });
}
void DesktopBackend::refreshPanel() { if (!demo_ && !screenBusy()) readPanel(); }
void DesktopBackend::readPanel(bool verify) {
    task_=verify ? ScreenTask::Verify : ScreenTask::Probe;
    screen_.setProgram("/usr/bin/kscreen-doctor"); screen_.setArguments({"-j"});
    timeout_.start(); screen_.start();
}
void DesktopBackend::setHz(int hz) {
    if (hz!=60 && hz!=165) { emit notice("Допустимы только 60 и 165 Гц",true); emit frequencyFinished(hz,false); return; }
    if (demo_) {
        panel_->currentMode=hz==60 ? "2" : "1";
        emit panelChanged(); emit notice(QString("Демо: %1 Гц").arg(hz),false); emit frequencyFinished(hz,true); return;
    }
    if (screenBusy()) { emit notice("KScreen занят; повторите после завершения операции",true); emit frequencyFinished(hz,false); return; }
    pendingHz_=hz;
    readPanel(); // Discover fresh IDs and current resolution before every change.
}
void DesktopBackend::applyPendingHz() {
    if (!pendingHz_ || !panel_) return;
    const auto mode=panel_->modeForHz(*pendingHz_);
    if (!mode) { emit notice("Нет нужной частоты для текущего разрешения",true); finishHz(false); return; }
    if (mode->id==panel_->currentMode) {
        emit notice(QString("Экран уже работает на %1 Гц").arg(*pendingHz_),false);
        finishHz(true); return;
    }
    task_=ScreenTask::Apply;
    screen_.setArguments({QString("output.%1.mode.%2").arg(panel_->id).arg(mode->id)});
    timeout_.start(); screen_.start();
}
void DesktopBackend::screenFinished(int code,QProcess::ExitStatus status) {
    timeout_.stop();
    const QByteArray out=screen_.readAllStandardOutput(); screen_.readAllStandardError();
    if (code!=0 || status!=QProcess::NormalExit) {
        emit notice("KScreen завершился с ошибкой; режим не подтверждён",true); finishHz(false); return;
    }
    if (task_==ScreenTask::Apply) { readPanel(true); return; }
    try {
        panel_=parsePanel(out); emit panelChanged();
        if (!panel_) { emit notice("Активная встроенная eDP-панель не найдена",true); finishHz(false); return; }
        if (task_==ScreenTask::Verify && pendingHz_) {
            const auto current=panel_->current();
            const bool ok=current && std::abs(current->hz-*pendingHz_)<0.6;
            emit notice(ok ? QString("Подтверждено: %1 Гц").arg(current->hz,0,'f',2)
                           : "Новая частота не подтверждена KScreen",!ok);
            finishHz(ok);
        } else applyPendingHz();
    } catch (const std::exception &e) {
        emit notice(QString::fromUtf8(e.what()),true); finishHz(false);
    }
}
void DesktopBackend::finishHz(bool success) {
    if (!pendingHz_) return;
    const int hz=*pendingHz_; pendingHz_.reset(); emit frequencyFinished(hz,success);
}
} // namespace n14