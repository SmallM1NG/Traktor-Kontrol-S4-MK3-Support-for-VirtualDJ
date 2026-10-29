# FAQ ❓

中文 · [English](FAQ.md) · [项目说明](../../README_zh.md)

**1.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">原理是什么？</b>

&emsp;**A:** 本项目在 VirtualDJ 官方映射的基础上，通过 DLL 读取软件中的 Deck 状态，并将数据传给配套 EXE，由 EXE 通过 NIHardwareService 控制电机、灯环和触觉反馈。屏幕内容由 VirtualDJ 皮肤绘制，开启屏幕输出接管后在后台异步发送。全程使用 NI 官方驱动，不需要替换为 WinUSB。

**2.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">支持哪些版本？</b>

&emsp;**A:** 仅支持 Windows x64 的 VirtualDJ 2021 及以上版本，需拥有 VirtualDJ Pro 许可证，并安装 NI 官方驱动及 NIHardwareService。本项目基于 VirtualDJ 9005 开发，仅在 9005 / 9644 下测试过，不保证全部功能在任意版本中都能正常使用。

**3.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">为什么需要管理员权限？</b>

&emsp;**A:** 配套 EXE 需要访问 NIHardwareService 进程及设备对象，执行电机、灯环和触觉控制，因此需要管理员权限。启动时允许配套助手的 UAC 提权即可，无需将 VirtualDJ 本身设置为以管理员身份运行。

**4.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">能不能改名或混用版本？</b>

&emsp;**A:** 不能修改 DLL / EXE 的文件名，也不能混用不同次构建的 DLL 和 EXE。DLL 绑定了配套 EXE 的文件名与 SHA-256，即使版本号相同，重新编译的 EXE 也必须搭配同次生成的 DLL。更新时使用同一发行包内的配套文件。

**5.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">能不能与 Traktor 交替使用？</b>

&emsp;**A:** 可以。本项目使用 NIHardwareService 和 NI 官方驱动，正常关闭 VirtualDJ 后，可直接启动 Traktor Pro 3/4 并正常使用，无需更换驱动或执行额外操作。

**6.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">不使用 S4 MK3 时，安装的组件会有影响吗？</b>

&emsp;**A:** DLL 位于 AutoStart，因此启动 VirtualDJ 时仍会加载，进行初始化、写入日志并等待设备连接；它并非完全不运行。未检测到有效的 S4 MK3 原生连接时，不会启动配套 EXE，也不会进入设备电机、灯环和触觉控制。

**7.** <b style="font-size: 1.15em">Q:</b> <b style="font-size: 1.15em">弹出功能不可用的报错框？</b>

&emsp;**A:** 这表示对应功能所需的内部位置或设备对象未通过校验，相关自定义功能已被禁用。VirtualDJ 或 NIHardwareService 的版本变化可能导致这种情况，不代表所有功能都无法使用。请提交 issue。
