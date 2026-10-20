// 顶层构建脚本：这里可以添加对所有子项目/模块生效的通用配置。
plugins {
    alias(libs.plugins.android.application) apply false
}