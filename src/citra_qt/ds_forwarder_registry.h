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

#include <QString>

namespace DSForwarderRegistry {

// program_id as produced by Loader::AppLoader::ReadProgramId() for the
// CIA about to be booted. Returns the absolute path to the DS ROM it
// forwards to, or an empty string if program_id isn't a known
// forwarder (the overwhelmingly common case — this is checked on
// every boot, so it has to be cheap and safe to call unconditionally).
QString ResolveForwarder(uint64_t program_id);

} // namespace DSForwarderRegistry
