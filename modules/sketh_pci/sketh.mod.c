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
	{ 0xae2ae519, "param_ops_int" },
	{ 0xaf0f7de1, "eth_validate_addr" },
	{ 0x1c937e3f, "pci_unregister_driver" },
	{ 0x2554c13b, "__pci_register_driver" },
	{ 0x92997ed8, "_printk" },
	{ 0x71038ac7, "pv_ops" },
	{ 0xba8fbd64, "_raw_spin_lock" },
	{ 0x46afdcba, "dma_free_attrs" },
	{ 0xabdba6e3, "napi_disable" },
	{ 0xfedc4dd2, "netif_tx_stop_all_queues" },
	{ 0x75c47b17, "napi_enable" },
	{ 0xc1514a3b, "free_irq" },
	{ 0x59c6aff4, "irq_set_affinity_hint" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0xfcec0987, "enable_irq" },
	{ 0x622933d1, "napi_complete_done" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0x5c2bcd37, "bpf_warn_invalid_xdp_action" },
	{ 0xcad13362, "netif_receive_skb" },
	{ 0x58ca4ff9, "eth_type_trans" },
	{ 0x28d384c6, "skb_put" },
	{ 0x53569707, "this_cpu_off" },
	{ 0x3a26ed11, "sched_clock" },
	{ 0x3429b711, "xdp_do_redirect" },
	{ 0x65487097, "__x86_indirect_thunk_rax" },
	{ 0xf84bd6ee, "bpf_stats_enabled_key" },
	{ 0x6a2bfc93, "netif_tx_wake_queue" },
	{ 0xbcaa1d60, "dma_unmap_page_attrs" },
	{ 0x42842159, "netdev_warn" },
	{ 0xa648e561, "__ubsan_handle_shift_out_of_bounds" },
	{ 0xc4cffb2c, "netdev_err" },
	{ 0xdf6c1fbd, "nf_register_net_hook" },
	{ 0x73a5ad3b, "register_netdev" },
	{ 0x40a9b349, "vzalloc" },
	{ 0xe0fcd26a, "alloc_etherdev_mqs" },
	{ 0xaff2dd5d, "pci_set_master" },
	{ 0x5b788242, "pci_request_selected_regions" },
	{ 0xa74ccb9b, "pci_enable_device" },
	{ 0xa487e741, "consume_skb" },
	{ 0xbcd5486f, "__netdev_alloc_skb" },
	{ 0x9e69cea5, "unregister_netdev" },
	{ 0xe821a606, "nf_unregister_net_hook" },
	{ 0x18c00784, "init_net" },
	{ 0x69449ab2, "pci_disable_device" },
	{ 0x93ac35bd, "pci_release_selected_regions" },
	{ 0x9c1b39b3, "free_netdev" },
	{ 0xef61363a, "bpf_prog_put" },
	{ 0x999e8297, "vfree" },
	{ 0xb1e18901, "pci_select_bars" },
	{ 0x4c9d28b0, "phys_base" },
	{ 0x56470118, "__warn_printk" },
	{ 0x14d83f6a, "dev_driver_string" },
	{ 0x73aefea2, "__dev_kfree_skb_any" },
	{ 0x15ba50a6, "jiffies" },
	{ 0x3f68deca, "dma_map_page_attrs" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0xc31db0ce, "is_vmalloc_addr" },
	{ 0x54b1fac6, "__ubsan_handle_load_invalid_value" },
	{ 0x242c6648, "netdev_info" },
	{ 0x754d539c, "strlen" },
	{ 0x298062ec, "__napi_schedule" },
	{ 0x17ad2c10, "napi_schedule_prep" },
	{ 0xc5b6f236, "queue_work_on" },
	{ 0x2d3385d3, "system_wq" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0xbdfb6dbb, "__fentry__" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("pci:v00001234d00005678sv*sd*bc*sc*i*");

MODULE_INFO(srcversion, "BB652AD2AC5E8E7C4CB0AB9");
