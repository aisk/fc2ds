# fc2ds

<img src="https://pbs.twimg.com/media/HWnUrsGbEAAsKuO?format=png&name=large" alt="在 melonDS 上运行的移植版" align="right" width="40%">

[English](README.md) | 中文

把 FC/NES 游戏逆向出来，在 Nintendo DS 上原生重写。

本项目仅供学习，主要面向自制游戏等非商业作品。如果要逆向商业游戏，前提是你持有该游戏的合法副本，并且不要把逆向的成果发布到网上。用本项目的工具逆向游戏所产生的任何侵权行为，都与本仓库无关，责任由实际操作的人承担。

仓库里没有任何商业游戏的内容，也没有主机厂商的专有代码（不含 BIOS、固件和官方 SDK，NDS 部分用开源的 BlocksDS 工具链构建），只有工具、运行时和文档这些通用的处理代码。它本身不会逆向任何东西，把游戏的汇编改写成 C 这类实际的逆向工作，都需要由进行移植的人亲自参与完成。

## 为什么是 NDS

NDS 每块屏幕横向 256 像素，和 FC 一样，纵向 192 行，比 FC 在电视上显示的 224 行左右稍矮一些。而绝大部分 FC 游戏都会在画面顶部或底部留一条状态栏，显示血量、金钱之类的信息。把它挪到另一块屏幕上，剩下的游戏画面正好放进上屏，显示效果几乎完美。

下屏还可以由移植版自己重画：直接读取游戏内存里的状态，用原版图标重新画出状态栏，再加上 FC 屏幕放不下的内容，比如关卡地图、Boss 血量、多语言翻译等（`common/nds/subscreen`）。点一下也能切回原版状态栏。

## 原理

它不是模拟器。游戏的 6502 代码会被逐个函数反编译成等价的 C，再编译成 ARM 原生代码。`make verify` 会在电脑上让移植版和原版同时运行，逐帧比对 RAM、PPU 和 APU 的状态。

不过 C 代码依然是在操作 NES 的硬件，所以项目里有一个精简的虚拟 NES（`common/vnes`），负责 PPU（相当于显卡）、APU（相当于声卡）和 mapper：

- **画面**交给 NDS 的 2D 硬件来画，不靠软件逐像素渲染。NES 的图块、nametable、精灵和调色板，分别对应到 NDS 的图块、BG 地图、OBJ 和调色板 RAM。帧中途改卷动的那些效果，变成用 HBlank DMA 逐行设置卷动。所以不受每行 8 个精灵的限制，也不会闪烁。
- **声音**在 ARM9 上用软件模拟 2A03 合成，再通过 maxmod 播放。

更多细节见 `docs/design.md`。

## 移植一个游戏

这个项目不能一键逆向。仓库里的工具负责追踪反汇编、逐帧验证和运行，但读懂汇编、翻译成 C 这些工作，需要人工完成，或者配合 Claude Code 这类 LLM 编程助手来完成。仓库已经为此做好了准备：`CLAUDE.md` 和 `.claude/skills/` 里的 skill 会引导它走完整个流程。

开始的方法：

1. 按 `docs/setup.md` 装好工具链。
2. 把你的 ROM 放进 `roms/` 目录。
3. 在仓库根目录打开 Claude Code（或者其他支持 skill 的编程助手，skill 在 `.claude/skills/` 里），输入 `/nes-port`。
4. 确认你持有这个游戏的合法副本，并且移植只用于自己学习。之后它会创建 `games/<name>`，按 `docs/porting-guide.md` 的阶段一步步往下做，每一步都用 `make verify` 验证。以后再输入 `/nes-port` 就能接着做。

想手工进行的话，用 `python3 common/tools/newgame.py roms/game.nes <name>` 创建游戏，各个阶段的做法见 `docs/porting-guide.md`。

## 游戏与版权

欢迎把自由许可的游戏（比如许可证允许的自制游戏）的移植放到本仓库的 `games/<name>` 下。商业游戏的移植属于原作的衍生作品，请放在你自己的私有仓库里：`games/` 默认被忽略，`newgame.py` 也会把每个新游戏初始化成独立的 git 仓库（加 `--here` 则留在本仓库）。ROM 一律不提交。

也欢迎给通用部分贡献代码，比如支持更多 mapper。

## TODO

- [x] Mapper：MMC3（mapper 4）
- [ ] Mapper：NROM（mapper 0）
- [ ] Mapper：MMC1（mapper 1）
- [ ] Mapper：UxROM（mapper 2）
- [ ] Mapper：CNROM（mapper 3）
- [ ] 支持使用 CHR RAM 的游戏
- [ ] 声音：DPCM 通道
- [ ] 声音：方波和噪音改用 NDS 自带的 PSG 通道（ARM7）发声，不再占用 ARM9 做软件合成
- [ ] 画面：把屏幕右边缘外的那一列补画出来，不再裁掉 8 px
- [ ] 画面：有些游戏靠精灵闪烁来做半透明，改成稳定的 alpha 混合
- [ ] 画面：校准调色板，做平滑的淡入淡出
- [ ] 下屏：把血条、图标行、关卡地图这些 HUD 部件做成共用的
- [ ] 找一个自由许可证的自制游戏，做一个公开的移植样例

## 许可证

[MPL-2.0](LICENSE)。修改本仓库的文件需要以同样的许可证开源，基于它做的游戏则可以用任何许可证。许可证只适用于本仓库，不涉及用它移植的游戏本身。
