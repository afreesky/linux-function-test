#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x2c635209, "module_layout" },
	{ 0x619cb7dd, "simple_read_from_buffer" },
	{ 0x7005463f, "debugfs_create_dir" },
	{ 0x89182523, "__pm_runtime_use_autosuspend" },
	{ 0x54b1fac6, "__ubsan_handle_load_invalid_value" },
	{ 0x56d7310c, "__pm_runtime_suspend" },
	{ 0xf8f7f9b4, "debugfs_create_file" },
	{ 0xf78aa7ac, "__platform_driver_register" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0xa555f7ee, "debugfs_remove" },
	{ 0xf0311d7, "_dev_err" },
	{ 0xec2dcbc3, "_dev_info" },
	{ 0xe732a328, "pm_runtime_enable" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0x92997ed8, "_printk" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x578da51f, "pm_runtime_set_autosuspend_delay" },
	{ 0x656e4a6e, "snprintf" },
	{ 0x6ebe366f, "ktime_get_mono_fast_ns" },
	{ 0x9d6d692d, "platform_driver_unregister" },
	{ 0xc3fcadfd, "devm_kmalloc" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x88db9f48, "__check_object_size" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "C12A750D45F84B4B7737B0D");
