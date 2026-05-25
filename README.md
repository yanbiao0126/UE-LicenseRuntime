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

---

## 技术实现

- 签名算法：`RSA + SHA256`
- 数据加密：`AES-128-ECB`
- OpenSSL 来源：UE 内置第三方库（`Build.cs` 中引入）

License 文件结构（当前实现）：
1. `EncryptedInfo`：固定结构体二进制数据（AES 加密后）
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

### 3) 放置客户端 License

将授权文件放在：

`ProjectDir/app.lic`

> `CheckLicenseValid` 默认读取该路径。  
> 插件已在 `Build.cs` 内配置：打包时会自动将编辑器工程根目录下的 `app.lic` 作为 `NonUFS` 文件拷贝进打包产物。

### 4) 蓝图调用

- 调用 `CheckLicenseValid()` 判断授权是否有效
- 调用 `CreateLicense(User, Expire, Level, Permanent, SavePath)` 生成授权文件

### 5) 打包后文件位置（默认）

以 Windows 打包为例，`app.lic` 会随包输出到项目根目录（与打包后项目目录结构一致），
运行时 `ProjectDir/app.lic` 可直接命中，无需额外手工拷贝。

---

## 蓝图接口说明

### `CheckLicenseValid() -> bool`

- 输入：无
- 默认读取：`ProjectDir/app.lic`
- 返回：
  - `true`：验签通过，且未过期或永久授权
  - `false`：文件不存在、验签失败、解密失败、授权过期等

### `CreateLicense(User, Expire, Level, Permanent, SavePath) -> bool`

- 输入：
  - `User`：用户名
  - `Expire`：到期日（建议 `YYYY-MM-DD`）
  - `Level`：功能等级
  - `Permanent`：是否永久
  - `SavePath`：输出文件路径
- 返回：当前实现固定返回 `true`
- 失败信息：通过日志输出（例如私钥读取失败、签名失败）

---

## 校验流程

1. 读取 `.lic` 文件中的加密授权信息与签名
2. 使用内置公钥执行 `RSA_verify(SHA256(data), signature)`
3. 验签通过后进行 AES 解密
4. 若非永久授权，则与当前日期比较是否过期
5. 结果返回给业务逻辑处理

---

## 接入建议

- 在 `GameInstance` 初始化或登录流程中执行授权校验
- 失败后统一处理（提示、降级、限制功能、退出）
- 将 `FuncLevel` 映射到具体功能开关，形成权限分级

---

## 安全注意事项

- `private.key` 仅用于生成 License，不应下发到客户端
- 当前 AES 密钥与公钥内嵌在代码中，生产环境建议进一步做密钥管理与混淆
- 日期判断依赖字符串格式，建议统一 `YYYY-MM-DD`
- 目前 `CreateLicense` 返回值不代表真实成功状态，建议后续改造为可靠返回

---

## 常见问题

### 1) 校验总是失败

- 检查 `ProjectDir/app.lic` 是否存在
- 检查 License 是否由匹配私钥签发
- 检查系统时间是否正确
- 检查日期格式是否为 `YYYY-MM-DD`

### 2) 无法生成 License

- 检查 `ProjectDir/private.key` 是否存在且 PEM 内容合法
- 查看日志中 `LicenseRuntime` 关键字报错

---

## 可选改进方向

- 增加设备指纹绑定
- 增加许可证吊销列表（黑名单）
- 接入在线校验或周期心跳校验
- 将默认 License 路径改为可配置项

