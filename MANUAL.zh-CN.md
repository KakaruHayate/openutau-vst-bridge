# OpenUtau Bridge 使用说明书

**版本 0.2.0（Alpha）· 适用于协议 v1.1 配套的 OpenUtau 测试版**

本说明书讲的是**日常怎么用**：安装、连接、每天写歌的流程、各个功能在哪、出问题了去哪查。想参与测试反馈的，另见《测试指南》（`TESTING.zh-CN.md`）。

---

## 1. 它是怎么工作的

OpenUtau Bridge 是一个**乐器插件**（VST3 / CLAP），装在 DAW 里。它自己不发声，而是：

1. 启动后在本机开一个端口，**等 OpenUtau 来连**；
2. OpenUtau 通过 Tools → DAW Integration 找到它、连上它；
3. 之后你在 OpenUtau 里的每个声部（part）渲染完成后，音频就传进插件，**按 OpenUtau 时间轴上的绝对位置**摆在 DAW 里；
4. 你在 OpenUtau 里继续改，DAW 里跟着变。

所以正确的理解是：**OpenUtau 是编辑器和渲染引擎，DAW 是混音台。** 音量、声像、静音、效果器全部在 DAW 侧做；OpenUtau 的推子不影响传到 DAW 的信号。

## 2. 安装

**系统要求**：Windows 10/11 64 位、macOS 11+、或主流 Linux 发行版；一个能加载 VST3 或 CLAP **乐器**的 DAW（REAPER、Cubase、Studio One、FL Studio、Bitwig、Ableton Live、Waveform、Cakewalk 等）。Audition、Audacity 等只收效果器的宿主**用不了**。

需要两样东西，且**必须配套**：

1. **OpenUtau 测试版**（带 DAW Integration 功能的那份）——解压到任意目录即可，和你平时的 OpenUtau 互不干扰；
2. **插件**——按下表复制：

