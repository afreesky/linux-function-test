# Linux Gadget Driver 安装指南

## 概述

本驱动用于Windows 11系统，支持Linux USB Gadget设备虚拟出的:
- **ACM串口** (VID: 0x0123, PID: 0x0456)
- **NCM网口** (VID: 0x0123, PID: 0x0456)

## 前置要求

1. Windows 11 系统
2. Windows SDK 或 WDK (用于签名)
3. 以管理员权限运行以下命令

## 步骤 1: 生成并签名驱动

### 方法A: 使用PowerShell脚本 (推荐)

```powershell
# 1. 以管理员身份运行PowerShell
.\generate_cert_and_sign.ps1
```

### 方法B: 使用批处理脚本

```batch
# 1. 以管理员身份运行命令提示符
generate_cert_and_sign.bat
```

## 步骤 2: 导入证书到Windows

1. 双击生成的 `LinuxGadgetDriver.cer` 文件
2. 点击 "安装证书"
3. 选择 "本地计算机"
4. 将证书存储在 "受信任的根证书颁发机构"
5. 点击完成，确认所有安全警告

## 步骤 3: 安装驱动

### 方法A: 手动安装

1. 连接Linux Gadget设备到Windows PC
2. 打开 "设备管理器"
3. 找到未识别的设备（带黄色感叹号）
4. 右键点击设备 → "更新驱动程序"
5. 选择 "浏览我的计算机以查找驱动程序"
6. 浏览到包含 `.inf` 文件的目录
7. 勾选 "包括子文件夹"
8. 点击 "下一步" 完成安装

### 方法B: 使用Zadig风格的手动安装

1. 打开设备管理器
2. 找到 "通用串行总线控制器" 下的 "Linux Gadget" 设备
3. 右键 → "更新驱动程序"
4. 选择 "让我从计算机上的可用驱动程序列表中选取"
5. 选择 "端口" (COM & LPT) - 用于ACM
6. 选择 "网络适配器" - 用于NCM
7. 点击 "从磁盘安装"
8. 选择对应的 `.inf` 文件

## 步骤 4: 验证安装

### ACM串口验证

1. 打开设备管理器
2. 展开 "端口 (COM 和 LPT)"
3. 应该看到 "Linux Gadget ACM Serial Device"
4. 记住COM端口号

### NCM网口验证

1. 打开设备管理器
2. 展开 "网络适配器"
3. 应该看到 "Linux Gadget NCM Network Device"
4. 打开网络连接，应该看到新的以太网适配器

## 常见问题

### Q: 安装驱动时提示"第三方INF不包含数字签名"

**解决方案:**
1. 重新运行签名脚本确保签名成功
2. 确认证书已正确导入到"受信任的根证书颁发机构"
3. 或者临时禁用驱动签名强制:
   ```powershell
   bcdedit /set testsigning on
   ```
   重启后生效

### Q: 设备显示为"未知USB设备"

**解决方案:**
1. 确认Linux Gadget配置正确
2. 检查USB连接
3. 查看Windows事件查看器获取详细错误

### Q: NCM网口无法正常工作

**解决方案:**
1. 确认Linux端配置了正确的NCM gadget
2. 在Windows中手动配置IP地址（如果DHCP不可用）
3. 检查Windows防火墙设置

## 驱动文件说明

```
drvier/
├── acm_driver.inf      # ACM串口驱动配置文件
├── ncm_driver.inf     # NCM网口驱动配置文件
├── LinuxGadgetDriver.cer  # 签名证书（公钥）
├── LinuxGadgetDriver.pfx  # 签名证书（私钥，需保管好）
├── generate_cert_and_sign.ps1  # 签名脚本
└── generate_cert_and_sign.bat   # 签名脚本（备选）
```

## 技术说明

- **ACM驱动**: 使用Windows内置的 `usbser.sys`
- **NCM驱动**: 使用Windows内置的 `wdmncm.sys` 或 `rndis.sys`
- **签名**: 使用自签名证书（仅限测试用途）
- **生产环境**: 需要从受信任的CA购买代码签名证书

## 联系支持

如有问题，请检查:
1. Windows事件查看器 → Windows日志 → 系统
2. 设备管理器 → 右键设备 → 属性 → 详细信息
