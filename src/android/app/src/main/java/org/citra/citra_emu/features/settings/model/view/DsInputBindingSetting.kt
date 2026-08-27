// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.settings.model.view

import android.content.Context
import android.content.SharedPreferences
import android.view.KeyEvent
import androidx.preference.PreferenceManager
import org.citra.citra_emu.CitraApplication

/**
 * A "press a button to bind" settings row for DS controls -- unlike
 * [InputBindingSetting] (the 3DS equivalent this is modeled on), this
 * stores a plain Android KeyEvent keyCode under its own [key], with no
 * interaction with the 3DS side's shared "InputMapping_HostAxis_*"
 * preference namespace. See that class's own reverseKey/writeButtonMapping
 * machinery for why: a physical key's InputMapping_HostAxis_* entry is a
 * StringSet of native 3DS ButtonType codes shared across every 3DS
 * binding that key participates in, and isn't scoped by which subsystem
 * owns it -- writing a DS button's binding into that same set would
 * inject a bogus ButtonType-cast integer that could fire an unintended
 * 3DS button the next time that physical key is pressed during real 3DS
 * gameplay. DS input is polled independently in Kotlin
 * (DsEmulationActivity.dispatchKeyEvent), so it has no need for that
 * shared table at all.
 */
class DsInputBindingSetting(val key: String, titleId: Int) :
    SettingsItem(null, titleId, 0) {
    private val context: Context get() = CitraApplication.appContext
    private val preferences: SharedPreferences
        get() = PreferenceManager.getDefaultSharedPreferences(context)

    // Always editable: unlike the 3DS Controls screen (which disables
    // rebinding while a 3DS EmulationActivity is running -- irrelevant
    // here since this screen is reached from DsEmulationActivity or the
    // main settings list, never from the 3DS emulation screen itself).
    override val isEditable: Boolean = true

    /** Human-readable text shown in the row, e.g. "Xbox Wireless Controller: RB". */
    val displayValue: String
        get() = preferences.getString(displayKey, "") ?: ""

    val keyCode: Int
        get() = preferences.getInt(key, 0)

    fun onKeyInput(keyEvent: KeyEvent) {
        val code = InputBindingSetting.translateEventToKeyId(keyEvent)
        val deviceName = keyEvent.device?.name ?: "Keyboard"
        preferences.edit()
            .putInt(key, code)
            .putString(displayKey, "$deviceName: ${InputBindingSetting.getButtonName(code)}")
            .apply()
    }

    /**
     * Most gamepads report L2/R2 as continuous analog axes rather than
     * discrete key events (see DsEmulationActivity.dispatchGenericMotionEvent's
     * own doc comment), so they never reach [onKeyInput] at all -- this
     * dialog's key-only capture would otherwise just sit there forever
     * waiting for a key event that never comes. Store the same synthetic
     * KEYCODE_BUTTON_L2/R2 that runtime axis handling already checks for
     * (via keyCodeToDsButton), so a trigger-axis binding actually takes
     * effect through the exact same int-keycode preference [onKeyInput]
     * itself writes -- no separate axis-binding storage format needed.
     */
    fun onAxisTriggerInput(keyCode: Int, deviceName: String) {
        preferences.edit()
            .putInt(key, keyCode)
            .putString(displayKey, "$deviceName: ${InputBindingSetting.getButtonName(keyCode)}")
            .apply()
    }

    fun clear() {
        preferences.edit().remove(key).remove(displayKey).apply()
    }

    override val type = TYPE_DS_INPUT_BINDING

    private val displayKey: String get() = "${key}_display"
}
