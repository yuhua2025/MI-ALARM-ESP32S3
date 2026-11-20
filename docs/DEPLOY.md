# 小米闹钟 · 在线烧录发布页（docs/）

纯静态站点，基于 [esp-web-tools](https://github.com/esphome/esp-web-tools)：用户插上 USB-C，在浏览器里点一下就把固件写进 ESP32-S3。没有后端，固件不经过任何服务器中转，全在本地浏览器与设备之间传输。

页面内容全部由 [`config.json`](./config.json) 驱动（站点信息、产品介绍、固件版本、烧录步骤），改内容不用动 HTML。

## 一、开启 GitHub Pages（首次）

本站点放在主仓库的 `docs/` 目录，跟固件源码在一起。

1. 打开仓库 <https://github.com/yuhua2025/MI-ALARM-ESP32S3> → **Settings** → **Pages**。
2. **Build and deployment** → Source 选 **Deploy from a branch**。
3. Branch 选 `main`，目录选 **`/docs`** → Save。
4. 等 1~2 分钟，访问 <https://yuhua2025.github.io/MI-ALARM-ESP32S3/>。

要点：
- **必须 HTTPS**：Web Serial 只在安全上下文可用（localhost 除外），GitHub Pages 自带 HTTPS，满足。
- 换固件要破缓存：改 `config.json` 的 `cacheBust`；改样式要同步改 `index.html` 里 `style.css?v=...` 和图片 URL 的查询串，否则 Pages 会继续发旧文件（约 10 分钟缓存）。
- 若要用自定义域名，在 `docs/` 加一个无扩展名的 `CNAME` 文件写域名，并在 Pages 设置里填同样的域名。本项目目前**没有**绑定自定义域名，用默认的 `yuhua2025.github.io` 即可。

## 二、本地预览

```bash
cd docs
python3 serve.py
# 打开 http://localhost:8000
```

`localhost` 是安全上下文，本地 **http 也能调起串口**烧录（方便真机验证发布页）。线上部署则必须 HTTPS。

手机同 Wi-Fi 测试：`python3 serve.py --https`（自动生成自签名证书，手机需手动信任）。

## 三、目录结构

```
docs/
├── index.html            页面骨架（含 <esp-web-install-button>）
├── config.json           ★ 站点全部内容都在这里改
├── serve.py              本地预览服务器
├── _headers              Netlify / Cloudflare Pages 响应头配置（GitHub Pages 忽略）
├── css/style.css         深色终端风主题
├── js/                   esp-web-tools 完整打包产物（27 个文件，别删/别只留一个）
├── manifests/            固件清单，每个版本一个（ESP32-S3 / offset 0）
├── firmware/             固件镜像本体（.bin）
└── img/                  页面图片（实拍 + SVG 示意）
```

`js/` 里是 esp-web-tools 的完整打包产物，组件内部用 `import()` 动态加载各芯片的 stub flasher，**必须整个目录一起部署**，不能只留 `install-button.js`。

## 四、发布新版本固件（升级步骤）

假设编译出了新版本 `v4.9.0`：

1. **确认镜像干净**：整盘镜像里 NVS 分区（`0x9000` 起 20KB）必须是全 `0xFF`，不能带 Wi-Fi 密码。
   ```bash
   python3 -c "f=open('新固件.bin','rb');f.seek(0x9000);d=f.read(0x5000);print('非FF字节:',sum(1 for b in d if b!=0xFF))"
   # 输出必须是 0
   ```
2. **放进固件目录**：把 16MB 的 merged 镜像拷到 `docs/firmware/`。
3. **算 SHA256**：`shasum -a 256 docs/firmware/新固件.bin`。
4. **写 manifest**：复制 `manifests/mi-alarm_v4_8_12.json` 为 `mi-alarm_v4_9_0.json`，改 `version` 和 `parts[0].path`（指向新 bin，offset 保持 0）。
5. **更新 config.json**：在 `firmwares[0].versions` 数组顶部插入新版本条目（version/date/manifest/bin/size/sha256/latest=true/changes），并把旧版本的 `latest` 去掉或删掉。
6. **破缓存**：把 `config.json` 的 `cacheBust` 改成新日期（如 `2026112002`）。
7. **本地验证**：`python3 serve.py` 打开页面，确认新版本出现在下拉框、SHA256 显示正确、烧录按钮可用。
8. 提交推送后等 Pages 更新。

## 五、常见问题

- **浏览器找不到串口**：换数据线（很多线只能充电）。ESP32-S3 用 XIAO 原生 USB-C 一般免驱动；若用外接 USB-TTL，装 CH340/CP2102 驱动。还不行就按住 BOOT、点一下 RESET、松开 BOOT 强制进下载模式。
- **烧录失败/校验不过**：把 `config.json` 的 `baudRate` 从 921600 依次降到 460800、115200。
- **iOS 能用吗**：不能，iOS 所有浏览器都不支持 Web Serial，必须用桌面 Chrome / Edge / Firefox。
- **烧完要做什么**：设备重启后开 `XiaoMiClock` 热点，手机连上配网；因为发布镜像 NVS 已清空，必须重新连一次 Wi-Fi。
- **能不能保留原配网信息**：技术上有 `app` 镜像可只烧 `0x10000` 保留 NVS，但本页面只发布整盘 merged 镜像（安全起见清空 NVS）。不要对外发布带 Wi-Fi 密码的镜像。

## 六、页面素材来源

- Hero / 设备大图：B 站视频封面 `【万物皆可小智】之小米智能闹钟版-封面.jpg`（压缩到 1200×675）。
- 原装外观：`v2-eb2faabccf5c17443ee37680112b1cd9_720w.gif`（什么值得买水印图，保留来源标注）。
- 数码管 / 功能示意：纯 SVG 手绘（`img/gallery-*.svg`）。
- 视频链接：<https://www.bilibili.com/video/BV1T4sq62EYT>

 Made by Dayu.
