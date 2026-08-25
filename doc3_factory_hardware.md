# Архитектура нового железа и ПО (RV1126B, factory)

> Документ описывает плату factory (RV1126B, заводская прошивка rpdzkj)
> и архитектуру порта percomedia/s30gui на эту плату.
> Старая архитектура: `doc1_old_architecture.md`.
> Old s30gui архитектура + порт notes: `K:\s30gui_rv1126b\ARCHITECTURE.md`.

---

## 1. Железо

### 1.1 Плата

| Параметр | Значение |
|----------|----------|
| Model | `rp-rv1126bp` |
| Compatible | `rockchip,rv1126b` |
| SoC | RV1126B |
| Архитектура | aarch64 (64-bit) |
| OS | Ubuntu 24.04.4 LTS |
| Kernel | 6.1.141 #506 SMP Wed May 13 20:12:29 CST 2026 |
| Hostname | `ubuntu2404` |
| RAM | 2 GB DDR4 @ 1056 MHz |
| eMMC | 7.28 GB (mmcblk0, 8 partitions) |
| NPU | RKNPU driver v0.9.8, 600 MHz |
| Дисплей | 720×1280 MIPI0 (DSI-1) |

### 1.2 eMMC разделы

```
mmcblk0p1  4M    (loader1)
mmcblk0p2  4M    (loader2)
mmcblk0p3  64M   (trust/parameter)
mmcblk0p4  128M  (boot)
mmcblk0p5  32M   (misc)
mmcblk0p6  6G    / (rootfs, Ubuntu)
mmcblk0p7  128M  (oem)
mmcblk0p8  944M  (userdata)
```

### 1.3 Подключение

**SSH (Windows):**
```
ssh factory          # → root@10.0.61.227 (ключ без пароля)
```

**UART (опциональный, плавающий — CP2102, 115200 baud):**
- TX→RX, RX→TX, GND→GND (кросс!)
- Скорость: **115200** (НЕ стандарт Rockchip 1500000)
- Debug UART = неподписанные штырьки RX TX GND (UART2/fiq_debugger)
- Подключается к любой плате по необходимости

**Login:** root / пароль `rpdzkj` (ключ добавлен 2026-08-10)

### 1.4 Сеть

**LAN (eth0):** статический
- `/etc/systemd/network/20-ethernet.network`
- IP: 10.0.61.227/8

> WiFi (AP6256) есть аппаратно, но **не используется** — работаем через LAN/OTG/UART.

---

## 2. Камеры

### 2.1 Физически подключены (проверено dmesg)

**Две камеры GC2093** (GalaxyCore), обе 1920×1080@30:

| | Cam 0 | Cam 1 |
|---|---|---|
| Сенсор | GC2093 | GC2093 |
| i2c | bus1, 0x37 | bus1, 0x7e |
| DT node | `gc2093@37` | `gc2093@7e` |
| DT status | (нет → okay) | `okay` |
| Facing | back | back |
| Module | default | default |
| Lens | default | default |
| DPHY | csi2-dphy0 | csi2-dphy3 |
| CSI2 | mipi0-csi2 | mipi2-csi2 |
| RKCIF | rkcif-mipi-lvds | rkcif-mipi-lvds2 |
| RKISP | rkisp-vir0 | rkisp-vir1 |
| Разрешение | 1920×1080@30 | 1920×1080@30 |

**ВНИМАНИЕ:** На factory обе камеры **GC2093** (одинаковые сенсоры),
в отличие от old где **GC2053 (IR) + SC200AI (RGB)** (разные сенсоры).
Различие RGB/IR на factory — **только по линзе/фильтру**, не по сенсору.

### 2.2 Device Tree сенсоры (всего 8 в DT, 2 активны)

```
i2c@21110000/:
  gc2093@37     ← Cam 0 (активна, Detected GC2093)
  gc2093@7e     ← Cam 1 (активна, Detected GC2093)
  sc200ai@30    ← не активна (Error applying setting)
  sc200ai-2@30  ← не активна
  sc450ai@30    ← не активна
  sc450ai-2@30  ← не активна
  sc850sl@30    ← не активна
  sc850sl-2@30  ← не активна
  imx415@1a     ← не активна
  imx415-2@1a   ← не активна
  imx464-0@1a   ← не активна
  imx464-2@1a   ← не активна
  os04a10-0@36  ← не активна
  os04a10-2@36  ← не активна
```

