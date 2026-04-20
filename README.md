# NativeVideoScrSaver

- 原生 Win32 窗口，按虚拟桌面像素尺寸铺满所有扩展屏
- 标准屏保参数：`/s`、`/c`、`/p`

## 生成

```powershell
.\publish-native-scr.ps1 -VideoPath "D:\test\test.mp4"
```

生成产物位于：

```text
.\publish\test.scr
```