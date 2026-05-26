# LicenseRuntime

`LicenseRuntime` 是一个 Unreal Engine 运行时授权校验插件，用于：
- 生成离线 License 文件（签名 + 加密）
- 在客户端运行时校验 License 合法性与有效期
- 通过蓝图接口快速接入项目逻辑

---

## 基本信息

- 插件名：`LicenseRuntime`
- 模块类型：`Runtime`
- 加载时机：`PostConfigInit`
- 当前版本：`1.0`
- 目标平台：`Win64`、`Linux`（`WhitelistPlatforms`）

---

## 功能概览

- 校验默认路径 `ProjectDir/app.lic` 是否有效
- 生成自定义路径 License 文件
- 支持的授权字段：
  - `Username`：用户名
  - `ExpireDate`：过期日期（`YYYY-MM-DD`）
  - `FuncLevel`：功能等级
  - `bPermanent`：是否永久授权
  - `ProjectId`：项目绑定ID（GUID，防跨项目复用）

---

## 技术实现

- 签名算法：`RSA + SHA256`
- 数据加密：`AES-128-ECB`
- OpenSSL 来源：UE 内置第三方库（`Build.cs` 中引入）
- 纯离线防回拨：`可信时间状态文件 + 单调时钟漂移检测`

License 文件结构（当前实现）：
1. `EncryptedInfo`：固定结构体二进制数据（AES 加密后，V3 内含 `ExpireUnixUtc` + `ProjectId`）
2. `Signature`：256 字节 RSA 签名

---

## 目录结构

```text
Plugins/LicenseRuntime
├─ LicenseRuntime.uplugin
├─ Source/LicenseRuntime
│  ├─ LicenseRuntime.Build.cs
│  ├─ Public
│  │  ├─ LicenseRuntime.h
│  │  ├─ LicenseTool.h
│  │  └─ LicenseBPLibrary.h
│  └─ Private
│     ├─ LicenseRuntime.cpp
│     ├─ LicenseTool.cpp
│     └─ LicenseBPLibrary.cpp
├─ Resources
└─ README.md
```

---

## 依赖要求

- Unreal Engine 4.27（按当前工程环境）
- C++ 模块依赖：
  - `Core`
  - `CoreUObject`
  - `Engine`
  - `Slate`
  - `SlateCore`

---

## 快速开始

### 1) 启用插件

确认插件位于：

`<YourProject>/Plugins/LicenseRuntime`

并在工程中启用。

### 2) 准备私钥（仅生成 License 时需要）

将 RSA 私钥放在项目根目录：

`ProjectDir/private.key`

> `CreateLicense` 会从该路径读取私钥进行签名。  
> 出于安全考虑，`CreateLicense` 仅在编辑器环境可用（`WITH_EDITOR`）。

### 3) 配置项目绑定ID（必须）

在项目 `Config/DefaultGame.ini` 中添加：

```ini
[/Script/LicenseRuntime.LicenseRuntimeSettings]
ProjectId="{01234567-89AB-CDEF-0123-456789ABCDEF}"
```

说明：
- `ProjectId` 必须是合法 GUID（推荐带中划线格式）
- 生成与校验都会读取该配置；缺失或非法会直接失败
- 同一 License 仅能在 `ProjectId` 一致的项目内使用

### 4) 放置客户端 License

将授权文件放在：

`ProjectDir/app.lic`

> `CheckLicenseValid` 默认读取该路径。  
> 插件已在 `Build.cs` 内配置：打包时会自动将编辑器工程根目录下的 `app.lic` 作为 `NonUFS` 文件拷贝进打包产物。

### 5) 蓝图调用

- 调用 `CheckLicenseValid()` 判断授权是否有效
- 调用 `CreateLicense(User, Expire, Level, Permanent, SavePath)` 生成授权文件

### 6) 打包后文件位置（默认）

以 Windows 打包为例，`app.lic` 会随包输出到项目根目录（与打包后项目目录结构一致），
运行时 `ProjectDir/app.lic` 可直接命中，无需额外手工拷贝。

---

## 蓝图接口说明

### `CheckLicenseValid() -> bool`

- 输入：无
- 默认读取：`ProjectDir/app.lic`
- 返回：
  - `true`：验签通过、项目ID匹配、且未过期或永久授权
  - `false`：文件不存在、验签失败、解密失败、项目ID不匹配、授权过期等

### `CreateLicense(User, Expire, Level, Permanent, SavePath) -> bool`

- 输入：
  - `User`：用户名
  - `Expire`：到期日（建议 `YYYY-MM-DD`）
  - `Level`：功能等级
  - `Permanent`：是否永久
  - `SavePath`：输出文件路径