Драйверы загружены для всех: `gc2093`, `sc200ai`, `sc450ai`, `sc850sl`, `imx415`, `imx464`, `os04a10`.
Но физически обнаружены только 2× GC2093.

### 2.3 Media pipeline

```
gc2093@37 → csi2-dphy0 → mipi0-csi2 → rkcif-mipi-lvds → rkisp-vir0 → rkvpss-vir0
gc2093@7e → csi2-dphy3 → mipi2-csi2 → rkcif-mipi-lvds2 → rkisp-vir1 → rkvpss-vir1
```

Два независимых ISP pipeline (vir0 + vir1), каждый со своим VPSS.
Это позволяет обрабатывать две камеры параллельно.

### 2.4 Известная проблема

`rkcif-mipi-lvds: ERROR: size err, intstat:0x1000200, size:0x0,0x0,0x0,0x0`
— периодические ошибки размера кадра от CIF. Не критично, кадры идут.

### 2.5 Сравнение с old

| | Old (RV1109/1126) | Factory (RV1126B) |
|---|---|---|
| Cam 0 | GC2053 (IR, i2c 0x37) | GC2093 (i2c 0x37) |
| Cam 1 | SC200AI (RGB, i2c 0x32) | GC2093 (i2c 0x7e) |
| Сенсоры | **разные** | **одинаковые** |
| Разрешение | 1920×1080 | 1920×1080 |
| Линзы | 40IR-2MP-F20 (IR), 30IRC-2MP-F20 (RGB) | default/default |
| ISP | rkisp (v2.x) | rkisp (v3.01.00) |
| CIF | rkcif (v4.19) | rkcif (v00.02.00) |
| VPSS | нет | rkvpss-vir0/vir1 (новое!) |

---

## 3. Дисплей

| Параметр | Значение |
|----------|----------|
| Интерфейс | DSI-1 (MIPI0) |
| Разрешение | 720×1280 (portrait) |
| Status | connected |
| DRM device | /dev/dri/card0 |

### 3.1 DRM planes (проверено 2026-08-07)

```
plane[73]: NV12,  720×1280  ← видео (overlay, zpos=0)
plane[58]: AR24,  720×1280  ← UI (primary, zpos=1, прозрачный)
```

ZPOS на new VOP **меняется** (atomic commit).
percomedia_stub ставит: overlay zpos=0 (низ), primary zpos=1 (верх).

### 3.2 Сравнение с old

| | Old | Factory |
|---|---|---|
| Разрешение | 800×1280 | 720×1280 |
| Video plane | RG24 (RGB888, opaque) | NV12 → XR24 (XRGB8888) |
| UI plane | AR24 (ARGB8888) | AR24 (ARGB8888) |
| Plane owner (video) | `OutputStreamFlo` (rkmedia VO) | `percomedia_stub` (DrmConf) |

---

## 4. Библиотеки на плате

### 4.1 Медиа/NN библиотеки

| Библиотека | Путь | Размер | Дата | Версия |
|------------|------|--------|------|--------|
| librockit.so | /usr/lib/ | 1460 KB | 2025-05-09 | git 8a3e1bfd4 |
| librknnrt.so | /usr/lib/ | 7726 KB | 2025-05-09 | rknpu2 v2.3.2 |
| librga.so | /usr/lib/ | 197 KB | 2025-05-09 | |
| librkaiq.so | /usr/lib/ | 2041 KB | 2025-05-29 | uAPI2 |
| librockchip_mpp.so | /usr/lib/aarch64-linux-gnu/ | | | |
| libdrm.so | /usr/lib/aarch64-linux-gnu/ | | | 2.125.0 |

**MD5 librockit.so:** `67dff89819d7cef610f367a1e9cf0042`

### 4.2 Заголовки на плате

