# TiRTC macOS ARM64 SDK 使用说明

本 SDK 提供两种接入方式：

1. 静态接入：编译期链接 `lib/libTiRTC.a`
2. 动态接入：编译期链接 `lib/libTiRTC.dylib`

无论使用哪种方式，都需要在运行时随程序一起分发 `lib/libtgrtc.dylib`。

## 包内容

- `include/tirtc/tiRTC.h`：TiRTC 对外 API 头文件
- `include/tirtc/basedef.h`：TiRTC 基础类型定义
- `lib/libTiRTC.a`：TiRTC 静态库
- `lib/libTiRTC.dylib`：TiRTC 动态库
- `lib/libtgrtc.dylib`：运行时依赖库

## 如何使用

业务代码只应包含 `include/tirtc/tiRTC.h`，并链接 `libTiRTC.a` 或 `libTiRTC.dylib`。

不要把 `libtgrtc.dylib` 当作 TiRTC SDK 的入口库使用。它是运行时依赖库，会由 SDK 内部按需加载。

## 运行时库摆放位置

如果使用 `libTiRTC.a` 静态接入，`libtgrtc.dylib` 需要放在主程序可执行文件旁边，确保运行时能通过 `@loader_path/libtgrtc.dylib` 找到它。

示例：

```text
MyApp.app/Contents/MacOS/MyApp
MyApp.app/Contents/MacOS/libtgrtc.dylib
```

如果使用 `libTiRTC.dylib` 动态接入，建议把 `libTiRTC.dylib` 和 `libtgrtc.dylib` 放在同一个运行时库目录，并确保应用能找到 `libTiRTC.dylib`。

示例：

```text
MyApp.app/Contents/Frameworks/libTiRTC.dylib
MyApp.app/Contents/Frameworks/libtgrtc.dylib
```

这种情况下，应用通常需要配置运行时搜索路径，例如 `@executable_path/../Frameworks`。

如果运行时报找不到 `libtgrtc.dylib`，请优先检查它是否被打进应用包，以及是否放在运行时可解析的位置。
