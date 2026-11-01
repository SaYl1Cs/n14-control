#include "fan_controller.h"
#include "fan_client.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QFileInfo>
#include <QTextStream>
#include <exception>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("n14-fan-controller");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"config","Absolute existing N14 settings JSON path","path"});
    parser.addOption({"check-config","Validate configuration without connecting to hardware"});
    parser.process(app);
    const QString config=parser.value("config");
    if (!QFileInfo(config).isAbsolute() || !QFileInfo(config).isFile()) {
        QTextStream(stderr)<<"An absolute existing --config file is required"<<Qt::endl; return 2;
    }
    try {
        const auto settings=n14::Settings::load(config);
        if (settings.mix!="Max") { QTextStream(stderr)<<"Live Mix must be Max"<<Qt::endl; return 2; }
        if (parser.isSet("check-config")) return 0;
        auto bus=QDBusConnection::systemBus();
        // Reserve the unique system-bus frontend name BEFORE creating a
        // hardware client.  The controller is deliberately independent of a
        // graphical/user login: it owns no EC transport and only calls the
        // root hardware broker over D-Bus.
        if (!bus.registerService(n14::FanService)) {
            QTextStream(stderr)<<"Fan controller already running or session bus unavailable"<<Qt::endl; return 2;
        }
        n14::FanController controller(config);
        if (!bus.registerObject(n14::FanPath,&controller,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals)) return 2;
        return app.exec();
    } catch (const std::exception &error) {
        QTextStream(stderr)<<error.what()<<Qt::endl; return 2;
    }
}