| 格式 | Windows | macOS | Linux |
|---|---|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\`（复制**整个文件夹**） | `~/Library/Audio/Plug-Ins/VST3/` | `~/.vst3/` |
| CLAP | `C:\Program Files\Common Files\CLAP\` | `~/Library/Audio/Plug-Ins/CLAP/` | `~/.clap/` |

> 没有管理员权限时，Windows 可改用 `%LOCALAPPDATA%\Programs\Common\VST3\` 和 `...\CLAP\`，效果相同。
>
> `.vst3` 是一个**文件夹**，整个复制，别只拷里面那个同名文件。

装完在 DAW 里**重新扫描插件**。macOS 首次加载若被 Gatekeeper 拦，右键 → 打开，或在系统设置里点「仍要打开」。

## 3. 第一次连接

**顺序很重要：先 DAW，后 OpenUtau。** 插件得先跑起来才能被找到。

1. 打开 DAW，新建工程；
2. 新建一条**乐器轨**（不是音频轨），在乐器位插入 **OpenUtau Bridge**（厂商 `KakaruHayate`）；
3. 打开配套的 OpenUtau 测试版；
4. 菜单 **Tools → DAW Integration...**；
5. 列表里应出现你的插件实例（`Status` 为 `Compatible`）。选中 → **Connect**；
6. 状态变成 `Connected to ...` 即成功。

**OpenUtau 工程必须先保存过。** 未保存的工程点 Connect 会收到「请先保存工程」的提示——这是刻意设计：未保存工程没有可靠的音频路径，传出去的内容没法保证。

## 4. 日常使用

### 写歌 → 出声

1. 在 OpenUtau 里正常建轨、选歌手、写音符；
2. **等渲染完成**（波形出现）。没渲染完的部分传不过去，DAW 里对应位置暂时是静音，属正常；
3. 回 DAW 按播放。声音出现在**和 OpenUtau 相同的时间位置**——音符写在第 10 秒，DAW 的第 10 秒才有声。

### 混音

- 直接用 DAW 的推子、声像、静音、效果器。
- OpenUtau 侧的音量/声像/静音**不影响**传出的信号（pre-fader 设计）：这样你改 OpenUtau 里的任何东西，进入 DAW 效果链的电平都是稳定的。
- 一个参照：DAW 里桥接轨的电平比老版本高约 3 dB，这是统一的输出基准，不是 bug。

### 换轨与多实例

- 每个实例有一个 **`OpenUtau Track` 参数**（DAW 的自动化面板里，显示为 Track 1、Track 2……），决定这个实例播 OpenUtau 的哪条音轨（从 1 数起）。
- **想在 DAW 里用多条 OpenUtau 音轨**：每条 DAW 轨放一个实例，各自设不同的 `OpenUtau Track`。然后在 OpenUtau 的 DAW Integration 列表里把它们**逐个 Connect**（每个实例一行）。
- 改这个参数支持自动化，随 DAW 工程保存。

### 信息小窗（仅 Windows）

双击插件打开。显示：

- **Project**：工程名；未保存时显示 unsaved。在 OpenUtau 里保存后自动更新；
- **连接状态与端口**；
- **Tempo**：当前 DAW 工程速度；
- **Transport**：playing / stopped；
- **音轨列表**：OpenUtau 项目的音轨名，本实例负责的那条前面有 ▸。

窗口只读，不能操作。`OpenUtau Track` 参数仍然在 DAW 的参数面板里调。

### 播放头同步

DAW 播放时，**OpenUtau 窗口里的播放头跟着 DAW 走**（按 OpenUtau 自己的时间轴换算）。暂停后小幅拖动 DAW 播放头，OpenUtau 也会小幅跟随。方向是单向的：OpenUtau 不会反向驱动 DAW。播放期间在 OpenUtau 里手动拖播放头没有意义，会被覆盖。

### 速度（BPM）

两边工程**按绝对时间（秒）对齐**。DAW 的 BPM 和 OpenUtau 工程不一致时，OpenUtau 会提醒一次；对齐小节需要你把两边 BPM 改成一致。播放头同步按时间走，不受 BPM 影响。

### 导出 / Bounce

直接用 DAW 的导出功能即可。插件在离线渲染模式下会等待还没传完的音频，导出结果和实时播放一致。个别极长的工程在导出时可能比纯本地渲染慢一些——音频是边播边传的。

### 保存与恢复

DAW 工程会记住每个实例的 `OpenUtau Track` 设置，但**不保存** OpenUtau 的工程内容。重开 DAW 工程后：打开 OpenUtau、打开对应工程、在 DAW Integration 里重新 Connect 一次即可。

## 5. 出问题了

**日志位置**（反馈必带）：

- Windows：`Win + R` → `%TEMP%\OpenUtau` → `bridge-数字.log`
- macOS / Linux：`/tmp/OpenUtau/bridge-数字.log`

**常见问题速查**：

| 现象 | 先检查 |
|---|---|
| DAW Integration 列表是空的 | 顺序反了没有（必须先 DAW 后 OpenUtau）；插件真的加载成功了吗；`.vst3` 是否只拷了文件没拷文件夹；点 Refresh |
| 列表有但连不上 | 防火墙/杀软是否拦了 127.0.0.1 回环；OpenUtau 工程是否保存过 |
| 连上了没声音 | OpenUtau 里渲染完了吗；DAW 轨是否静音/音量为零；`OpenUtau Track` 指向的轨有音符吗；播放头位置对吗 |
| 声音出现在错误的时间位置 | DAW 工程采样率改动后重播一次；确认不是 DAW 的「时间起点」设置问题 |
| 导出是空白 | 确认插件是 0.2.0；导出前先实时播放过一遍让音频传过去 |
| 播一会儿断开 | 带上日志和 DAW 缓冲区设置反馈 |

更系统的排查见《测试指南》末尾的「连不上怎么办」。

## 6. 明确不支持的

- MIDI 输入（在 DAW 里弹琴/画 MIDI 无效）；
- 小节级（tempo map）同步；
- 把 OpenUtau 界面嵌进 DAW 窗口；
- 跨机器连接（只走本机 127.0.0.1）；
- Pro Tools（AAX 需要 Avid 授权 SDK）。

---

*配套文档：`RELEASE.zh-CN.md`（本版预发布说明）、`TESTING.zh-CN.md`（测试清单与反馈格式）、`PROTOCOL.md`（线路协议，英文）。*
