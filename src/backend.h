#pragma once
#include "core.h"
#include <QObject>
#include <QDBusServiceWatcher>
#include <QProcess>
#include <QTimer>

namespace n14 {
class DesktopBackend : public QObject {
    Q_OBJECT
public:
    explicit DesktopBackend(bool demo, QObject *parent=nullptr);
    QString activeProfile() const { return profile_; }
    bool profileMatches(const QString &profile) const { return profile_==profile && !profileDrifted_; }
    bool profileBusy() const { return profileBusy_; }
    bool powerAvailable() const { return !service_.isEmpty(); }
    bool demo() const { return demo_; }
    bool screenBusy() const { return screen_.state()!=QProcess::NotRunning; }
    const std::optional<Panel> &panel() const { return panel_; }
    void setProfile(const QString &profile);
    void setHz(int hz);
    void refreshPower();
    void refreshPanel();
signals:
    void profileFinished(bool success);
    void frequencyFinished(int hz,bool success);
    void profileChanged();
    void externalProfileChanged();
    void panelChanged();
    void notice(const QString &message, bool error);
private slots:
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);
private:
    void readPanel(bool verify=false);
    void screenFinished(int code, QProcess::ExitStatus status);
    void applyPendingHz();
    void finishHz(bool success);
    bool demo_;
    bool profileBusy_=false;
    QString profile_, systemProfile_, service_, path_, interface_;
    bool profileDrifted_=false;
    QDBusServiceWatcher *watcher_;
    QProcess screen_;
    QTimer timeout_;
    enum class ScreenTask { Probe, Apply, Verify } task_=ScreenTask::Probe;
    std::optional<int> pendingHz_;
    std::optional<Panel> panel_;
};
} // namespace n14