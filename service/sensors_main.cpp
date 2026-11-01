#include "sensor_sampler.h"

#include <QCoreApplication>
#include <QTextStream>

#include <csignal>

namespace {
std::atomic_bool stopRequested=false;
void stop(int) { stopRequested.store(true); }
}

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments().size()!=1 || ::geteuid()!=0) {
        QTextStream(stderr)<<"Run through the installed root sensor service.\n";
        return 1;
    }
    std::signal(SIGTERM,stop); std::signal(SIGINT,stop);
    n14::sensor::NativeSensorSampler sampler;
    QString reason;
    const int result=sampler.run(stopRequested,1000,&reason);
    if (result!=0 && !reason.isEmpty()) QTextStream(stderr)<<reason<<'\n';
    return result;
}