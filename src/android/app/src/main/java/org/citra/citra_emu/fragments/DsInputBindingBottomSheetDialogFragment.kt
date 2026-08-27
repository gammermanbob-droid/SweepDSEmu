// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.fragments

import android.content.DialogInterface
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import org.citra.citra_emu.R
import org.citra.citra_emu.databinding.DialogInputBinding
import org.citra.citra_emu.features.settings.model.view.DsInputBindingSetting

/**
 * "Press a button" capture dialog for DS control bindings -- the DS
 * analogue of [MotionBottomSheetDialogFragment]. Mostly just its key-
 * capture half (ACTION_UP -> [DsInputBindingSetting.onKeyInput]) since
 * DS bindings are plain discrete buttons with no analog directions to
 * capture ([MotionBottomSheetDialogFragment] can't be reused directly
 * here since its `setting` field and capture logic are typed to the
 * 3DS-specific InputBindingSetting) -- except for L2/R2, which on most
 * gamepads report as continuous analog trigger axes rather than key
 * events at all (see [onMotionEvent]), so a *little* of
 * MotionBottomSheetDialogFragment's own axis-capture is still needed.
 */
class DsInputBindingBottomSheetDialogFragment : BottomSheetDialogFragment() {
    private var _binding: DialogInputBinding? = null
    private val binding get() = _binding!!

    private var setting: DsInputBindingSetting? = null
    private var onCancel: (() -> Unit)? = null
    private var onDismiss: (() -> Unit)? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (setting == null) {
            dismiss()
        }
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = DialogInputBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        BottomSheetBehavior.from<View>(view.parent as View).state =
            BottomSheetBehavior.STATE_EXPANDED

        isCancelable = false
        view.requestFocus()
        view.setOnFocusChangeListener { v, hasFocus -> if (!hasFocus) v.requestFocus() }
        dialog?.setOnKeyListener { _, _, event -> onKeyEvent(event) }
        binding.root.setOnGenericMotionListener { _, event -> onMotionEvent(event) }

        binding.textTitle.text =
            String.format(
                getString(R.string.input_dialog_title),
                getString(R.string.button),
                getString(setting!!.nameId)
            )
        binding.textMessage.text = getString(R.string.input_dialog_description)

        binding.buttonClear.setOnClickListener {
            setting?.clear()
            dismiss()
        }
        binding.buttonCancel.setOnClickListener {
            onCancel?.invoke()
            dismiss()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    override fun onDismiss(dialog: DialogInterface) {
        super.onDismiss(dialog)
        onDismiss?.invoke()
    }

    private fun onKeyEvent(event: KeyEvent): Boolean {
        return when (event.action) {
            KeyEvent.ACTION_UP -> {
                setting?.onKeyInput(event)
                dismiss()
                true
            }

            else -> false
        }
    }

    // Same axis names DsEmulationActivity.dispatchGenericMotionEvent's own
    // runtime trigger handling checks -- AXIS_BRAKE/AXIS_GAS cover
    // controllers that report L2/R2 under those names instead. Binds the
    // exact synthetic keycode that runtime handling looks for via
    // keyCodeToDsButton(KEYCODE_BUTTON_L2/R2), so this dialog's capture
    // and the runtime's own check agree on what "L2/R2 bound" means.
    private fun onMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_CLASS_JOYSTICK == 0) return false
        if (event.action != MotionEvent.ACTION_MOVE) return false

        val lTrigger = event.getAxisValue(MotionEvent.AXIS_LTRIGGER)
            .takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_BRAKE)
        val rTrigger = event.getAxisValue(MotionEvent.AXIS_RTRIGGER)
            .takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_GAS)

        val deviceName = event.device?.name ?: "Controller"
        if (lTrigger > 0.5f) {
            setting?.onAxisTriggerInput(KeyEvent.KEYCODE_BUTTON_L2, deviceName)
            dismiss()
            return true
        }
        if (rTrigger > 0.5f) {
            setting?.onAxisTriggerInput(KeyEvent.KEYCODE_BUTTON_R2, deviceName)
            dismiss()
            return true
        }
        return true
    }

    companion object {
        const val TAG = "DsInputBindingBottomSheetDialogFragment"

        fun newInstance(
            setting: DsInputBindingSetting,
            onCancel: () -> Unit,
            onDismiss: () -> Unit
        ): DsInputBindingBottomSheetDialogFragment {
            val dialog = DsInputBindingBottomSheetDialogFragment()
            dialog.apply {
                this.setting = setting
                this.onCancel = onCancel
                this.onDismiss = onDismiss
            }
            return dialog
        }
    }
}
