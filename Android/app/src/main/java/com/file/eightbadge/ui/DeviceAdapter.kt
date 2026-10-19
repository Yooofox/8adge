package com.file.eightbadge.ui

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.file.eightbadge.databinding.ItemBleDeviceBinding
import com.file.eightbadge.model.BleDeviceItem

class DeviceAdapter(
    private val onConnectClick: (BleDeviceItem) -> Unit
) : ListAdapter<BleDeviceItem, DeviceAdapter.DeviceViewHolder>(DiffCallback) {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): DeviceViewHolder {
        val binding = ItemBleDeviceBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return DeviceViewHolder(binding)
    }

    override fun onBindViewHolder(holder: DeviceViewHolder, position: Int) {
        holder.bind(getItem(position))
    }

    inner class DeviceViewHolder(private val binding: ItemBleDeviceBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: BleDeviceItem) {
            binding.textDeviceName.text = item.name
            binding.textMacAddress.text = item.address
            binding.textRssi.text = "${item.rssi} dBm"
            binding.btnConnect.setOnClickListener {
                onConnectClick(item)
            }
        }
    }

    companion object DiffCallback : DiffUtil.ItemCallback<BleDeviceItem>() {
        override fun areItemsTheSame(oldItem: BleDeviceItem, newItem: BleDeviceItem): Boolean {
            return oldItem.address == newItem.address
        }

        override fun areContentsTheSame(oldItem: BleDeviceItem, newItem: BleDeviceItem): Boolean {
            return oldItem == newItem
        }
    }
}
