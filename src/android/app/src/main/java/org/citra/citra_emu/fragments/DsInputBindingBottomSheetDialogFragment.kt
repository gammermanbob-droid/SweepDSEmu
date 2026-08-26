// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.fragments

import android.content.DialogInterface
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import org.citra.citra_emu.R
import org.citra.citra_emu.databinding.DialogInputBinding
import org.citra.citra_emu.features.settings.model.view.DsInputBindingSetting

/**
 * "Press a button" capture dialog for DS control bindings -- the DS
 * analogue of [MotionBottomSheetDialogFragment], trimmed down to just
 * its key-capture half (ACTION_UP -> [DsInputBindingSetting.onKeyInput])
 * since DS bindings are plain discrete buttons, never analog axes;
 * [MotionBottomSheetDialogFragment] can't be reused directly here since
 * its `setting` field and capture logic are typed to the 3DS-specific
 * InputBindingSetting.
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
