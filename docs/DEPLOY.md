# 小米闹钟 · 在线烧录发布页（docs/）

纯静态站点，基于 [esp-web-tools](https://github.com/esphome/esp-web-tools)：用户插上 USB-C，在浏览器里点一下就把固件写进 ESP32-S3。没有后端，固件不经过任何服务器中转，全在本地浏览器与设备之间传输。

同一块硬件提供两套固件，页面都能烧录：

| 固件线 | 版本 | 镜像大小 | manifest | 说明 |
| --- | --- | --- | --- | --- |
| 基础版（Arduino 自研） | v4.8.12 | 16.00 MB | `mi-alarm_v4_8_12.json` | 真人报时、闹铃闪灯、网页配网设置、12/24 切换 |
| 小智版（小智 AI） | v1.0.2 | 8.54 MB | `xiaozhi_v1_0_2.json` | 中文语音对话、唤醒表情、亮度语音控制、UDP 调试 |

页面内容全部由 [`config.json`](./config.json) 驱动（站点信息、产品介绍、固件版本、烧录步骤），改内容不用动 HTML。`firmwares` 数组的每一项是一张固件卡片，支持 `default: true` 指定默认选中项。

## 一、开启 GitHub Pages（首次）

本站点放在主仓库的 `docs/` 目录，跟固件源码在一起。

1. 打开仓库 <https://github.com/yuhua2025/MI-ALARM-ESP32S3> → **Settings** → **Pages**。
2. **Build and deployment** → Source 选 **Deploy from a branch**。
3. Branch 选 `main`，目录选 **`/docs`** → Save。
4. 等 1~2 分钟，访问 <https://yuhua2025.github.io/MI-ALARM-ESP32S3/>。

要点：
- **必须 HTTPS**：Web Serial 只在安全上下文可用（localhost 除外），GitHub Pages 自带 HTTPS，满足。
- 换固件要破缓存：改 `config.json` 的 `cacheBust`；改样式要同步改 `index.html` 里 `style.css?v=...` 和图片 URL 的查询串，否则 Pages 会继续发旧文件（约 10 分钟缓存）。
- 自定义域名：在 `docs/` 放一个无扩展名的 `CNAME` 文件写域名，并在 Pages 设置里填同样的域名、勾上 Enforce HTTPS。本项目已绑定 <https://mi-alarm-esp32s3.515166.xyz/>，两个地方保持一致；换域名时两处都要改。

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

`firmware/` 目前有两个 merged 镜像：`XiaoMiClock_v4.8.12_esp32s3_merged.bin`（基础版）和 `xiaozhi_mi-alarm-tm1652_v2.6.0_brightness_merged.bin`（小智版）。两者都是从 `0x0` 开始烧的整盘镜像，分区表不同，**互刷必须擦除 Flash**。

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
4. **写 manifest**：复制对应固件线的模板（基础版用 `manifests/mi-alarm_v4_8_12.json`，小智版用 `manifests/xiaozhi_v1_0_2.json`）为新文件，改 `name`、`version` 和 `parts[0].path`（指向新 bin，offset 保持 0）。
5. **更新 config.json**：基础版改 `firmwares[0]`，小智版改 `firmwares[1]`；在该条的 `versions` 数组顶部插入新版本条目（version/date/manifest/bin/size/sha256/latest=true/changes），并把旧版本的 `latest` 去掉或删掉。
6. **破缓存**：把 `config.json` 的 `cacheBust` 改成新日期（如 `2026112002`）。
7. **本地验证**：`python3 serve.py` 打开页面，确认新版本出现在下拉框、SHA256 显示正确、烧录按钮可用。
8. 提交推送后等 Pages 更新。

## 五、常见问题

- **浏览器找不到串口**：换数据线（很多线只能充电）。ESP32-S3 用 XIAO 原生 USB-C 一般免驱动；若用外接 USB-TTL，装 CH340/CP2102 驱动。还不行就按住 BOOT、点一下 RESET、松开 BOOT 强制进下载模式。
- **烧录失败/校验不过**：把 `config.json` 的 `baudRate` 从 921600 依次降到 460800、115200。
- **iOS 能用吗**：不能，iOS 所有浏览器都不支持 Web Serial，必须用桌面 Chrome / Edge / Firefox。
- **烧完要做什么**：基础版重启后开 `XiaoMiClock` 热点，手机连上配网；小智版按语音提示配网。两个发布镜像的 NVS 都已清空，必须重新连一次 Wi-Fi。
- **能不能保留原配网信息**：技术上有 `app` 镜像可只烧 `0x10000` 保留 NVS，但本页面只发布整盘 merged 镜像（安全起见清空 NVS）。不要对外发布带 Wi-Fi 密码的镜像。

## 六、页面素材来源

- Hero / 设备大图：B 站视频封面 `【万物皆可小智】之小米智能闹钟版-封面.jpg`（压缩到 1200×675）。
- 原装外观：`v2-eb2faabccf5c17443ee37680112b1cd9_720w.gif`（什么值得买水印图，保留来源标注）。
- 数码管 / 功能示意：纯 SVG 手绘（`img/gallery-*.svg`）。
- 视频链接：<https://www.bilibili.com/video/BV1T4sq62EYT>

 Made by Dayu.