- 限制：仅编辑器环境可调用；非编辑器环境会直接返回 `false`
- 返回：`true` 表示生成成功，`false` 表示生成失败
- 失败信息：通过日志输出（例如私钥读取失败、签名失败）

### 使用示例（对应 `ULicenseBPLibrary`）

#### 示例 1：运行时校验（蓝图）

在 `GameInstance` 的 `Event Init` 中：

1. 调用 `CheckLicenseValid()`
2. 分支判断返回值：
   - `true`：进入正常流程（进入主菜单/关卡）
   - `false`：提示“授权无效或已过期”，并退出或进入受限模式

#### 示例 2：生成 License（蓝图）

在内部工具蓝图（如 Editor Utility Widget）中调用（编辑器环境）：

- `CreateLicense("TestUser", "2026-12-31", 2, false, "D:/Licenses/app.lic")`

参数建议：
- `Expire` 使用 `YYYY-MM-DD`
- `Permanent = true` 时表示永久授权（仍建议保留标准日期字符串）
- `SavePath` 使用绝对路径，避免写入到不可预期目录

---

## 校验流程

1. 读取 `.lic` 文件中的加密授权信息与签名
2. 使用内置公钥执行 `RSA_verify(SHA256(data), signature)`
3. 验签通过后进行 AES 解密
4. 校验 `License.ProjectId` 与当前项目配置 `ProjectId` 是否一致
5. 执行离线防回拨检测（多副本可信时间状态 + 单调时钟漂移检测）
6. 若非永久授权，则使用 UTC 时间戳判定是否过期
7. 结果返回给业务逻辑处理

---

## 接入建议

- 在 `GameInstance` 初始化或登录流程中执行授权校验
- 失败后统一处理（提示、降级、限制功能、退出）
- 将 `FuncLevel` 映射到具体功能开关，形成权限分级

---

## 安全注意事项

- `private.key` 仅用于生成 License，不应下发到客户端
- 当前 AES 密钥与公钥内嵌在代码中，生产环境建议进一步做密钥管理与混淆
- 纯离线模式下，插件会在多个路径保存可信时间状态副本（`ProjectSavedDir`、`ProjectPersistentDownloadDir`、`UserSettingsDir`）；任一副本缺失/损坏和墙钟/单调时钟大漂移都会计入异常次数，系统时间明显回拨、License 或状态文件修改时间异常会直接拒绝校验
- 日期输入建议统一 `YYYY-MM-DD`（生成时会转换为 UTC 时间戳用于过期判定）
- 项目绑定ID（`ProjectId`）必须在 `DefaultGame.ini` 配置且格式合法（GUID）

---

## 常见问题

### 1) 校验总是失败

- 检查 `ProjectDir/app.lic` 是否存在
- 检查 License 是否由匹配私钥签发
- 检查系统时间是否正确
- 检查日期格式是否为 `YYYY-MM-DD`
- 当前版本已升级为 V3（含 `ProjectId` 绑定），旧 V2 License 会被强制拒绝，请重新签发

### 2) 无法生成 License

- 检查 `ProjectDir/private.key` 是否存在且 PEM 内容合法
- 查看日志中 `LicenseRuntime` 关键字报错

---

## 项目ID绑定验证清单

可按以下场景逐项执行回归：

1) 正向验证（同项目）  
- 前置：`DefaultGame.ini` 已配置合法 `ProjectId`，并在该项目内调用 `CreateLicense` 生成 `app.lic`  
- 执行：运行 `CheckLicenseValid()`  
- 预期：返回 `true`

2) 跨项目复用拦截  
- 前置：将上一步生成的 `app.lic` 复制到另一个 `ProjectId` 不同的项目  
- 执行：运行 `CheckLicenseValid()`  
- 预期：返回 `false`，日志包含“项目ID不匹配，拒绝复用授权”

3) 配置缺失/非法拦截  
- 前置：删除或写错 `ProjectId`（非GUID）  
- 执行：分别调用 `CreateLicense` 与 `CheckLicenseValid()`  
- 预期：均返回 `false`，日志提示“缺少项目ID配置”或“项目ID格式非法”

4) 旧版授权淘汰  
- 前置：准备历史 V2 格式 `app.lic`  
- 执行：运行 `CheckLicenseValid()`  
- 预期：返回 `false`，日志提示“旧版V2授权文件，已强制失效”

---

## 可选改进方向

- 增加设备指纹绑定
- 增加许可证吊销列表（黑名单）
- 接入在线校验或周期心跳校验
- 将默认 License 路径改为可配置项
