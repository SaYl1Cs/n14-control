#pragma once

#include <QByteArray>

namespace n14::reference {

// Qt's JSON reader keeps the last value for duplicate object members. The
// pinned Python producer rejects duplicates, so callers must run this check
// before handing untrusted JSON to QJsonDocument.
bool hasDuplicateJsonMembers(const QByteArray &json);

} // namespace n14::reference