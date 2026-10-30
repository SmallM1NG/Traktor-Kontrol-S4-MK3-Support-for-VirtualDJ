<p align="center">
  <img width="72%" alt="Traktor Kontrol S4 MK3 Support for VirtualDJ" src="Docs/images/header.png">
</p>

<p align="center"><b>Traktor Kontrol S4 MK3 Support for VirtualDJ</b></p>
<p align="center">
	<a href="https://github.com/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ?color=brightgreen&label=Latest&style=for-the-badge"></a>
	<img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white&style=for-the-badge">
	<img alt="Windows x64" src="https://img.shields.io/badge/Windows-x64-0078D6.svg?logo=windows&logoColor=white&style=for-the-badge">
	<img alt="VirtualDJ 8" src="https://img.shields.io/badge/VirtualDJ-8-E60000.svg?style=for-the-badge">
	<img alt="License GPLv3" src="https://img.shields.io/badge/License-GPLv3-blue.svg?style=for-the-badge">
	<a href="https://github.com/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ?style=for-the-badge"></a>
</p>

<p align="center"><b>中文</b> · <a href="README.md">English</a></p>

<p align="center"><a href="#项目介绍">项目介绍</a> · <a href="#功能展示">功能展示</a> · <a href="#如何安装">如何安装</a> · <a href="#如何使用">如何使用</a> · <a href="#其他内容">其他内容</a></p>

<p align="center"><a href="Docs/md/FAQ_zh.md">FAQ</a> · <a href="Docs/md/DEVELOPMENT_zh.md">开发相关</a> · <a href="Docs/md/CHANGELOG_zh.md">更改日志</a></p>

---

<a id="项目介绍"></a>
## 项目介绍 ℹ️

本项目为 **VirtualDJ** 提供 **Native Instruments Traktor Kontrol S4 MK3** 的 **完整（几乎）** 支持。

- 基于 VirtualDJ 官方提供的映射，补齐了启动自动报告推子位置、屏幕显示、电机和灯光相关的一些功能。
- 支持屏幕左右独立显示，提供 **12 个页面**，可随时切换。
- 支持 **电机相关功能 (TT Mode)**，可在设置中调整 **转速、刻度触感、阻力、Hotcue 触觉反馈**等功能。
- 支持 **17 种 Deck 颜色**切换。

> 本项目以 [GPLv3](LICENSE) 开源，使用、修改或分发时请遵守该协议。

> [!WARNING]
> 本项目由爱好者自行维护，与 VirtualDJ、Native Instruments 公司无任何关系。  
> 本项目仅支持 <strong>Windows x64</strong>，基于 **VirtualDJ 9005** 版本开发，且仅在 **9005 / 9644** 版本下测试过，不保证全部功能在任意版本中都能正常使用。  
> 本项目部分功能为实验性功能，未经长期测试，可能出现稳定性问题，请自行承担由此造成的损失。

<a id="功能展示"></a>
## 功能展示 ✨

### 实机演示

<p align="center">
  <img src="Docs/images/controller-demo.webp" width="100%" alt="控制器功能演示">
</p>

### 屏幕样式

<table align="center">
  <tr>
    <td align="center"><img src="Docs/images/screens/page-12.png" width="200" alt="总览波形"><br><b>总览波形</b></td>
    <td align="center"><img src="Docs/images/screens/page-01.png" width="200" alt="实时波形"><br><b>实时波形</b></td>
    <td align="center"><img src="Docs/images/screens/page-02.png" width="200" alt="频谱"><br><b>频谱</b></td>
    <td align="center"><img src="Docs/images/screens/page-03.png" width="200" alt="效果"><br><b>效果</b></td>
  </tr>
  <tr>
    <td align="center"><img src="Docs/images/screens/page-04.png" width="200" alt="PAD"><br><b>PAD</b></td>
    <td align="center"><img src="Docs/images/screens/page-05.png" width="200" alt="混音器"><br><b>混音器</b></td>
    <td align="center"><img src="Docs/images/screens/page-06.png" width="200" alt="原生浏览器"><br><b>原生浏览器</b></td>
    <td align="center"><img src="Docs/images/screens/page-07.png" width="200" alt="自绘浏览器"><br><b>自绘浏览器</b></td>
  </tr>
  <tr>
    <td align="center"><img src="Docs/images/screens/page-08.png" width="200" alt="3D 浏览器"><br><b>3D 浏览器</b></td>
    <td align="center"><img src="Docs/images/screens/page-09.png" width="200" alt="歌曲封面"><br><b>歌曲封面</b></td>
    <td align="center"><img src="Docs/images/screens/page-10.png" width="200" alt="LOGO"><br><b>LOGO</b></td>
    <td align="center"><img src="Docs/images/screens/page-11.png" width="200" alt="视频"><br><b>视频</b></td>
  </tr>
