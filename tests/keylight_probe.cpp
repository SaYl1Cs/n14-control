#include "hardware.h"
#include "reference_control.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

#include <cmath>
#include <exception>
#include <utility>

using namespace n14;

namespace {
class KeylightProbe final : public QObject {
public:
    KeylightProbe(QCoreApplication &app,QString reportPath,int percent)
        : QObject(&app),app_(app),reportPath_(std::move(reportPath)),percent_(percent),backend_(false,this) {
        try { expectedRaw_=reference::keylightPercentToRaw(percent_); }
        catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
        connect(&backend_,&HardwareBackend::changed,this,[this] {
            if (phase_!=Phase::Capabilities || !backend_.available()) return;
            if (!backend_.ecAvailable() || backend_.capabilities()["ec_protocol"].toInt()!=2) {
                fail("EC protocol 2 is required"); return;
            }
            phase_=Phase::Initial; backend_.refreshEcStatus();
        });
        connect(&backend_,&HardwareBackend::ecStatusRawChanged,this,
                [this](int mode,int cpu,int gpu,int light,int tach) {
            status_={{"fan_mode",mode},{"cpu_target_raw",cpu},{"gpu_target_raw",gpu},
                     {"keylight_raw",light},{"cpu_tach_raw",tach}};
        });
        connect(&backend_,&HardwareBackend::notice,this,
                [this](const QString &message,bool error) {
            notices_.append(QJsonObject{{"message",message},{"error",error}});
            QTextStream(stderr)<<"keylight-probe: "<<message<<Qt::endl;
        });
        connect(&backend_,&HardwareBackend::ecStatusFailed,this,
                [this](const QString &reason) { fail("observation failed; no retry: "+reason); });
        connect(&backend_,&HardwareBackend::ecFinished,this,[this](bool ok,const QString &target) {
            if (phase_!=Phase::Applying) return;
            if (!ok || target!="keylight") {
                fail("keylight transaction failed; no retry or compensation"); return;
            }
            ++commands_;
            phase_=Phase::Final;
            // HardwareBackend schedules one normal post-command observation.
        });
        connect(&backend_,&HardwareBackend::ecStatusReady,this,[this] {
            if (phase_==Phase::Initial) {
                initial_=status_;
                if (initial_.isEmpty() || initial_["fan_mode"].toInt()!=2) {
                    fail("initial manual mode 2 is required"); return;
                }
                phase_=Phase::Applying;
                backend_.applyEcTarget("keylight",percent_);
            } else if (phase_==Phase::Final) {
                final_=status_;
                if (final_["keylight_raw"].toInt(-1)!=expectedRaw_) {
                    fail(QString("keylight readback mismatch: expected raw %1, got %2")
                         .arg(expectedRaw_).arg(final_["keylight_raw"].toInt(-1))); return;
                }
                finish(true,{});
            }
        });
        timeout_.setSingleShot(true);
        connect(&timeout_,&QTimer::timeout,this,[this] { fail("keylight probe timed out"); });
        timeout_.start(90000);
        QTimer::singleShot(0,this,&KeylightProbe::start);
    }

private:
    enum class Phase { Capabilities, Initial, Applying, Final, Finished };

    void start() { backend_.refresh(); }

    void fail(const QString &reason) {
        if (phase_==Phase::Finished) return;
        errors_.append(reason); finish(false,reason);
    }

    void finish(bool ok,const QString &reason) {
        if (phase_==Phase::Finished) return;
        phase_=Phase::Finished; timeout_.stop();
        QJsonObject report;
        report["schema"]=1;
        report["test"]="single physical keyboard-light EC transaction";
        report["ok"]=ok && errors_.isEmpty();
        report["errors"]=errors_; report["notices"]=notices_;
        report["requested_percent"]=percent_; report["expected_raw"]=expectedRaw_;
        report["initial_status"]=initial_; report["final_status"]=final_;
        report["confirmed_commands"]=commands_; report["automatic_retries"]=0;
        report["recovery_writes"]=0; report["reason"]=reason;
        report["client_bus_name"]=QDBusConnection::systemBus().baseService();
        QSaveFile file(reportPath_); const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) {
            QTextStream(stderr)<<"keylight-probe: cannot write report"<<Qt::endl; app_.exit(1); return;
        }
        app_.exit(ok && errors_.isEmpty() ? 0 : 1);
    }

    QCoreApplication &app_;
    QString reportPath_;
    int percent_=0,expectedRaw_=-1,commands_=0;
    HardwareBackend backend_;
    QTimer timeout_;
    QJsonObject status_,initial_,final_;
    QJsonArray errors_,notices_;
    Phase phase_=Phase::Capabilities;
};
}

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments().size()!=4 || app.arguments()[1]!="--physical") {
        QTextStream(stderr)<<"Usage: n14-keylight-probe --physical PERCENT ABSOLUTE_REPORT_PATH"<<Qt::endl;
        return 2;
    }
    bool ok=false; const int percent=app.arguments()[2].toInt(&ok);
    const QString report=app.arguments()[3];
    if (!ok || percent<0 || percent>100 || !QFileInfo(report).isAbsolute() || QFileInfo::exists(report) ||
        !QFileInfo(report).dir().exists()) return 2;
    KeylightProbe probe(app,report,percent);
    return app.exec();
}