// src/citra_qt/ds_forwarder_registry.cpp

#include "citra_qt/ds_forwarder_registry.h"

#include <QFile>
#include <QTextStream>

#include "common/file_util.h"

namespace DSForwarderRegistry {

namespace {

// One line per forwarder: "<16 lowercase hex program_id>=<absolute DS
// ROM path>". Written by tools/make_ds_forwarder.py, one line appended
// per forwarder generated — plain text rather than JSON so this side
// doesn't need a JSON dependency just to read a flat key/value map.
QString RegistryPath() {
    return QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) +
           QStringLiteral("ds_forwarders.txt");
}

} // namespace

QString ResolveForwarder(uint64_t program_id) {
    QFile file(RegistryPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString(); // No forwarders registered yet — the common case.
    }

    const QString needle = QStringLiteral("%1=").arg(program_id, 16, 16, QLatin1Char('0'));

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.startsWith(needle, Qt::CaseInsensitive)) {
            return line.mid(needle.length()).trimmed();
        }
    }

    return QString();
}

} // namespace DSForwarderRegistry
