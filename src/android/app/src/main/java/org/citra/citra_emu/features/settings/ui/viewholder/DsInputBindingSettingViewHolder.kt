// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.settings.ui.viewholder

import android.view.View
import org.citra.citra_emu.databinding.ListItemSettingBinding
import org.citra.citra_emu.features.settings.model.view.DsInputBindingSetting
import org.citra.citra_emu.features.settings.model.view.SettingsItem
import org.citra.citra_emu.features.settings.ui.SettingsAdapter

class DsInputBindingSettingViewHolder(val binding: ListItemSettingBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {
    private lateinit var setting: DsInputBindingSetting

    override fun bind(item: SettingsItem) {
        setting = item as DsInputBindingSetting
        binding.textSettingName.setText(item.nameId)
        val display = setting.displayValue
        if (display.isNotEmpty()) {
            binding.textSettingDescription.visibility = View.GONE
            binding.textSettingValue.visibility = View.VISIBLE
            binding.textSettingValue.text = display
        } else {
            binding.textSettingDescription.visibility = View.GONE
            binding.textSettingValue.visibility = View.GONE
        }

        val alpha = if (setting.isActive) 1f else 0.5f
        binding.textSettingName.alpha = alpha
        binding.textSettingDescription.alpha = alpha
        binding.textSettingValue.alpha = alpha
    }

    override fun onClick(clicked: View) {
        adapter.onDsInputBindingClick(setting, bindingAdapterPosition)
    }

    override fun onLongClick(clicked: View): Boolean {
        adapter.onDsInputBindingLongClick(setting, bindingAdapterPosition)
        return false
    }
}