</table>

### Deck 色卡

<table align="center">
  <tr>
    <td align="center" width="64"><img src="Docs/images/colors/red.png" width="48" height="16" alt="Red 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/carrot.png" width="48" height="16" alt="Carrot 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/orange.png" width="48" height="16" alt="Orange 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/beige.png" width="48" height="16" alt="Beige 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/yellow.png" width="48" height="16" alt="Yellow 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/lime.png" width="48" height="16" alt="Lime 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/green.png" width="48" height="16" alt="Green 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/aqua.png" width="48" height="16" alt="Aqua 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/cyan.png" width="48" height="16" alt="Cyan 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/marine.png" width="48" height="16" alt="Marine 亮色"></td>
  </tr>
  <tr>
    <td align="center" width="64"><img src="Docs/images/colors/dark/red.png" width="48" height="16" alt="Red 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/carrot.png" width="48" height="16" alt="Carrot 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/orange.png" width="48" height="16" alt="Orange 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/beige.png" width="48" height="16" alt="Beige 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/yellow.png" width="48" height="16" alt="Yellow 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/lime.png" width="48" height="16" alt="Lime 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/green.png" width="48" height="16" alt="Green 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/aqua.png" width="48" height="16" alt="Aqua 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/cyan.png" width="48" height="16" alt="Cyan 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/marine.png" width="48" height="16" alt="Marine 暗色"></td>
  </tr>
  <tr>
    <td align="center" width="64">Red</td>
    <td align="center" width="64">Carrot</td>
    <td align="center" width="64">Orange</td>
    <td align="center" width="64">Beige</td>
    <td align="center" width="64">Yellow</td>
    <td align="center" width="64">Lime</td>
    <td align="center" width="64">Green</td>
    <td align="center" width="64">Aqua</td>
    <td align="center" width="64">Cyan</td>
    <td align="center" width="64">Marine</td>
  </tr>
</table>

<table align="center">
  <tr>
    <td align="center" width="64"><img src="Docs/images/colors/blue.png" width="48" height="16" alt="Blue 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/violet.png" width="48" height="16" alt="Violet 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/fuscia.png" width="48" height="16" alt="Fuscia 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/magenta.png" width="48" height="16" alt="Magenta 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/pink.png" width="48" height="16" alt="Pink 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/salmon.png" width="48" height="16" alt="Salmon 亮色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/white.png" width="48" height="16" alt="White 亮色"></td>
  </tr>
  <tr>
    <td align="center" width="64"><img src="Docs/images/colors/dark/blue.png" width="48" height="16" alt="Blue 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/violet.png" width="48" height="16" alt="Violet 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/fuscia.png" width="48" height="16" alt="Fuscia 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/magenta.png" width="48" height="16" alt="Magenta 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/pink.png" width="48" height="16" alt="Pink 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/salmon.png" width="48" height="16" alt="Salmon 暗色"></td>
    <td align="center" width="64"><img src="Docs/images/colors/dark/white.png" width="48" height="16" alt="White 暗色"></td>
  </tr>
  <tr>
    <td align="center" width="64">Blue</td>
    <td align="center" width="64">Violet</td>
    <td align="center" width="64">Fuscia</td>
    <td align="center" width="64">Magenta</td>
    <td align="center" width="64">Pink</td>
    <td align="center" width="64">Salmon</td>
    <td align="center" width="64">White</td>
  </tr>
</table>


---

<a id="如何安装"></a>
## 如何安装 📥

本项目仅支持 **Windows x64** 的 **VirtualDJ 2021 及以上版本**，需拥有 **VirtualDJ Pro** 许可证，且依赖 **NIHardwareService**，请确保符合以上要求。