| Компонент | Путь | Что есть |
|-----------|------|----------|
| rockit (rk_mpi) | /usr/include/rockchip/ | rk_mpi.h, rk_mpi_cmd.h, mpp_*.h, vpu*.h |
| RGA | /usr/include/rga/ | im2d.h, im2d.hpp, im2d_mpi.h, rga.h, и др. |
| rkaiq | /usr/include/rkaiq/uAPI2/ | rk_aiq_user_api2_*.h (uAPI2) |
| RKNN | **НЕТ** на плате | есть в SDK: `K:\sdk\external\rknpu2\runtime\Linux\librknn_api\include\` |

### 4.3 Сравнение API old vs new

| Компонент | Old (RV1109/1126) | Factory (RV1126B) |
|-----------|-------------------|-------------------|
| Media API | rkmedia (RK_MPI_*) | rockit (rk_mpi_*) |
| RGA | imfill макрос, imsync(void) | imfill_t, imsync(int) |
| rkaiq | v1.0 direct API | uAPI2 |
| NN | rockx v1.1.0 (librockx.so) | RKNN v2 (librknnrt.so) / ROCKIVA |
| NPU runtime | librknn_runtime.so + librknn_api.so (v1.x) | librknnrt.so (v2.x, объединено) |
| Qt | 5.9.4 (armhf) | 5.15.11 (aarch64) |

### 4.4 AVS (закрытая тема)

AVS stitch **не работает** на factory:
- `librockit.so` требует `librkALG_avsCore.so` даже для NOBLEND
- Не содержит строк `jsonPath` → `rkAVS_readJson fail`
- Замена librockit.so на rock-версию → kernel incompatibility
- **Вывод:** AVS на factory — закрытая тема (см. `K:\sdk\AGENTS.md`)

---

## 5. Заводские сервисы (demo-прошивка rpdzkj, НЕ PERCo)

### 5.1 Запущенные сервисы

| Сервис | Описание |
|--------|----------|
| `rkaiq_3A.service` | Rockchip Auto Image Quality (3A) |
| `mobilenet.service` | Нейросеть (mobilenet demo) |
| `adbd` (pid 617) | ADB на порту 5555 |

### 5.2 Заводские бинарники

| Файл | Описание |
|------|----------|
| `/usr/bin/rknn_common_test` | RKNN test |
| `/usr/bin/rknn_server` | NPU server |
| `/usr/share/model/RV1126B/mobilenet_v1.rknn` | Тестовая модель (4.7 MB, ImageNet) |

### 5.3 Заводское видео-демо (было ранее)

`rp_demo_vi_vo_rtsp -W 720 -H 1280` — 2 камеры → VO → дисплей + RTSP (порт 554).
Линкован: librga, librknnrt, librockit, libdrm (НЕ Qt).
(Может быть не запущен на текущей прошивке.)

---

## 6. SDK на PC (`K:\sdk\external\*`)

### 6.1 Структура

```
K:\sdk\external\
├── rockit/mpi/sdk/include/    — rk_mpi_*.h (замена rkmedia)
├── rockit/lib/arm64/rv1126b/  — librockit.so, librockit.a
├── linux-rga/im2d_api/        — im2d_*.h (новая версия, imsync(int))
├── linux-rga/include/         — rga.h, drmrga.h
├── camera_engine_rkaiq/rkaiq/include/ — rkaiq uAPI2
├── rknpu2/                    — RKNN v2 (замена rockx)
│   ├── runtime/Linux/librknn_api/include/  — rknn_api.h, rknn_custom_op.h, rknn_matmul_api.h
│   ├── runtime/Linux/librknn_api/aarch64/  — librknnrt.so (7.7 MB)
│   ├── runtime/Linux/rknn_server/aarch64/  — rknn_server
│   ├── doc/                   — PDF документация (CN+EN)
│   └── examples/              — demo (yolov5, android)
├── iva/                       — ROCKIVA (альтернатива RKNN C API)
│   └── librockiva/rockiva-rv1126b-Linux/
│       ├── include/           — rockiva_face_api.h, det_api.h, ba_api.h, one_api.h
│       └── lib64/             — librockiva.so (666 KB), librknnrt.so (идентичен rknpu2)
├── avs/                       — AVS stitch (не работает на factory)
└── iva/                       — IVA
```

### 6.2 RKNN v2 API

**Заголовки:** `K:\sdk\external\rknpu2\runtime\Linux\librknn_api\include\rknn_api.h`

**API:** `rknn_init`, `rknn_query`, `rknn_inputs_set`, `rknn_run`, `rknn_outputs_get`,
`rknn_outputs_release`, `rknn_destroy`, `rknn_create_mem`, `rknn_set_io_mem`

**Несовместимость:** rknpu v1.x (old) и rknpu2 v2.x (new) — разные API, разные модели.

### 6.3 ROCKIVA (высокоуровневая обёртка)

**Заголовки:** `K:\sdk\external\iva\librockiva\rockiva-rv1126b-Linux\include\`
- `rockiva_face_api.h` — face detection, recognition, liveness, features
- `rockiva_det_api.h` — general object detection
- `rockiva_ba_api.h` — behavior analysis (tracking)
- `rockiva_image.h` — image utilities
- `rockiva_one_api.h` — unified API

**Плюс:** готовые face detection + recognition + liveness, меньше кода.
**Минус:** тяжёлая абстракция, модели нужно искать, меньше контроля.

### 6.4 Конвертация моделей (RKNN)

Конвертация выполняется на `conv` (см. `K:\sdk\AGENTS.md` → «Компьютер конвертации моделей»):
- Docker образ `rknn-toolkit2:2.3.2`

**Готовая модель:**
```
RetinaFace_mobile320.onnx
    ↓ rknn-toolkit2 v2.3.2, INT8, target=rv1126b
