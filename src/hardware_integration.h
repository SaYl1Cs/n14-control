#pragma once

class QCoreApplication;
class QString;

namespace n14 {

// Runs the real application-side D-Bus fan/keylight workflow. It is deliberately
// opt-in and never part of the normal test suite because it performs EC writes.
int runHardwareIntegrationTest(QCoreApplication &application, const QString &reportPath);

} // namespace n14