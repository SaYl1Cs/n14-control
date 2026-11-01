#include "service.h"
#include "hardware.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QJsonDocument>
#include <QTextStream>
#include <unistd.h>
#include <utility>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments()==QStringList{app.arguments().first(),"--probe"}) {
        HardwareService service; QTextStream(stdout)<<service.Capabilities()<<'\n'; return 0;
    }
    if (app.arguments().size()!=1 || geteuid()!=0) {
        QTextStream(stderr)<<"Run through the installed system service; --probe is read-only and unprivileged.\n"; return 1;
    }
    HardwareService::Options options;
    options.ecLiveGpuPowerState=[] {
        return n14::reference::PosixGpuPowerStateSource{}.read();
    };
    auto bus=QDBusConnection::systemBus();
    HardwareService service(n14::CpuController(),nullptr,std::move(options));
    if (!bus.registerObject(n14::HardwarePath,&service,QDBusConnection::ExportAllSlots)
        || !bus.registerService(n14::HardwareService)) { QTextStream(stderr)<<bus.lastError().message()<<'\n'; return 1; }
    return app.exec();
}