> [!WARNING]
> **注意**：如果你曾经使用过其他类似项目，并将屏幕驱动替换为了 **WinUSB（通过 USB Bulk 与屏幕通信）**，请将屏幕驱动恢复至 NI 官方驱动 **niks4m3bd** 才可使用本项目。如果更换了其他模块的驱动，也请全部恢复。  
> 本项目所实现的功能基于 **NIHardwareService** 和 NI 官方驱动，这意味着你可以在<strong>关闭 VirtualDJ</strong> 之后直接启动 Traktor Pro 3/4 并正常使用，而无需其他额外操作。  

<ol>
<li>

<p><strong>先确认系统已安装 NIHardwareService</strong>。通常在安装 Traktor Pro 或 Controller Editor 时，该服务会一并安装。</p>
<p><strong>检查方法：</strong>按 <code>Win + R</code>，输入 <code>services.msc</code> 并回车，在“服务”列表中查找 <strong>NIHardwareService</strong>。</p>
<ul>
  <li><strong>已找到：</strong>确认服务处于“正在运行”状态，然后继续下一步；如果尚未运行，右键选择“启动”后再次确认。</li>
  <li><strong>未找到：</strong>安装以下任一软件：
    <ul>
      <li><a href="https://www.native-instruments.com/zh/products/traktor-pro">Traktor Pro</a></li>
      <li><a href="https://support.native-instruments.com/support/solutions/articles/69000882238-native-instruments-drivers-other-files#Controller-Editor">Controller Editor（Windows 版）</a></li>
    </ul>
    安装完成后，再次打开“服务”检查 NIHardwareService。确认已存在并正在运行后，再继续下一步。
  </li>
</ul>

</li>

<li>

