// src/citra_qt/ds_forwarder_registry.h
//
// A DS "forwarder" is a real, installable 3DS CIA (built by
// tools/make_ds_forwarder.py using bannertool/makerom/devkitARM) whose
// only purpose is to appear as a normal icon on the emulated 3DS HOME
// Menu. It carries the target DS ROM's own icon/title so it looks
// exactly like a real forwarder would on modded hardware. Its actual
// 3DS code does nothing meaningful — when the HOME Menu launches it,
// GMainWindow::BootGame() checks this registry *before* running it for
// real, and redirects straight to BootDSGame() with the associated
// ROM instead. Nothing about this needs the forwarder's own code to do
// anything special, since Azahar controls the interception itself.
//
// The registry is a simple, hand-written JSON file the generator
// script writes to (and this reads from) — deliberately not part of
// Azahar's own Settings::values, since it's project metadata (which
// title ID maps to which ROM) rather than a user preference.

#pragma once

#include <QList>
#include <QString>

namespace DSForwarderRegistry {

// program_id as produced by Loader::AppLoader::ReadProgramId() for the
// CIA about to be booted. Returns the absolute path to the DS ROM it
// forwards to, or an empty string if program_id isn't a known
// forwarder (the overwhelmingly common case — this is checked on
// every boot, so it has to be cheap and safe to call unconditionally).
QString ResolveForwarder(uint64_t program_id);

struct Forwarder {
    uint64_t program_id;
    QString rom_path;
};

// Every registered forwarder, in file order. Used by SweepDSEmuNDSBrewer's
// "manage existing forwarders" list — unlike ResolveForwarder(), which is
// on the hot boot-time path, this is only called when that UI is open.
QList<Forwarder> ListForwarders();

// Registers (or re-registers, replacing any stale entry for the same
// program_id) a freshly-built forwarder. Mirrors
// tools/make_ds_forwarder.py's own register_forwarder() so both stay
// interchangeable against the same registry file.
void RegisterForwarder(uint64_t program_id, const QString& rom_path);

// Un-registers a forwarder and undoes everything CIA installation did for
// it: the SD title's own content directory (via
// Service::AM::GetTitlePath(SDMC, program_id)) and its ticket file under
// NANDDir/dbs/ticket.db/ (matched by the program_id prefix all ticket
// filenames start with — the trailing ticket_id half of the filename isn't
// recorded anywhere else, so a prefix match is how the existing
// Service::AM ticket-path helpers locate one too), plus the registry line
// itself. Returns false if program_id wasn't registered; a forwarder whose
// CIA was never actually installed (built but not installed, or installed
// then already removed some other way) still has its registry line
// dropped either way, since that's this function's one unconditional job.
bool RemoveForwarder(uint64_t program_id);

} // namespace DSForwarderRegistry
