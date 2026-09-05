# OpenUtau DAW 桥接 — 测试指南（Alpha）

感谢帮忙测试。这份东西还很早期，我们需要知道它在真实的 DAW 里会怎么坏。

## 这是什么

装上之后，你在 **OpenUtau 里编辑**，声音直接出现在 **DAW 的时间轴上**，位置和 OpenUtau 里一致。改了音符、调了音量，DAW 里跟着变，不用导出 wav 再拖进去。

**它不是**把 OpenUtau 塞进 DAW 窗口里那种（Synthesizer V 的做法）。OpenUtau 还是独立的窗口，你在那边写歌；DAW 里那个插件只负责把 OpenUtau 渲染好的声音摆到正确的位置上播出来。所以插件**没有编辑器界面**，双击它不会弹出一个写音符的窗口 —— 这是设计如此，不是坏了。这一版起插件带一个**小信息窗**，显示工程名、保存状态、连接状态、BPM、播放状态和音轨列表（见第 16 项），仅此而已。

## 先说清楚

- **这是 alpha 测试版，请不要用在正经作品上。** 测试前把你在做的工程存好、备份好。
- 测试用的 OpenUtau 是**单独一份**，和你平时用的那个互不干扰，你原来的 OpenUtau 不用卸载、不用改动。
- 不需要额外安装 VC++ 运行库之类的东西。

## 你需要什么

**一个能加载「乐器类」插件的 DAW。** 这点很关键，因为这个插件是乐器（instrument），不是效果器（effect）。

可以用的（能加载 VST3 或 CLAP 乐器）：

- REAPER、Waveform、Cakewalk、Studio One、Cubase / Nuendo、FL Studio、Bitwig Studio、Ableton Live、Mixcraft、Samplitude

**用不了的**，别浪费时间：

- **Adobe Audition、Audacity、GoldWave** —— 它们只支持效果器插件，没有乐器轨，插件根本不会出现在列表里
- **macOS 的 Logic Pro、GarageBand** —— 它们跑在沙箱里，很可能扫不到插件（这是已知问题，如果你手上就是这两个，帮我们确认一下现象也算有用）

**系统**：Windows 10/11（64 位）、macOS 11 或更新、主流 Linux 发行版都有对应文件。

**从群文件 / 网盘下载**，按你的系统取两样东西：

| 你的系统 | OpenUtau 测试版 | 插件 |
|---|---|---|
| Windows 64 位 | `OpenUtau-win-x64.zip` | `OpenUtau-Bridge-Windows` |
| macOS（Apple 芯片，M1 及以后） | `OpenUtau-osx-arm64.dmg` | `OpenUtau-Bridge-macOS` |
| macOS（Intel） | `OpenUtau-osx-x64.dmg` | `OpenUtau-Bridge-macOS` |
| Linux 64 位 | `OpenUtau-linux-x64.tar.gz` | `OpenUtau-Bridge-Linux` |

需要 Windows 32 位 / ARM64 版本的话说一声，有构建但没放上来。

<!-- CHUNK2 -->

## 第一步：装这两个东西

### 1. 测试版 OpenUtau