RetinaFace_mobile320_rv1126b.rknn
```
- Файлы: `T:\rknn_convert\` (ONNX, convert_rv1126b.py, model/dataset.txt, model/test.jpg)
- Запуск: `ssh conv 'docker run --rm -v ~/rknn-docker:/workspace rknn-toolkit2:2.3.2 python /workspace/convert_rv1126b.py'`
- Модель: 65 слоёв, 3 головы детекции (Bbox/Class/Landmark), 320×320 вход

### 6.5 EASY-EAI Toolkit (альтернативный SDK)

**Путь:** `T:\easy_eai` (git clone https://github.com/EASY-EAI/EASY-EAI-Toolkit-1126B.git)
**Версия:** 0.2.2 (2026-08-05)

Готовые обёртки над rockit/RGA/DRM/RKNN для RV1126B:
- `algorithm/face_detect/` — libface_detect.a (RetinaFace-like, 5 landmarks)
- `algorithm/face_recognition/` — libface_recognition.a (512-dim feature)
- `media/display/` — libdisplay.a (DRM overlay+UI, КАК НАШ DrmConf!)
- `media/lmo_adapter/` — liblmo_adapter.a (rockit wrapper)
- `Solutions/avs/` — AVS stitch (готовое, но AVS не работает на factory)

**display API** — ровно то что мы делали вручную в DrmConf:
- `disp_init()` / `uiLayer_init()` — overlay + UI layers
- `window_commit(ptr, w, h, rotation)` — вывод BGR888 на overlay
- `uiLayer_commit(ptr, w, h)` — вывод BGR888 на UI layer
- `display_pro.h` — zero-copy (DMA-BUF), до 16 окон на layer

---

## 7. Архитектура порта (percomedia_stub)

### 7.1 Что сделано

**percomedia_stub_aarch64.cpp** — заглушка с реальным видеовыводом:
- VI: ОБЕ камеры (dev 0 + dev 1), rockit API
- RGA: однопроходный `improcess` (NV12→BGRA + rotate + scale)
- Вывод: DRM overlay plane (прямой DRM, не VO)
- Камеры: **Cam 1 (dev 1) = цветная**, Cam 0 = ЧБ
- `display_ch=1` в `/home/percomedia.json`
- Без bind-схемы (нет RGA0/RGA1/RGA2/RGA3, один improcess)

**s30gui_stub** — собирается с stub, UI/WebSocket/настройки работают.
`DISABLE_S30_CHECK` — S30 не требуется, режим CONTROL по умолчанию.

### 7.2 Отличия от old

| | Old | Factory (stub) |
|---|---|---|
| Media API | rkmedia (RK_MPI_*) | rockit (rk_mpi_*) |
| Bind VI→RGA→VO | да | **нет** (rockit не поддерживает bind) |
| RGA каналы | 4 (RGA0-3) | 1 (однопроходный improcess) |
| Вывод видео | rkmedia VO | DrmConf (прямой DRM atomic commit) |
| Оверлей рамки | rectangle_mb + imcomposite | TBD |
| NN каналы | RGA1 (RGB) + RGA2 (IR) | TBD (когда будет ROCKIVA) |
| VENC | мёртвый код | не портируется |

### 7.3 DRM pipeline (new)

```
1. Камера → NV12 (rockit VI)
2. RGA: NV12 → BGRA_8888 (improcess, rotate+scale+convert)
3. [TBD] imfill + imcomposite — рамки (когда будет ROCKIVA)
4. DRM overlay: XRGB8888 (opaque) — рамки будут запечены
5. Qt UI: AR24 (ARGB8888, прозрачный) — через QT_QPA_KMS_CONFIG
```

### 7.4 DrmConf на new — ПОЛНЫЙ вывод буферов

На new DrmConf **расширен** — добавлены `display_show`, `display_get_fd`,
`display_get_pitch`, `display_get_buf` (которых нет в old drmconf.cpp).
Это вынужденная мера: rockit VO конфликтует с Qt linuxfb DRM.

| | Old | New |
|---|---|---|
| DrmConf::display_init | ZPOS only | ZPOS + dumb buffers |
| DrmConf::display_show | **нет** | atomic commit буфера на overlay |
| Вывод видео | rkmedia VO | DrmConf (`display_show`) |
| Plane owner | `OutputStreamFlo` (rkmedia) | `percomedia_stub` (DrmConf) |

### 7.5 Почему DrmConf вместо rockit VO

Точное соответствие `VO_LAYER` → DRM plane на RV1126B пока не установлено.
DrmConf гарантированно позволяет занять overlay plane (73), зафиксировать zpos=0
и сосуществовать с Qt на primary. Это инженерный выбор, не доказанная невозможность.

**Абстракция изменилась:**
- rkmedia: `OVERLAY` / `PRIMARY` / `CURSOR`
- rockit: `CLUSTER0..3` / `ESMART0..3` / `SMART0..1` / `VIRTUAL0..3`

---

## 8. NPU инвентаризация

### 8.1 Железо

| | Old (RV1109/1126) | Factory (RV1126B) |
|---|---|---|
| NPU compatible | `rockchip,npu` | `rockchip,rv1126b-rknpu` |
| NPU freq | TBD | 600 MHz |
| Архитектура | armhf (32-bit) | aarch64 (64-bit) |
| Driver | TBD | RKNPU v0.9.8 |

### 8.2 RKNN runtime

| | Old | Factory |
|---|---|---|
| Библиотека | librknn_runtime.so (3.2 MB) | librknnrt.so (7.7 MB) |
| API обёртка | librknn_api.so (5.7 KB) | (встроена в librknnrt.so) |
| Версия SDK | rknpu v1.x | rknpu2 v2.3.2 |
| Путь | /usr/lib/librknn_runtime.so | /usr/lib/librknnrt.so |
| Тест | TBD | rknn_common_test + mobilenet_v1.rknn — **работает** |

### 8.3 Что нужно для NPU инференса

**Минимум (RKNN C API напрямую) — ВСЁ ЕСТЬ:**
1. `rknn_api.h` — `K:\sdk\external\rknpu2\runtime\Linux\librknn_api\include\`
2. `librknnrt.so` (aarch64) — на плате + в SDK
3. `.rknn` модель — `RetinaFace_mobile320_rv1126b.rknn` (конвертирована, 925 KB)
4. Кросс-тулчейн aarch64 — `D:\dev\aarch64-none-linux\bin\` (Arm GNU 15.2.1)
5. Документация — `K:\sdk\external\rknpu2\doc\` (PDF, CN+EN)

**Для face recognition:**
6. Модель face recognition в .rknn — TBD (найти или конвертировать)
7. Модель face liveness — TBD

---

## 9. Сборка под factory

### 9.1 Инструменты

| Инструмент | Путь |
|------------|------|
| Кросс-компилятор | `D:\dev\aarch64-none-linux\bin\aarch64-none-linux-gnu-g++.exe` (Arm GNU 15.2.1) |
| Sysroot тулчейна | `D:\dev\aarch64-none-linux\aarch64-none-linux-gnu\libc\` |
| CMake | `C:\Users\pvv\scoop\shims\cmake.exe` (3.30.5) |
| Make | `D:\dev\mingw64\bin\mingw32-make.exe` |
| Toolchain file | `K:\rv1126b_temp\aarch64-toolchain.cmake` |
| Qt qmake | `K:\sdk\5.9.4\mingw53_32\bin\qmake.exe` (тот же, build system only) |
| Qt headers | `K:\cross_sysroot_aarch64\usr\include\qt5\` (5.15.11) |
| Qt libs | `K:\cross_sysroot_aarch64\usr\lib\libQt5*.so.5.15.11` |

### 9.2 CMake сборка (тесты)

```powershell
$env:PATH = "C:\Users\pvv\scoop\shims;D:\dev\aarch64-none-linux\bin;D:\dev\mingw64\bin;C:\Windows\System32;C:\Windows"
cd K:\rv1126b_temp
Remove-Item build_test -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory build_test | Out-Null
cd build_test
cmake -G "MinGW Makefiles" -DSDK_PATH="K:/sdk" -DCMAKE_TOOLCHAIN_FILE="../aarch64-toolchain.cmake" ..
cmake --build . -j4
```

### 9.3 Деплой на factory

```powershell
scp file factory:/tmp/
ssh factory 'cd /tmp; ...'
```

---

## 10. Статус порта

### 10.1 Работает

- **percomedia_stub** — VI + RGA + DRM overlay (видео с камеры на дисплей)
- **s30gui_stub** — UI/WebSocket/настройки (с заглушкой percomedia)
- **Заглушка S30** — `s30_stub.js` (Node.js WebSocket клиент)
- **RKNN конвертация** — RetinaFace_mobile320 → .rknn (INT8, 925 KB)
- **Тулчейн** — Arm GNU 15.2.1, CMake, кросс-сборка
- **VPSS passthrough** — VI→VPSS→RGA→VO (тест в rv1126b_temp)
- **AVS stitch** — работает на rock (не на factory!)

### 10.2 TBD

- **Замена rockx → ROCKIVA/rknn v2** — заголовки есть, модели нужны
- **Замена rkmedia → rockit** — в реальном percomedia (stub уже использует)
- **Прямоугольники лиц** — draw_rects в overlay buffer (TBD)
- **RGA каналы для NN** — когда будет ROCKIVA
- **IR LED управление** — PWM или GPIO (на old мёртвый код)
- **Бизнес-логика S30** — замок, проходы, настройки
- **Qt 5.15.11** — кросс-сборка с aarch64 sysroot

---

## 11. Ключевые отличия old → factory (кратко)

| | Old (RV1109/1126) | Factory (RV1126B) |
|---|---|---|
| Архитектура | armhf (32-bit) | aarch64 (64-bit) |
| OS | buildroot | Ubuntu 24.04.4 LTS |
| Kernel | 4.19 | 6.1.141 |
| RAM | TBD | 2 GB DDR4 |
| Дисплей | 800×1280 | 720×1280 |
| Камеры | GC2053 (IR) + SC200AI (RGB) | 2× GC2093 (одинаковые) |
| Media API | rkmedia | rockit |
| RGA | imfill макрос, imsync(void) | imfill_t, imsync(int) |
| rkaiq | v1.0 direct | uAPI2 |
| NN | rockx v1.1.0 | RKNN v2 / ROCKIVA |
| NPU | rknpu v1.x | rknpu2 v2.3.2 (v0.9.8 driver) |
| Qt | 5.9.4 | 5.15.11 |
| VENC | мёртвый код | не портируется |
| VPSS | нет | есть (rkvpss-vir0/vir1) |
| AVS | нет | не работает (закрыто) |
| Вывод видео | rkmedia VO | DrmConf (прямой DRM) |
| Bind схема | VI→RGA→VO | нет bind, improcess + DRM |
