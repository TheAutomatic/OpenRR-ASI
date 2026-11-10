# OptiFG 宿主桥接（实验功能）

让 OptiScaler 的 DLSSD 宿主调用 OpenRR 执行 RR，并复用 Upscaler 输入驱动 OptiFG。
用户已报告鸣潮中 Streamline 启动、超分、RR 和 XeFG 同时生效；不代表所有游戏或 OptiScaler 版本均已验证。

通用发行配置保持 `OptiFGHost=false`，默认使用独立 RR 路径。需要 **Upscaler 输入帧生成** 的用户，按下方说明配置伴生 DLL 后再设为 `true`。此开关在 RR 创建时决定是否进入宿主模式，修改后须重启游戏；在 OptiScaler 菜单关闭 FG 不会自动切回独立 RR 路径。宿主模式增加包装及参数处理，启用 FG 另有插帧开销，目前不承诺相对独立 RR 的性能无损。

## 安装

1. 关闭游戏，备份现有 ASI 和配置。替换原位置的 `openrr-amd.asi`。
2. 移走之前 OpenRR 测试包提供的 `_nvngx.dll`。不要盲目删除游戏或其他插件原有的同名文件。这个名称会影响 OptiScaler 的 NGX 存在性与签名替代流程。
3. 放置配套 `openrr-ngx-host.dll`。推荐记录该文件的完整绝对路径。
4. 在现有 `OptiScaler.ini` 的 `[Libraries]` 节，**直接修改原有的 `NvngxPath = auto` 行**，不要追加第二个同名配置项，也不要重复创建该节。例如：

   ```ini
   [Libraries]
   NvngxPath=D:\Games\Example\OptiScaler\openrr-ngx-host.dll
   ```

   路径仅为示例，必须替换为实际文件位置。若使用相对路径，写作 `NvngxPath=.\openrr-ngx-host.dll`，注意点后面的反斜杠。使用 `OptiScaler` 子目录或自定义 `OptiDllPath` 时优先使用绝对路径，不假定相对路径以 `OptiDllPath` 为基准。
5. 在现有 `openrr-amd.ini` 的 `[OpenRR]` 节设置 `OptiFGHost=true`，重启游戏。保留原有权重及 DLSSD 文件。
6. 使用 `FGInput=upscaler`、`FGOutput=xefg`，保持游戏 RR 开启；其余超分及 Spoofing 配置保持原设置。

## 核对与撤回

OptiScaler 日志应显示从配置路径加载 `openrr-ngx-host.dll`；OpenRR 日志出现 `[openrr-host] EXPERIMENT enabled` 表示桥接已启用。缺失时先检查实际运行目录、文件路径、重复配置项和 DLL 加载记录。

定位或代理校验失败时不启用桥接。底层 RR 创建失败仍可能触发 OptiScaler 自带的普通超分回退。定位器允许位移变化，但代码结构改变时可能拒绝匹配。

撤回时关闭游戏，恢复原 ASI 与配置，移走本次伴生 DLL；不要重新安装旧 OpenRR 测试包的 `_nvngx.dll`。
