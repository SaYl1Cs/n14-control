#include "reference_power.h"

#include <QDir>
#include <QFile>

namespace n14::reference {
namespace {

QByteArray readSmall(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data=file.read(129);
    return data.size()>128 ? QByteArray() : data;
}

void reason(QString *out,const QString &value) {
    if (out) *out=value;
}

} // namespace

std::optional<QPair<QString,QString>> PosixGpuPowerStateSource::read(QString *failure) const {
    QStringList devices;
    const QDir root(pciRoot_);
    // /sys/bus/pci/devices entries are symlinks into /sys/devices.  They must
    // be included when discovering the pinned NVIDIA display function.
    for (const auto &name : root.entryList(QDir::AllEntries|QDir::NoDotAndDotDot)) {
        const QString path=root.filePath(name);
        const auto vendor=QString::fromUtf8(readSmall(QDir(path).filePath("vendor"))).trimmed();
        const auto classText=QString::fromUtf8(readSmall(QDir(path).filePath("class"))).trimmed();
        bool ok=false;
        const int deviceClass=classText.toInt(&ok,16);
        if (vendor=="0x10de" && ok && (deviceClass>>16)==3) devices.append(path);
    }
    if (devices.size()!=1) {
        reason(failure,"expected exactly one NVIDIA display PCI device"); return {};
    }
    const auto readState=[&] {
        return qMakePair(QString::fromUtf8(readSmall(QDir(devices.first()).filePath("power/runtime_status"))).trimmed(),
                         QString::fromUtf8(readSmall(QDir(devices.first()).filePath("power_state"))).trimmed());
    };
    const auto first=readState();
    if (first.first.isEmpty() || first.second.isEmpty() || readState()!=first) {
        reason(failure,"GPU power state changed or is unavailable during observation"); return {};
    }
    if (first!=qMakePair(QStringLiteral("active"),QStringLiteral("D0")) &&
        first!=qMakePair(QStringLiteral("suspended"),QStringLiteral("D3cold"))) {
        reason(failure,"GPU power state is unconfirmed"); return {};
    }
    return first;
}

} // namespace n14::reference