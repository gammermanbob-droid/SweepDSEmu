// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import androidx.preference.PreferenceManager
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.features.settings.model.Settings
import org.citra.citra_emu.model.CheapDocument
import org.citra.citra_emu.model.Game
import org.citra.citra_emu.model.GameInfo

object GameHelper {
    const val KEY_GAME_PATH = "game_path"
    const val KEY_GAMES = "Games"

    // Folder names, wherever they occur, whose entire contents are internal
    // MoonShell 2 tooling rather than user-facing games -- see the doc
    // comment at this set's one use site in addGamesRecursive.
    private val dsHomebrewInternalFolderNames = setOf("launch", "extlink", "resetmse", "misctools")

    // Since the game val in GetGame is tied to the JNI we need to cache the game list in order to use it elsewhere
    var cachedGameList = mutableListOf<Game>()
    private lateinit var preferences: SharedPreferences

    fun getGames(): List<Game> {
        val games = mutableListOf<Game>()
        val context = CitraApplication.appContext
        preferences = PreferenceManager.getDefaultSharedPreferences(context)
        val gamesDir = preferences.getString(KEY_GAME_PATH, "")
        val gamesUri = Uri.parse(gamesDir)

        addGamesRecursive(games, FileUtil.listFiles(gamesUri), 3)
        NativeLibrary.getInstalledGamePaths().forEach {
            games.add(
                getGame(Uri.parse(it.path), isInstalled = true, addedToLibrary = true, it.mediaType)
            )
        }

        // Cache list of games found on disk
        val serializedGames = mutableSetOf<String>()
        games.forEach {
            serializedGames.add(Json.encodeToString(it))
        }
        preferences.edit()
            .remove(KEY_GAMES)
            .putStringSet(KEY_GAMES, serializedGames)
            .apply()

        cachedGameList = games.toMutableList()
        return games.toList()
    }

