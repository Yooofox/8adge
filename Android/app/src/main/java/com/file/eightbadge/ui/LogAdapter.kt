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
                LogType.INFO -> Color.parseColor("#80D8FF")    // 青色 / 浅蓝
                LogType.SUCCESS -> Color.parseColor("#B9F6CA") // 浅绿色
                LogType.WARNING -> Color.parseColor("#FFE082") // 琥珀色 / 黄色
                LogType.ERROR -> Color.parseColor("#FF8A80")   // 红色
                LogType.BLE_TX -> Color.parseColor("#EA80FC")  // 紫色 / 粉红色
                LogType.BLE_RX -> Color.parseColor("#A7FFEB")  // 青绿色
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
