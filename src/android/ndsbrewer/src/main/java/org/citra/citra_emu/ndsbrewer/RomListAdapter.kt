// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

class RomListAdapter(
    private val roms: List<NdsRom>,
    private val onCheckedChanged: () -> Unit = {}
) : RecyclerView.Adapter<RomListAdapter.ViewHolder>() {

    val checkedStates = BooleanArray(roms.size)

    class ViewHolder(itemView: android.view.View) : RecyclerView.ViewHolder(itemView) {
        val checkbox: CheckBox = itemView.findViewById(R.id.checkbox)
        val icon: ImageView = itemView.findViewById(R.id.icon)
        val name: TextView = itemView.findViewById(R.id.name)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context).inflate(R.layout.item_rom, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val rom = roms[position]
        holder.name.text = rom.name
        holder.icon.setImageBitmap(rom.icon)
        holder.checkbox.setOnCheckedChangeListener(null)
        holder.checkbox.isChecked = checkedStates[position]
        holder.checkbox.setOnCheckedChangeListener { _, checked ->
            checkedStates[position] = checked
            onCheckedChanged()
        }
        holder.itemView.setOnClickListener { holder.checkbox.toggle() }
    }

    override fun getItemCount() = roms.size

    fun setAllChecked(checked: Boolean) {
        checkedStates.fill(checked)
        notifyDataSetChanged()
    }

    fun selectedRoms(): List<NdsRom> = roms.filterIndexed { i, _ -> checkedStates[i] }
}