    private fun addGamesRecursive(
        games: MutableList<Game>,
        files: Array<CheapDocument>,
        depth: Int,
        // True only for the initial call, i.e. the game directory's own top
        // level (normally the sdmc root) -- real DS/DSi ROMs live nested
        // under roms/nds, roms/dsi, so a .nds/.dsi file sitting loose right
        // here is a system/loader file (GBARunner2's *dldi*.nds stubs,
        // MoonShell 2's moonshl2_*.nds alt-boot variants, ntrboot.nds, a
        // disk-check utility, ...), not a real game, with the one deliberate
        // exception of BOOT.NDS itself -- TWiLightMenu++'s own loader, which
        // *is* meant to be picked from this list directly (see melon_ds_core.cpp's
        // BuildHomebrewSDCardRoot and this file's own git history for why it
        // has to sit loose at this exact location rather than in a subfolder).
        atRoot: Boolean = true
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                // "nds_sdcard_root" is MelonDSCore's own internal mirror of
                // this same sdmc tree (built for the DS core's virtual SD
                // card, see BuildHomebrewSDCardRoot in melon_ds_core.cpp)
                // -- everything under it is a copy of a real file that
                // already got listed once from its actual location, so
                // recursing into it too just shows every DS ROM/homebrew
                // file twice.
                if (it.filename == "nds_sdcard_root") {
                    return@forEach
                }
                // A leading underscore is the DS-homebrew community's own
                // convention for "internal, not a real game" -- TWiLightMenu++
                // (_nds), GBARunner2 (_gba), and nds-bootstrap's own loader
                // stubs inside _nds (_hn.HugeNDSLoader.nds, _te.TextEdit.nds,
                // _vh.VeryHugeNDSLoader.nds, etc.) all use it, and every real
                // flashcart menu/homebrew launcher hides them accordingly.
                // Recursing into _nds specifically is also how those loader
                // stubs ended up individually listed here as if they were
                // games in the first place: _nds only exists at the real sdmc
                // root (rather than solely inside the internal mirror) since
                // TWiLightMenu++ needs it there to actually load, but nothing
                // inside it is meant to be picked from this list directly.
                if (it.filename.startsWith("_")) {
                    return@forEach
                }
                // MoonShell 2's own bundled utility apps (launch/: Check
                // Disk, Image Viewer, Language Select, Morning Timer, Voice
                // Recorder), extension-loader plugins (extlink/: ipk.nds
                // archive viewer, nes.nesterds.nds NES plugin, plus the
                // underscore-prefixed loader stubs already caught above),
                // flashcart-specific reset stubs (resetmse/), and alternate-
                // version loader binaries (misctools/MoonShell2_*/*/moonshl2alt.nds)
                // -- all internal to MoonShell2 itself, never meant to be
                // launched as standalone games, regardless of which of
                // moonshl2/'s two locations (the sdmc-root alias or
                // roms/nds/moonshl2) they're reached through. moonshl2.nds
                // itself (MoonShell2's own real launcher) is a deliberate
                // exception -- not caught by any of these names.
                if (it.filename in dsHomebrewInternalFolderNames) {
                    return@forEach
                }
                addGamesRecursive(games, FileUtil.listFiles(it.uri), depth - 1, atRoot = false)
            } else {
                // Same "internal, not a real game" convention as above --
                // covers any loader stub that happens to sit loose rather
                // than inside an underscore-prefixed folder.
                if (it.filename.startsWith("_")) {
                    return@forEach
                }
                val extension = FileUtil.getExtension(it.uri)
                if (Game.dsExtensions.contains(extension)) {
                    if (preferences.getBoolean(Settings.KEY_DS_HIDE_FROM_GAME_LIST, false)) {
                        return@forEach
                    }
                    // A loose DS/DSi file at the real sdmc root is a
                    // system/loader file, not a game -- except BOOT.NDS
                    // itself (see this function's atRoot doc comment).
                    if (atRoot && !it.filename.equals("BOOT.NDS", ignoreCase = true)) {
                        return@forEach
                    }
                }
                if (Game.allExtensions.contains(extension)) {
                    games.add(
                        getGame(
                            it.uri,
                            isInstalled = false,
                            addedToLibrary = true,
                            Game.MediaType.GAME_CARD
                        )
                    )
                }
            }
        }
    }

    fun getGame(
        uri: Uri,
        isInstalled: Boolean,
        addedToLibrary: Boolean,
        mediaType: Game.MediaType
    ): Game {
        val filePath = uri.toString()
        var nativePath: String? = null
        var gameInfo: GameInfo?
        if (BuildUtil.isGooglePlayBuild || FileUtil.isNativePath(filePath) ||
            filePath.startsWith("!")
        ) {
            gameInfo = GameInfo(filePath)
        } else {
            nativePath = if (uri.scheme == "fd") {
                uri.toString()
            } else {
                "!" + NativeLibrary.getNativePath(uri)
            }
            gameInfo = GameInfo(nativePath)
        }

        val valid = gameInfo.isValid()
        if (!valid) {
            gameInfo = null
        }

        val isEncrypted = gameInfo?.isEncrypted() == true

        val newGame = Game(
            valid,
            (
                gameInfo?.getTitle() ?: FileUtil.getFilename(
                    uri
                )
                ).replace("[\\t\\n\\r]+".toRegex(), " "),
            filePath.replace("\n", " "),
            // TODO: This next line can be deduplicated but I don't want to right now -OS
            if (BuildUtil.isGooglePlayBuild || FileUtil.isNativePath(filePath) ||
                filePath.startsWith("!")
            ) {
                filePath
            } else {
                nativePath!!
            },
            gameInfo?.getTitleID() ?: 0,
            mediaType,
            gameInfo?.getCompany() ?: "",
            if (isEncrypted) {
                CitraApplication.appContext.getString(R.string.unsupported_encrypted)
            } else {
                gameInfo?.getRegions()
                    ?: ""
            },
            isInstalled,
            gameInfo?.isSystemTitle() ?: false,
            gameInfo?.getIsVisibleSystemTitle() ?: false,
            gameInfo?.getIsInsertable() ?: false,
            gameInfo?.getIcon(),
            gameInfo?.getFileType() ?: "",
            gameInfo?.getFileType()?.contains("(Z)") ?: false,
            if (FileUtil.isNativePath(filePath)) {
                CitraApplication.documentsTree.getFilename(filePath)
            } else {
                FileUtil.getFilename(Uri.parse(filePath))
            }
        )

        if (addedToLibrary) {
            val addedTime = preferences.getLong(newGame.keyAddedToLibraryTime, 0L)
            if (addedTime == 0L) {
                preferences.edit()
                    .putLong(newGame.keyAddedToLibraryTime, System.currentTimeMillis())
                    .apply()
            }
        }

        return newGame
    }
}
