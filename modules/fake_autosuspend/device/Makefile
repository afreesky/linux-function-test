# 指定内核源码路径（若未通过命令行传入）
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# 模块目标名称（需与源码中的module_init/module_exit对应）
MODULE_NAME = fake_platform_device
obj-m := $(MODULE_NAME).o

# 多文件模块需列出所有依赖对象（示例）
$(MODULE_NAME)-objs := main.o

# 开发机本地头文件路径（可选）
ccflags-y := -I$(PWD)/include

# 编译目标
all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# 清理和安装目标
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	sudo insmod $(MODULE_NAME).ko

uninstall:
	sudo rmmod $(MODULE_NAME)

# 调试选项（可选）
DEBUG_CFLAGS += -DDEBUG -g
ccflags-y += $(DEBUG_CFLAGS)
