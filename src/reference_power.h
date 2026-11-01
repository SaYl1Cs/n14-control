#pragma once

#include <QPair>
#include <QString>

#include <optional>
#include <utility>

namespace n14::reference {

// Read-only runtime-PM observation used immediately before an EC transaction.
// It never opens the EC transport and never changes driver state.
class PosixGpuPowerStateSource {
public:
    explicit PosixGpuPowerStateSource(QString pciRoot = "/sys/bus/pci/devices")
        : pciRoot_(std::move(pciRoot)) {}

    std::optional<QPair<QString,QString>> read(QString *reason = nullptr) const;

private:
    QString pciRoot_;
};

} // namespace n14::reference