// src/citra_qt/ds_forwarder_registry.cpp

#include "citra_qt/ds_forwarder_registry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "common/file_util.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"

namespace DSForwarderRegistry {

namespace {

// One line per forwarder: "<16 lowercase hex program_id>=<DS ROM path,
// relative to UserDir when the ROM lives inside it, absolute
// otherwise>". Written by tools/make_ds_forwarder.py, one line appended
// per forwarder generated — plain text rather than JSON so this side
// doesn't need a JSON dependency just to read a flat key/value map.
//
// Storing a UserDir-relative path (rather than always absolute) is
// what makes a forwarder built on one machine/profile still resolve
// correctly once that whole profile directory is copied elsewhere —
// e.g. the desktop-built profile tree that gets copied onto an Android
// device's storage: ROMs there always live at the same path relative
// to UserDir regardless of platform (sdmc/roms/nds/...), even though
// UserDir's own absolute location differs completely between a Mac
// and an Android device. A ROM path outside UserDir entirely (rare,
// but possible if someone points the builder at an arbitrary folder)
// still falls back to an absolute path, which is the best that can be
// done for it.
QString UserDir() {
    return QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir));
}

QString RegistryPath() {
    return UserDir() + QStringLiteral("ds_forwarders.txt");
}

// Converts an absolute rom_path into the form that should be written to
// the registry: relative to UserDir if rom_path is inside it, else left
// absolute.
QString ToStoredPath(const QString& rom_path) {
    const QDir user_dir(UserDir());
    const QString relative = user_dir.relativeFilePath(rom_path);
    if (relative.startsWith(QStringLiteral(".."))) {
        return rom_path;
    }
    return relative;
}

// Inverse of ToStoredPath: resolves a registry value (relative or
// absolute) back into a path usable directly by the loader.
QString FromStoredPath(const QString& stored) {
    if (QFileInfo(stored).isAbsolute()) {
        return stored;
    }
    return QDir::cleanPath(UserDir() + QLatin1Char('/') + stored);
}

// Same directory Service::AM's own (file-local) GetTicketPath() builds
// filenames under — not exposed publicly since callers there always
// already know the ticket_id half, which nothing on this side ever
// records. A prefix scan is the only way in from just a program_id.
QString TicketDbDir() {
    return QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir)) +
           QStringLiteral("dbs/ticket.db/");
}

QString ProgramIdHex(uint64_t program_id) {
    return QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0'));
}

} // namespace

QString ResolveForwarder(uint64_t program_id) {
    QFile file(RegistryPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString(); // No forwarders registered yet — the common case.
    }

    const QString needle = ProgramIdHex(program_id) + QStringLiteral("=");

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.startsWith(needle, Qt::CaseInsensitive)) {
            return FromStoredPath(line.mid(needle.length()).trimmed());
        }
    }

    return QString();
}

QList<Forwarder> ListForwarders() {
    QList<Forwarder> forwarders;

    QFile file(RegistryPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return forwarders;
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            continue; // Blank or malformed line — skip rather than fail the whole list.
        }
        bool ok = false;
        const uint64_t program_id = line.left(equals).toULongLong(&ok, 16);
        if (!ok) {
            continue;
        }
        forwarders.append(Forwarder{program_id, FromStoredPath(line.mid(equals + 1))});
    }

    return forwarders;
}

void RegisterForwarder(uint64_t program_id, const QString& rom_path) {
    const QString needle = ProgramIdHex(program_id) + QStringLiteral("=");

    QStringList lines;
    QFile file(RegistryPath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (!line.startsWith(needle, Qt::CaseInsensitive)) {
                lines.append(line);
            }
        }
        file.close();
    }
    lines.append(needle + ToStoredPath(rom_path));

    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream out(&file);
        for (const QString& line : lines) {
            out << line << '\n';
        }
    }
}

bool RemoveForwarder(uint64_t program_id) {
    const QString needle = ProgramIdHex(program_id) + QStringLiteral("=");

    QFile file(RegistryPath());
    QStringList remaining_lines;
    bool found = false;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith(needle, Qt::CaseInsensitive)) {
                found = true;
                continue;
            }
            remaining_lines.append(line);
        }
        file.close();
    }

    // Best-effort from here regardless of whether the registry line was
    // found — a forwarder can end up installed-but-unregistered (or vice
    // versa) if something upstream was interrupted, and cleanup should
    // still get as much of it as it can rather than bailing early.
    const QString content_dir = QString::fromStdString(
        Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, program_id));
    QDir(content_dir).removeRecursively();

    const QDir ticket_dir(TicketDbDir());
    const QString ticket_prefix = ProgramIdHex(program_id);
    for (const QString& name :
         ticket_dir.entryList(QStringList{ticket_prefix + QStringLiteral(".*.tik")},
                              QDir::Files, QDir::NoSort)) {
        QFile::remove(ticket_dir.filePath(name));
    }
    // QDir::entryList's name filters are case-sensitive on some platforms
    // but registered program IDs are always written lowercase
    // (ProgramIdHex) while installed ticket filenames are uppercase hex
    // (matching Service::AM's own %08X-style formatting) — glob both
    // cases explicitly rather than relying on filesystem case-folding,
    // which differs between macOS/Windows (case-insensitive) and Linux
    // (case-sensitive).
    for (const QString& name :
         ticket_dir.entryList(QStringList{ticket_prefix.toUpper() + QStringLiteral(".*.tik")},
                              QDir::Files, QDir::NoSort)) {
        QFile::remove(ticket_dir.filePath(name));
    }

    if (found && file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream out(&file);
        for (const QString& line : remaining_lines) {
            out << line << '\n';
        }
    }

    return found;
}

} // namespace DSForwarderRegistry