**Windows**：解压 `OpenUtau-win-x64.zip` 到任意目录，**放在和你现有 OpenUtau 不同的文件夹**，比如 `D:\OpenUtau-DAW测试\`。里面的 `OpenUtau.exe` 就是入口。这是免安装的绿色版，不会动你现有的安装。

**macOS**：打开 `.dmg`，把 OpenUtau 拖到「应用程序」。首次打开若提示无法验证开发者，右键 → 打开。

**Linux**：解压 `tar.gz`，运行里面的 `OpenUtau`。

它和你平时那份是完全独立的两份程序。音源、设置这些是共用的，所以你的音源不用重新装。

### 2. 插件

**Windows** —— `OpenUtau-Bridge-Windows` 文件夹里有两个东西，`OpenUtau Bridge.vst3`（一个**文件夹**）和 `OpenUtau Bridge.clap`（一个文件）。装哪个都行，两个都装也行：

- VST3：把 `OpenUtau Bridge.vst3` **整个文件夹**复制到
  `C:\Program Files\Common Files\VST3\`
- CLAP：把 `OpenUtau Bridge.clap` 复制到
  `C:\Program Files\Common Files\CLAP\`（没有这个文件夹就新建一个）

> `.vst3` 是一个文件夹，不是文件。请整个文件夹复制过去，**不要**只把里面那个同名文件拷出来 —— 那样 DAW 会扫不到。
>
> 如果没有管理员权限写 `Program Files`，改用这两个位置也一样有效（在文件管理器地址栏直接粘贴路径就能到）：
> `%LOCALAPPDATA%\Programs\Common\VST3\` 和 `%LOCALAPPDATA%\Programs\Common\CLAP\`

**macOS**：

- VST3 → `~/Library/Audio/Plug-Ins/VST3/`
- CLAP → `~/Library/Audio/Plug-Ins/CLAP/`

首次加载可能被 Gatekeeper 拦（插件没有签名）。如果 DAW 说「无法验证开发者」，在「系统设置 → 隐私与安全性」里点「仍要打开」。

**Linux**：

- VST3 → `~/.vst3/`
- CLAP → `~/.clap/`

装好后，**在 DAW 里重新扫描插件**（各家位置不同，一般在「首选项 → 插件」里有个 rescan / 重新扫描按钮）。

<!-- CHUNK3 -->

## 第二步：第一次连接（顺序很重要）

**必须先开 DAW、后开 OpenUtau。** 插件是被 OpenUtau 找的那一方，它得先跑起来才能被找到。反过来的话列表会是空的。

1. 打开 DAW，新建一个空工程
2. 新建一条**乐器轨 / MIDI 轨**（不是音频轨），在它的乐器位插入 **OpenUtau Bridge**
   - 找不到？搜 "OpenUtau"，厂商是 `KakaruHayate`
3. 打开测试版 `OpenUtau.exe`
4. 菜单栏 **Tools → DAW Integration...**
5. 表格里应该出现一行，`Plugin` 列写着 `OpenUtau Bridge` 加一串数字，`Status` 列是 `Compatible`
6. 选中那一行，点 **Connect**
7. 下方状态文字变成 `Connected to ...` 就成了

如果表格是空的，点一下 **Refresh**。还是空的就看文末「连不上怎么办」。

> 界面目前只有英文（OpenUtau 的翻译走 Crowdin，这个测试分支还没同步）。对照表：
>
> | 界面 | 意思 |
> |---|---|
> | Plugin / Port / API / Status | 插件 / 端口 / 协议版本 / 状态 |
> | Compatible | 版本兼容，可以连 |
> | Refresh / Connect / Disconnect | 刷新 / 连接 / 断开 |
> | No DAW plugin found... | 没找到插件，先在 DAW 里加载插件再刷新 |
> | Connected to ... | 已连接 |
> | Connection lost: ... | 连接断了 |

## 第三步：让声音出来

1. 在 OpenUtau 里正常建音轨、选音源、写几个音符（写长一点，两三秒以上比较好听清）
2. **等它渲染完**（波形出现、进度走完）—— 没渲染完的部分不会传给 DAW，这时候 DAW 里是静音，属正常
3. 回到 DAW，按播放
4. 声音应该出现在**和 OpenUtau 里相同的时间位置**

比如你把音符写在 OpenUtau 工程的第 10 秒，那 DAW 的第 10 秒才有声音，从头播会先听到一段安静。这是对的。

<!-- CHUNK4 -->

## 请帮我们测这几项

能测几条测几条，**每条填一下结果**，最后把这个表发回来就很有价值了。「异常」的那几行麻烦多写两句现象。

| # | 怎么做 | 应该看到 | 正常 / 异常 |
|---|---|---|---|
| 1 | 按上面走完，播放 | 声音出现在正确的时间位置 | |
| 2 | 把 OpenUtau 里的音符整体往后拖 5 秒，等渲染完 | DAW 里的声音也跟着往后 5 秒 | |
| 3 | 改几个音符的音高 / 歌词，等渲染完 | DAW 里播出来是改过之后的 | |
| 4 | 在 OpenUtau 里拉音轨音量推子 | **DAW 里的输入电平不变**；混音音量请用 DAW 推子调 | |
| 5 | 在 OpenUtau 里拉声像（Pan）推子 | **DAW 里的左右声道不变**；声像请用 DAW 控制 | |
| 6 | 在 OpenUtau 里点音轨的 Mute，再点 Solo | **DAW 里的桥接信号仍在**；静音 / 独奏请用 DAW 控制 | |
| 7 | 在 DAW 里改插件的 `OpenUtau Track` 参数（自动化面板里，显示成 Track 1、Track 2…） | 切到 OpenUtau 里对应序号的那条音轨的声音 | |
| 8 | 开循环播放，让它转几圈 | 每圈都正常出声，不会越播越卡或者变静音 | |
| 8b | 播放中**暂停**，让播放头停在有声音的地方 | **立刻安静**，不会嗡嗡地重复同一小段 | |
| 8c | 停止状态下拖动播放头 / 框选一段经过有声音的地方 | **保持安静**，不会一路吱吱作响 | |
| 9 | **导出 / Bounce / Render 成音频文件** | 导出的文件里**有声音**，不是空白 | |
| 10 | 存盘、关掉 DAW、重开工程 | 插件还在，`OpenUtau Track` 参数值还是你设的那个（需要重新连一次 OpenUtau） | |
| 11 | 播放中直接关掉 OpenUtau | DAW 继续正常播放已经传过去的声音，不炸 | |
| 12 | 在 DAW 设置里改缓冲区大小（比如 256 → 1024），再播 | 照常出声，没有爆音、没有静音 | |
| 13 | 在 DAW 设置里改采样率（比如 44100 → 48000），再播 | 照常出声，音高不变（不能变快变慢） | |
| 14 | 连着的时候在 OpenUtau 里新建/删除一条音轨或 part | DAW 那边跟着更新，不崩 | |
| 15 | 在 OpenUtau 里**切换歌手，然后立刻按播放**；还在渲染时再按一次播放。反复几轮 | OpenUtau **不闪退** |
| 16 | 双击 DAW 里的插件（打开它的界面） | 出现一个**小信息窗**：工程名（未保存时显示 unsaved）、连接状态和端口、BPM、播放状态、音轨列表；`OpenUtau Track` 参数指向的那条轨前面有 ▸ 标记 |
| 17 | DAW 里**播放、暂停、拖动播放头**，同时看着 OpenUtau 的窗口 | **OpenUtau 里的播放头跟着 DAW 走**（按 OpenUtau 自己的时间轴换算）；暂停后小幅度拖动，OpenUtau 的播放头也跟着小幅移动 |
| 18 | 在 DAW 里把 BPM 改成和 OpenUtau 工程不同的值（比如 120 → 140），再改回来 | 改成不同值时 **OpenUtau 弹一次**「两边 BPM 不一致」的提示；改回一致后**不再弹** |
| 19 | 在 OpenUtau 里把工程**另存/保存**一次 | 信息窗里的工程名从 unsaved 变成实际文件名 |
| 20 | 新建一个 OpenUtau 工程**不保存**，直接在 DAW Integration 窗口点 Connect | OpenUtau 弹出**友好的提示**（要求先保存工程），不崩溃、不静默失败 | |

**第 8b / 8c 和第 15 是上一版专门修的，请重点验。** 8b/8c 之前会听到一小段音频被反复重播成嗡嗡声；15 之前会让 OpenUtau 直接消失（无报错窗口）。第 15 项需要歌手带 `dsvariance` 模型，并且设置里的 DiffSinger 张量缓存是开着的，否则走不到出问题的那条路。

**第 16 – 20 是这一版（v1.1）新增的能力，是本轮的重点。** 其中第 17（播放头跟随）请在你的 DAW 里多试几种操作顺序：先拖后播、播放中拖、暂停后拖，任何一次 OpenUtau 播放头「卡住不动」或者「跳到错误位置」都请记下来。

**第 9 项是我们最想知道的。** 导出比实时播放快得多，声音是通过网络边播边传的，所以以前导出会得到静音。这一版专门改了这个逻辑，但只在实验室条件下验证过，没在真 DAW 里试过。

**第 13 项也重点看一下。** 声音在 OpenUtau 那边固定是 44100Hz，DAW 用别的采样率时插件要自己转换。如果听起来变调、变速或者有杂音，就是这块有问题。

<!-- CHUNK5 -->

## 已知限制 —— 这些不用报

碰到下面这些是预期行为，不是 bug：

- **信息窗只是一块显示屏。** 上面没有任何能点的控件；写歌、调音、渲染依然全部在 OpenUtau 里做。目前只有 Windows 上有这个窗口，macOS / Linux 的插件双击没有反应。
- **OpenUtau 的播放头会跟着 DAW 走。** 播放期间在 OpenUtau 里手动拖播放头，位置会立刻被 DAW 的位置覆盖。想让 OpenUtau 自己决定位置，先在 DAW 里停止播放。
- **DAW 和 OpenUtau 的 BPM 不一致时只有提示，没有换算。** 两边按绝对时间（秒）对齐，不是按小节；不一致时小节线会对不上，OpenUtau 会提醒你。
- **这一版声音比更早的版本响。** OpenUtau 的推子不再作用于桥接输出，而原来居中声像会衰减 3 dB，所以同一个工程现在会大约响 3 dB。这是预期的，请用 DAW 的推子调平衡。
- **在 DAW 里放多个实例是支持的**（每条轨一个实例，用各自的 `OpenUtau Track` 参数指向不同音轨），但前提是 OpenUtau 测试版和插件都是这一版配套的包。
- **不接受 MIDI 输入。** 在 DAW 里弹琴、画 MIDI 都不会有反应，音符只能在 OpenUtau 里写。
- **没渲染完的部分是静音的。** OpenUtau 那边渲染完才会传过去。
- **DAW 工程不会保存 OpenUtau 的内容。** 重开工程后要重新连一次。
- macOS 的 Logic Pro / GarageBand 大概率扫不到插件（沙箱限制）。
- Linux 上如果是多人共用的机器，可能连不上（临时目录冲突）。

## 出问题了怎么报告

请**一定带上日志文件**，没有日志基本没法查。

**日志在哪**：

- Windows：按 `Win + R`，粘贴 `%TEMP%\OpenUtau` 回车，里面的 `bridge-数字.log`（可能有好几个，都发或者发最新的那个）
- macOS / Linux：`/tmp/OpenUtau/` 下的 `bridge-数字.log`

**一份好的反馈包含**：

1. 那个 `bridge-*.log`
2. DAW 的**名字和版本号**（比如 REAPER 7.28、Waveform 13.1）
3. 系统版本（比如 Win11 23H2 / macOS 15.2 / Ubuntu 24.04）
4. 装的是 VST3 还是 CLAP
5. 你做了什么 → 期望什么 → 实际什么
6. 能录一小段屏幕最好

## 连不上怎么办

**表格是空的 / 提示 No DAW plugin found**

- 顺序反了？必须先在 DAW 里加载插件，再去 OpenUtau 点 Refresh
- 插件真的加载成功了吗？在 DAW 里看那条轨上有没有 OpenUtau Bridge
- `.vst3` 是不是只拷了里面那个文件、没拷整个文件夹
- 杀毒软件 / 防火墙拦了本机回环连接（插件和 OpenUtau 之间走 127.0.0.1 通信）。加白名单试试
- 打开 `%TEMP%\OpenUtau\PluginServers`，插件加载后这里应该有一个 `.json` 文件。没有就是插件那边没起来，看 `bridge-*.log`

**连上了但没声音**

- OpenUtau 里渲染完了吗（有波形吗）
- DAW 那条轨被 mute 了吗、音量推到底了吗、输出路由对吗
- 播放头的位置有音符吗（试试从头播）
- `OpenUtau Track` 参数是不是指向了一条空音轨
- OpenUtau 里那条音轨自己被 mute 或者被别的轨 solo 掉了吗

**播一会儿就断 / 有杂音**

- 把日志和 DAW 缓冲区设置一起发来

---

再次感谢。这东西目前在三个平台的自动测试里都是全绿的，但自动测试测不到「真实 DAW 怎么驱动插件」这一层，只能靠你们。
