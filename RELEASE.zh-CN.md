# OpenUtau Bridge 发布说明

**版本 1.0.0 · 协议 v1.2 · 2026-09-07**

OpenUtau Bridge 的第一个正式版。写给直接使用它的人；安装与日常使用见《使用说明书》（`MANUAL.zh-CN.md`，英文 `MANUAL.md`）。协议层的技术细节见 `PROTOCOL.md`（英文）。

---

## 这是什么

一个把 OpenUtau 渲染好的声音直接摆到 DAW 时间轴上的插件。你在 **OpenUtau 里编辑**（写音符、换歌手、调渲染），声音实时出现在 **DAW 的乐器轨上**，位置和 OpenUtau 里一致——不用导出 wav 再拖。

它**不是**把 OpenUtau 塞进 DAW 窗口的那种（Synthesizer V / ARA 的做法）。OpenUtau 始终是独立窗口；DAW 里的插件只负责「连接、接收音频、按位置摆放」。所以它没有编辑器界面——插件自带一个只读的**信息小窗**和**换轨下拉框**（Windows / macOS），双击插件即可打开。

## 包内容

每个平台的包里是同一套东西：

| 平台 | 文件 |
|---|---|
| Windows 64 位 | `OpenUtau Bridge.vst3`（文件夹）+ `OpenUtau Bridge.clap`（文件） |
| macOS（Apple 芯片 / Intel） | 同上两种格式 |
| Linux 64 位 | 同上两种格式 |

> **必须与带 DAW Integration 功能的 OpenUtau 成对使用**（`OpenUtau-win-x64.zip` 等对应平台的发行包）。插件的协议是 v1.2，并与 v1.1 主程序兼容；不带 DAW Integration 的旧版 OpenUtau 连不上。

## 核心功能

- **实时同步**：OpenUtau 里的编辑（音符、歌词、歌手、渲染）自动更新到 DAW。
- **多实例多轨**：每条 DAW 轨一个实例，`OpenUtau Track` 参数（或插件窗口里的下拉框）各管 OpenUtau 的一条音轨，支持 DAW 自动化。
- **插件窗口**（Windows / macOS）：轨道下拉框换轨、连接状态与端口、工程名与保存状态、当前音轨的歌手与渲染引擎、BPM、播放状态。固定暗色配色。
- **播放头同步**：DAW 的播放头单向驱动 OpenUtau——播放状态变化立即同步，播放中约 100ms 一次，停止时拖动超过 50ms 才同步。
- **BPM 不匹配提醒**：两边工程速度差 0.5 BPM 以上时，OpenUtau 对每种不一致提醒一次。
- **Pre-fader 输出**：OpenUtau 的音量/声像/静音不影响桥接信号，混音完全交给 DAW。
- **导出 / Bounce 支持**：离线渲染等待缺失的音频，导出与实时播放一致。
- **采样率自适应**：线路上固定 44.1kHz，插件自动转换到 DAW 的采样率。

## 相对 0.2.0 Alpha 的变化

- **插件窗口扩展到 macOS**（原生 AppKit 实现，与 Windows 同一套信息行 + 换轨下拉框）；Linux 暂无插件窗口，切轨走 DAW 的通用参数面板。
- **窗口可靠性**：宿主重建 UI / 销毁插件窗口后自动恢复；换轨下拉框不再与自动化或用户操作互相覆盖。
- **信息行拆分**：歌手与渲染引擎各占一行（协议 v1.2 的 `singer` / `engine` 字段），换轨或换歌手后自动更新。
- **协议升级到 v1.2**：新增 `singer` / `engine` 元数据字段；主版本不变，对 v1.1 的 OpenUtau 自动降级兼容，未知消息安全忽略。

## 已知问题与限制

- **插件窗口仅 Windows / macOS。** Linux 上双击插件暂时无界面，功能不受影响。
- **OpenUtau 播放头被 DAW 驱动。** 播放期间在 OpenUtau 里手动拖播放头会被立刻覆盖，属预期行为。
- **两边按秒对齐，不按小节。** BPM 不一致时只有提醒，没有换算。
- **未保存的 OpenUtau 工程连不上。** 会收到「请先保存工程」的提示，这是刻意的保护（未保存工程无法生成可靠的音频路径）。
- **DAW 工程不保存 OpenUtau 内容。** 重开 DAW 工程后要重新连一次 OpenUtau。
- **不接受 MIDI 输入。** 音符只能在 OpenUtau 里写。
- macOS 的 Logic Pro / GarageBand 大概率扫不到插件（沙箱限制）。
- **macOS 插件为 ad-hoc 签名**（项目没有付费开发者证书）：Apple 芯片上若 DAW（如 Cubase 15）报「The VST signature is invalid」，按《使用说明书》第 2 节的两条命令清除隔离属性并本地重签即可加载；Logic/GarageBand 因沙箱即使重签也大概率不行。彻底解决需要 Developer ID 签名 + 公证（Apple 开发者计划）。

## 质量验证

- 插件自带测试 125/125 通过（协议、并发、混音、resample 全链路，真实 socket）。
- GitHub CI 三平台（Windows x64 / macOS arm64 / Linux x64）构建 + 测试 + Steinberg VST3 validator 全绿。

## 反馈

到仓库的 [Issues](https://github.com/KakaruHayate/openutau-vst-bridge/issues) 提交，**必带**：日志文件（Windows 在 `Win + R` → `%TEMP%\OpenUtau` 下的 `bridge-数字.log`；macOS / Linux 在 `/tmp/OpenUtau/`）、DAW 名称与版本、系统版本、VST3 还是 CLAP、复现步骤。

## 许可

插件本体 MPL-2.0，依赖均为宽松许可（详见 `README.md`）。配套发行的 OpenUtau 主程序遵循其自身许可。
