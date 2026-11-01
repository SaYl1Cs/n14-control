#pragma once
#include <QObject>
#include <QJsonObject>
#include <QDBusServiceWatcher>
#include <QTimer>
#include <optional>

namespace n14 {
inline constexpr auto FanService="org.n14control.Fans1";
inline constexpr auto FanPath="/org/n14control/Fans1";
inline constexpr auto FanInterface="org.n14control.Fans1";

// GUI-only system-bus facade. No EC provenance, lock or direct hardware calls.
class FanClient final : public QObject {
    Q_OBJECT
public:
    explicit FanClient(bool demo,QObject *parent=nullptr);
    void refresh();
    void setMode(bool manual,int duty=0);
    void setKeylight(int percent);
    bool keylightPending() const;
    const QJsonObject &state() const { return state_; }
signals:
    void changed();
    void notice(const QString &message,bool error);
private slots:
    void acceptState(const QString &json);
private:
    void call(const QString &method,const QVariantList &arguments={});
    bool demo_;
    QJsonObject state_;
    QDBusServiceWatcher watcher_;
    QTimer keylightTimer_{this};
    std::optional<int> keylightRequest_;
    bool keylightCallPending_=false;
    quint64 revision_=0;
};
}