从 [Releases](https://github.com/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ/releases/latest) 下载最新 **ZIP 压缩包**（当前为 [v0.1.0](https://github.com/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ/releases/tag/v0.1.0)）。

</li>
<li>

<strong>完整解压</strong>压缩包内的内容，应包含以下文件：

```text
Traktor Kontrol S4 MK3 Bridge v0.1.0.dll
Traktor Kontrol S4 MK3 NI Service Helper v0.1.0.exe
Traktor Kontrol S4 MK3 Screen Preview (Default).zip
Traktor Kontrol S4 MK3 Screen.zip
Traktor Kontrol S4 MK3.zip
S4MK3 Settings.xml
```

</li>
<li>

<strong>找到你的 VirtualDJ 数据目录</strong>：

- 2023 之前的版本，通常位于 `C:\Users\用户名\Documents\VirtualDJ`。
- 2023 之后的版本，通常位于 `C:\Users\用户名\AppData\Local\VirtualDJ`。
- 如果你更改了数据目录，请确认自定义的数据目录位置。

</li>
<li>

<strong>关闭 VirtualDJ</strong>，然后按下表将文件放入数据目录内的对应文件夹：

| 文件 | 目标目录（相对于 VirtualDJ 数据目录） | 说明 |
| --- | --- | --- |
| `Traktor Kontrol S4 MK3 Bridge v0.1.0.dll` | `Plugins64\AutoStart` | <strong>与配套 EXE 放在一起</strong> |
| `Traktor Kontrol S4 MK3 NI Service Helper v0.1.0.exe` | `Plugins64\AutoStart` | <strong>与配套 DLL 放在一起</strong> |
| `Traktor Kontrol S4 MK3.zip` | `Devices` | <strong>无需解压</strong> |
| `Traktor Kontrol S4 MK3 Screen.zip` | `Skins` | <strong>无需解压</strong> |
| `Traktor Kontrol S4 MK3 Screen Preview (Default).zip` | `Skins` | <strong>无需解压</strong> |
| `S4MK3 Settings.xml` | `Pads` | 直接复制 |

</li>
<li>

启动 VirtualDJ，在 Master 效果栏中找到 **Traktor Kontrol S4 MK3 Bridge v0.1.0**，即为安装成功。

</li>
</ol>

---

<a id="如何使用"></a>
## 如何使用 ▶️

### 1. 连接设备并启动

接通 S4 MK3 的电源并连接至电脑，然后打开 VirtualDJ。待 VirtualDJ 与 S4 MK3 建立连接后，DLL 会<strong>自动启动配套 EXE</strong>。此时如弹出<strong>管理员权限</strong>提示，请点击<strong>“是”</strong>，即可使用本项目的全部功能。随后可通过提供的 PAD 映射或软件内的控制器设置调整部分功能。

> [!WARNING]
> 配套 EXE 需要访问 **NIHardwareService 进程及设备对象**，执行电机、灯环和触觉控制，因此需要<strong>管理员权限</strong>。如果拒绝提权，依赖 EXE 的这些功能将无法正常使用；<strong>无需将 VirtualDJ 本身设置为以管理员身份运行</strong>。  
> 如弹出<strong>“功能不可用”</strong>的报错框，表示对应功能所需的内部位置或设备对象未通过校验，<strong>该功能将被停用</strong>，并不代表全部功能都无法使用。请根据提示确认受影响的功能，并提交 Issue。

### 2. 由 PAD 页面调整的设置

> 除<strong>预览窗口的开关及合并 / 独立模式</strong>外，其余 PAD 设置均<strong>按 Deck 独立保存</strong>。

#### 打开设置页

在 VirtualDJ 的 PAD 页面中选择 **S4MK3 Settings**，通过 **Param 1** 切换以下三个设置子页：

| 子页 | 用途 |
| --- | --- |
| `TT/COLOR` | 开关 TT 模式、调整 Deck 配色 |
| `Screen Style` | 调整屏幕时间显示、波形和频谱样式，以及已播部分的颜色显示 |
| `Screen Switch` | 切换屏幕页面、设置 AUTO 和预览窗口 |

#### TT 模式与 Deck 配色

<p>在 <strong>TT/COLOR</strong> 子页中：</p>

<p align="left">
  <img src="Docs/images/settings/settings-02.png" width="600" alt="TT 模式与 Deck 配色">
</p>
<table>
<thead><tr><th>控件</th><th>功能</th></tr></thead>
<tbody>
<tr><td><code>TT ON/OFF</code></td><td>开关当前 Deck 的 TT 模式</td></tr>
<tr><td><code>DECK COLOR</code></td><td>将当前的 Deck 颜色应用到设备对应 Deck 的灯光颜色；识别设备支持的 17 色及其别名，其他颜色保留原有亮暗配对</td></tr>
<tr><td><code>PREV COLOR</code> / <code>NEXT COLOR</code></td><td>在 17 种颜色之间切换</td></tr>
<tr><td><code>CURRENT BRIGHT</code> / <code>CURRENT DIM</code></td><td>显示当前亮色和暗色</td></tr>
</tbody>
</table>

#### 屏幕样式

<p>在 <strong>Screen Style</strong> 子页中：</p>

<p align="left">
  <img src="Docs/images/settings/settings-03.png" width="600" alt="屏幕样式">
</p>
<table>
<thead><tr><th>控件</th><th>功能</th></tr></thead>
<tbody>
<tr><td><code>ELAPSED / REMAIN</code></td><td>切换已播放时间 / 剩余时间</td></tr>
<tr><td><code>TIME DECIMALS ON/OFF</code></td><td>开关时间的小数显示</td></tr>
<tr><td><code>SHADOW ON/OFF</code></td><td>开关实时波形的节拍背景阴影</td></tr>
<tr><td><code>BEAT NUMBERS ON/OFF</code></td><td>开关实时波形的节拍数字</td></tr>
<tr><td><code>SPECTRUM WHITE ON/OFF</code></td><td>开关频谱的白色显示</td></tr>
<tr><td><code>PLAYED COLOR ON/OFF</code></td><td>开关总览波形已播部分的 Deck 颜色遮罩，关闭时保留原生暗化效果</td></tr>
</tbody>
</table>

#### 屏幕切页与预览

<p>在 <strong>Screen Switch</strong> 子页中：</p>

<p align="left">
  <img src="Docs/images/settings/settings-04.png" width="600" alt="屏幕切页与预览">
</p>
<table>
<thead><tr><th>控件</th><th>功能</th></tr></thead>
<tbody>
<tr><td><code>SCREEN PREVIEW ON/OFF</code></td><td>显示 / 隐藏屏幕预览窗口</td></tr>
<tr><td><code>PREVIEW: COMBINED / SEPARATE</code></td><td>切换合并 / 独立预览窗口</td></tr>
<tr><td><code>AUTO ON/OFF</code></td><td>开关浏览时自动切页</td></tr>
<tr><td>当前页面名称</td><td>显示当前选中的屏幕页面</td></tr>
<tr><td><code>PREV PAGE</code> / <code>NEXT PAGE</code></td><td>切换当前 Deck 的上一页 / 下一页</td></tr>
</tbody>
</table>

<p>控制器上也可使用以下组合切页：</p>
<table>
<thead><tr><th>操作</th><th>功能</th></tr></thead>
<tbody>
<tr><td>SHIFT + A / C</td><td>左侧屏幕上一页 / 下一页</td></tr>
<tr><td>SHIFT + B / D</td><td>右侧屏幕上一页 / 下一页</td></tr>
</tbody>
</table>

<p><strong>AUTO</strong>：在 Screen Switch 中开启 <code>AUTO ON/OFF</code> 后，操作对应侧浏览旋钮会进入上一次的浏览器页面，使用控制器加载 / 克隆歌曲后返回上一次非浏览器页面。鼠标、键盘操作不触发此自动切页流程。</p>
<p><strong>预览窗口</strong>：选用 <strong>Traktor Kontrol S4 MK3 Screen Preview (Default)</strong> 皮肤后，在 Screen Switch 中使用 <code>SCREEN PREVIEW ON/OFF</code> 开关预览，使用 <code>PREVIEW: COMBINED / SEPARATE</code> 选择合并或独立窗口。窗口允许调整大小；缩放后不保证与实体屏幕逐像素一致。</p>

<table align="center">
<tr><th align="center"><p align="center">1</p></th><th align="center"><p align="center">2</p></th><th align="center"><p align="center">3</p></th><th align="center"><p align="center">4</p></th><th align="center"><p align="center">5</p></th><th align="center"><p align="center">6</p></th><th align="center"><p align="center">7</p></th><th align="center"><p align="center">8</p></th><th align="center"><p align="center">9</p></th><th align="center"><p align="center">10</p></th><th align="center"><p align="center">11</p></th><th align="center"><p align="center">12</p></th></tr>
<tr><td align="center"><p align="center">总览波形</p></td><td align="center"><p align="center">实时波形</p></td><td align="center"><p align="center">频谱</p></td><td align="center"><p align="center">效果</p></td><td align="center"><p align="center">PAD</p></td><td align="center"><p align="center">混音器</p></td><td align="center"><p align="center">原生浏览器</p></td><td align="center"><p align="center">自绘浏览器</p></td><td align="center"><p align="center">3D 浏览器</p></td><td align="center"><p align="center">歌曲封面</p></td><td align="center"><p align="center">LOGO</p></td><td align="center"><p align="center">视频</p></td></tr>
</table>

### 3. 由软件的控制器设置页面调整的设置

<ol>
<li>打开 VirtualDJ <strong>设置 → 控制器（Controllers）</strong>。</li>
<li>选择 <strong>Traktor Kontrol S4 MK3</strong>，点击设备的<strong>齿轮图标</strong>，打开专用设置。</li>
<li>找到下表对应的选项，选择所需模式或修改数值（和 Traktor Pro 3/4 中的对应同名设置项功能相同）。</li>
</ol>

<p align="center">
  <img src="Docs/images/settings/traktor-controller-settings.png" width="600" alt="Traktor Pro 3/4 设置页面">
  <br><b>Traktor Pro 3/4 设置页面</b>
</p>

<p align="center">
  <img src="Docs/images/settings/settings-01.png" width="600" alt="VirtualDJ 控制器设置页面">
  <br><b>VirtualDJ 控制器设置页面</b>
</p>
<table>
<thead><tr><th>设置项名称</th><th>说明</th></tr></thead>
<tbody>
<tr><td><code>JogLEDMode</code></td><td>设置 Jog 灯环的指针模式。选择 <strong>Physical Jog</strong> 时，在播放旋转基础上叠加手动位移，触摸或暂停时仅跟随手动位移，TT 模式下直接跟随实体碟盘；选择 <strong>Absolute Position</strong> 时完全跟随软件内的指针位置。</td></tr>
<tr><td><code>TTTurntableSpeed</code></td><td>设置碟盘旋转速度，选择 <strong>33.3 / 45 RPM</strong> 时会以对应速度并依据 Tempo 位置调整转速；选择 <strong>BPM / 4 (Experimental)</strong> 时按当前 BPM ÷ 4 计算转速。最高支持 <strong>75 RPM</strong>，超出时停止电机；转速恢复范围内且 TT 开启、歌曲播放时恢复旋转。</td></tr>
<tr><td><code>TicksWhenNudging</code></td><td>设置 Jog 模式下播放期间的刻度触感，选择 <strong>None</strong> 关闭，<strong>Coarse</strong> 为粗刻度，<strong>Fine</strong> 为细刻度，同时应用于左右两侧 Jog。</td></tr>
<tr><td><code>JogwheelTension</code></td><td>设置左右两侧 Jog 的旋转阻力，输入 <strong>0–255</strong> 的整数，数值越大阻力越大，0 为最轻。</td></tr>
<tr><td><code>HapticHotcues</code></td><td>设置是否启用 Cue 点触觉反馈，选择 <strong>yes</strong> 开启、<strong>no</strong> 关闭；开启后在主 Cue、<strong>Hotcue 1–8</strong> 及 <strong>Saved Loop 1–8</strong> 的起止点提供触觉反馈。</td></tr>
<tr><td><code>Screen Output Takeover</code></td><td>设置设备屏幕的输出方式。选择 <strong>ON</strong> 时，由 <strong>DLL 的后台线程</strong>发送屏幕画面，不影响软件主界面的皮肤渲染，可显著减少主界面卡顿情况；选择 <strong>OFF</strong> 时，恢复 VirtualDJ 原生输出。此设置同时应用于左右两块屏幕。</td></tr>
</tbody>
</table>

---

<a id="其他内容"></a>
## 其他内容 📚

<p align="center"><a href="Docs/md/DEVELOPMENT_zh.md">开发相关</a> · <a href="Docs/md/FAQ_zh.md">FAQ</a></p>

### 补充说明 💡

> [!WARNING]
> **当前已知问题**
>
> 1. **TT 模式下的侧面微调**：启用 TT 后，无法通过触摸碟盘侧面来临时加减速。我尝试了很多办法，但都有一些稳定性问题，加上暂时没有更多精力继续尝试，就先搁置了。
> 2. **HapticHotcues 的点位识别**：当前仅支持编号为 1–8 的 Hotcue / Saved Loop，不支持仅标记或隐藏的点。如果在启用 Loop 时设置 Hotcue，虽然使用起来与 Saved Loop 没有区别，但无法正确获取 Loop 长度。需要重启 VirtualDJ，待其自动变为 Saved Loop 后，才能正确获取长度。
> 3. **电机转速上限**：电机旋转速度的上限为 <strong>75 RPM（300 BPM ÷ 4）</strong>，适用于所有转速模式。由于尚不确定更高转速是否会损伤电机，因此设置了此上限。当准备发送给电机的转速超过上限时，会拒绝该转速并立即停止电机；转速恢复至限制范围内后，在 TT 模式开启且歌曲播放时自动恢复旋转。


---

<a id="bug-reports"></a>
### BUG 汇报 😨

遇到问题时，请先查看 [FAQ](Docs/md/FAQ_zh.md) 和上方的“当前已知问题”。如仍未解决，请通过 [Issues](https://github.com/SmallM1NG/Traktor-Kontrol-S4-MK3-Support-for-VirtualDJ/issues) 反馈，并提供以下信息：

- **版本信息**：本项目、VirtualDJ、Windows 和 NIHardwareService 的版本。
- **问题描述**：具体表现、是否可复现，复现步骤，以及对应截图等信息。
- **运行日志**：附上 VirtualDJ 数据目录 `Plugins64\AutoStart` 中的 DLL / EXE 日志，并注明问题发生的时间。

---

<a id="acknowledgements"></a>
### 鸣谢 🙌

感谢 [MIXXX](https://github.com/mixxxdj/mixxx)、[Encdr](https://github.com/RufusIbiza/Encdr) 为本项目开发提供了部分参考。  
感谢参与此项目测试的朋友们。

---

<a id="links"></a>
### LINK 🔗

<p align="center">
  <a href="https://space.bilibili.com/475951038">BILIBILI</a>
  ·
  <a href="https://cn.virtualdj.com/wiki/Developers.html">VirtualDJ 开发者文档</a>
</p>

---

<a id="donate"></a>
### 捐赠 🧋

<p align="center">
  🥰请我喝奶茶喵 谢谢你喵🥰
</p>

<p align="center">
  <a href="https://ko-fi.com/djm1ng"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi"></a>
</p>

<p align="center">
  <img width="420" src="Docs/images/alipay_wechat.jpg" alt="请我喝奶茶">
</p>
