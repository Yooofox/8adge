package com.file.eightbadge.ui

import android.graphics.Color
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.file.eightbadge.databinding.ItemConsoleLogBinding
import com.file.eightbadge.model.LogMessage
import com.file.eightbadge.model.LogType

class LogAdapter : ListAdapter<LogMessage, LogAdapter.LogViewHolder>(DiffCallback) {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): LogViewHolder {
        val binding = ItemConsoleLogBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return LogViewHolder(binding)
    }

    override fun onBindViewHolder(holder: LogViewHolder, position: Int) {
        holder.bind(getItem(position))
    }

    inner class LogViewHolder(private val binding: ItemConsoleLogBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: LogMessage) {
            binding.textTimestamp.text = item.timestamp
            binding.textLogContent.text = "[${item.type}] ${item.message}"

            val color = when (item.type) {
                LogType.INFO -> Color.parseColor("#80D8FF")    // Cyan / Light Blue
                LogType.SUCCESS -> Color.parseColor("#B9F6CA") // Light Green
                LogType.WARNING -> Color.parseColor("#FFE082") // Amber / Yellow
                LogType.ERROR -> Color.parseColor("#FF8A80")   // Red
                LogType.BLE_TX -> Color.parseColor("#EA80FC")  // Purple / Pink
                LogType.BLE_RX -> Color.parseColor("#A7FFEB")  // Teal
            }
            binding.textLogContent.setTextColor(color)
        }
    }

    companion object DiffCallback : DiffUtil.ItemCallback<LogMessage>() {
        override fun areItemsTheSame(oldItem: LogMessage, newItem: LogMessage): Boolean {
            return oldItem.id == newItem.id
        }

        override fun areContentsTheSame(oldItem: LogMessage, newItem: LogMessage): Boolean {
            return oldItem == newItem
        }
    }
}
