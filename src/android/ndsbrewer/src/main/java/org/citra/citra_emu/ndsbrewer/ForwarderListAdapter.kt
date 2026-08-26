// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import java.io.File

class ForwarderListAdapter(
    private val forwarders: List<Forwarder>,
    private val onDelete: (Forwarder) -> Unit
) : RecyclerView.Adapter<ForwarderListAdapter.ViewHolder>() {

    class ViewHolder(itemView: android.view.View) : RecyclerView.ViewHolder(itemView) {
        val icon: ImageView = itemView.findViewById(R.id.icon)
        val name: TextView = itemView.findViewById(R.id.name)
        val deleteButton: android.widget.ImageButton = itemView.findViewById(R.id.delete_button)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context).inflate(R.layout.item_forwarder, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val forwarder = forwarders[position]
        val romName = File(forwarder.romPath).name
        holder.name.text = "$romName  (%016x)".format(forwarder.programId)
        // Best-effort: the original ROM may have since been moved/deleted
        // out from under an already-registered forwarder, in which case
        // this is just null and the icon stays blank rather than crashing
        // the whole list.
        holder.icon.setImageBitmap(
            if (File(forwarder.romPath).isFile) NdsIconDecoder.decode(forwarder.romPath) else null
        )
        holder.deleteButton.setOnClickListener { onDelete(forwarder) }
    }

    override fun getItemCount() = forwarders.size
}
