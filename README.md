# RV1126B Dual Camera SDK

Отфильтрованный SDK Rockchip RV1126B с поддержкой **двухкамерного отображения** (dual camera display). Репозиторий содержит только необходимую часть SDK — `app/` и `external/rockit/` — без тяжёлой истории и kernel/uboot.

## Источник

- **Origin SDK:** `rv1126b-linux6.1` (Rockchip RV1126B, ядро Linux 6.1)
- **Базовый коммит:** `7285d5c2f` ("Fix(4g): EC20 LTE automatic connection at startup")
- **Содержимое:** `app/` (ipcweb-backend, lvgl_demo, rkadk, rkipc) + `external/rockit/`

## Поддерживаемые платформы

| Чип | Архитектура | Конфиги rkipc |
|-----|-------------|---------------|
| **RV1126B** | arm64 | `rv1126b_ipc`, `rv1126b_dv`, `rv1126b_dual_ipc` |
| RV1126 | arm32 | `rv1126_ipc_rockit`, `rv1126_aiisp_ipc`, `rv1126_battery_ipc`, и др. |
| RV1106 / RV1103B | arm32 | `rv1106_ipc`, `rv1106_dual_ipc`, и др. |
| RK3588 / RK3576 | arm64 | `rk3588_ipc`, `rk3576_ipc` |

---

## Dual Camera Display

Главная доработка — **одновременный вывод двух камер** на один VO-слой через RGA-композитор. Каждая камера получает свой `vo_chn` и прямоугольник на дисплее.

### Архитектура пайплайна

```
Cam 0 (цветная) ─→ VI ─→ VPSS ─→ VO (layer 0, chn 0) ─→ окно (x₀, y₀, W, H)
Cam 1 (серая)   ─→ VI ─→ VPSS ─→ VO (layer 0, chn 1) ─→ окно (x₁, y₁, W, H)
```

Обе камеры идут через один `vo_layer=0`, но разные `vo_chn` (0 и 1). `splice_mode=RGA` включает аппаратный композитор Rockchip RGA, который смешивает каналы в один кадр.

### Раскладки экрана

#### RV1106 / RV1103B (дисплей 320×480, ландшафт)

```
┌──────────┬──────────┐
│          │          │
│  Cam 0   │  Cam 1   │
│ 256×N    │ 256×N    │
│ x=0      │ x=264    │
│ vo_chn=0 │ vo_chn=1 │
│          │          │
└──────────┴──────────┘
  0        256  264   480
```

- Sensor 0: `x=0`, `vo_chn=0`, `width=320` (полная ширина, RGA обрезает)
- Sensor 1: `x=320`, `vo_chn=1`, `width=320`

#### RV1126B (дисплей 1080×1920, портрет)

```
┌──────────┬──────────┐
│          │          │
│          │          │
│  Cam 0   │  Cam 1   │
│ 540×1920 │ 540×1920 │
│ x=0      │ x=540    │
│ vo_chn=0 │ vo_chn=1 │
│          │          │
│          │          │
└──────────┴──────────┘
  0       540       1080
```

- Sensor 0: `x=0`, `width=540`, `vo_chn=0`
- Sensor 1: `x=540`, `width=540`, `vo_chn=1`

---

## Изменённые файлы

### C-код (общий для всех платформ)

| Файл | Изменение |
|------|-----------|
| `app/rkadk/src/display/rkadk_disp.c` | `stDispHandle` → массив `stDispHandle[RKADK_MAX_SENSOR_CNT]` с индексацией по `u32CamId`. Каждая камера получает независимый handle (bInit, tid, bSendBuffer, u32CamId). |
| `app/rkadk/examples/CMakeLists.txt` | Добавлена цель сборки `rkadk_dual_disp_test` |
| `app/rkadk/examples/rkadk_dual_disp_test.c` | **Новый файл** — test app для запуска двух камер |

### INI-конфиги RV1106

| Файл | Изменение |
|------|-----------|
| `app/rkadk/inicfg/rv1106_1103/rkadk_setting_sensor_1.ini` | `x: 0→320`, `vo_chn: 0→1` |
| `app/rkadk/inicfg/rv1106_1103/rkadk_defsetting_sensor_1.ini` | `x: 0→320`, `vo_chn: 0→1` |

### INI-конфиги RV1126B

| Файл | Изменение |
|------|-----------|
| `app/rkadk/inicfg/rv1126b/rkadk_setting_sensor_0.ini` | `width: 1080→540` |
| `app/rkadk/inicfg/rv1126b/rkadk_defsetting_sensor_0.ini` | `width: 1080→540` |
| `app/rkadk/inicfg/rv1126b/rkadk_setting_sensor_1.ini` | `x: 0→540`, `width: 1080→540`, `vo_chn: 0→1` |
| `app/rkadk/inicfg/rv1126b/rkadk_defsetting_sensor_1.ini` | `x: 0→540`, `width: 1080→540`, `vo_chn: 0→1` |

### Патч-файлы (в корне репо)

| Файл | Описание |
|------|----------|
| `dual_camera_patch.diff` | Патч для RV1106 (C-код + ini + CMakeLists) |
| `dual_camera_patch_rv1126b.diff` | Патч ini-конфигов для RV1126B |
| `dual_camera_patch_README.md` | Оригинальное описание патча RV1106 |
| `apply_patch.sh` | Скрипт применения патча |

---

## Ключевое изменение: массив хэндлов по CamId

**До** — один глобальный handle на все камеры:

```c
static RKADK_DISP_HANDLE_S stDispHandle = {
    .bInit = false, .u32CamId = 0, .bSendBuffer = false, .tid = 0};
```

**После** — массив по количеству сенсоров, каждая камера независима:

```c
static RKADK_DISP_HANDLE_S stDispHandle[RKADK_MAX_SENSOR_CNT] = {
    [0 ... RKADK_MAX_SENSOR_CNT - 1] = {
        .bInit = false, .u32CamId = 0, .bSendBuffer = false, .tid = 0}};
```

Все обращения `stDispHandle.bInit` → `stDispHandle[u32CamId].bInit` в `RKADK_DISP_Init()` и `RKADK_DISP_DeInit()`. Это позволяет вызывать `RKADK_DISP_Init(0)` и `RKADK_DISP_Init(1)` независимо — каждая камера создаёт свой VPSS, VO-канал и поток.

### Особенность для RV1126B

Код в `#if defined(RV1106_1103) || defined(RV1103B)` (поток `RKADK_DISP_GetVpssMb`) **не компилируется** для RV1126B. RV1126B использует путь `RKADK_MPI_SYS_Bind` (VPSS→VO через системный bind), а не ручную пересылку кадров в потоке. Патч корректно обходит это — изменения `tid`/`bSendBuffer` применяются только к RV1106, а массив хэндлов и проверки `bInit` — общие.

---

## Почему `[display]` находится в файле сенсора?

Файл `rkadk_setting_sensor_0.ini` — это **не "файл сенсора"**, а **файл всего медиа-пайплайна для камеры 0** (CamId=0). Один ini = одна камера = один полный пайплайн от матрицы до всех выходов.

### Структура файла

```ini
[sensor]      # матрица (2688×1520, ISP, flip/mirror)
[vi.0]        # VI канал 0 → RECORD_MAIN (2688×1520)
[vi.1]        # VI канал 1 → RECORD_SUB/PREVIEW/LIVE (1280×720)
[vi.2]        # VI канал 2 → DISP (1920×1080)         ← для дисплея
[vi.3]        # VI канал 3 → THUMB (256×176)
[record.0]    # запись основной поток (H.264, 2688×1520)
[record.1]    # запись субпоток (H.264, 1280×720)
[photo]       # фото (JPEG, 2688×1520)
[preview]     # превью-стрим (H.264, 1280×720)
[live]        # лайв-стрим (H.264, 1280×720)
[display]     # вывод на экран (VO)                   ← вот он
[thumb]       # миниатюры (JPEG, 256×176)
```

### Логика

Камера — это не только сенсор. Это **полный пайплайн**:

```
Сенсор → VI (4 канала) → VPSS → {запись, фото, превью, дисплей, миниатюры}
```

`[display]` — это один из выходов пайплайна камеры, наряду с `[record]`, `[photo]`, `[preview]`, `[thumb]`. Каждый выход забирает кадр из VPSS и направляет его в свой модуль (VENC для записи, VO для дисплея, и т.д.).

Связь между секциями:

```ini
[vi.2]
module = DISP                          # VI канал 2 → для дисплея

[display]
vpss_grp = 1                           # VPSS группа 1
vpss_chn = 1                           # VPSS канал 1 → забирает кадр для VO
vo_chn   = 0                           # VO канал 0 → прямоугольник на экране
```

`[display]` описывает **как именно кадр этой конкретной камеры попадает на экран**: через какой VPSS, в какой VO-канал, в какой прямоугольник.

### Для двух камер — два файла

```
rkadk_setting_sensor_0.ini   → CamId=0 → vo_chn=0, x=0,     width=540
rkadk_setting_sensor_1.ini   → CamId=1 → vo_chn=1, x=540,   width=540
```

Каждая камера имеет свой собственный display-конфиг, потому что каждая выводится в свой прямоугольник на экране через свой VO-канал. Именно поэтому для dual camera пришлось править display-секцию в **обоих** файлах — у каждой камеры свой прямоугольник.

### Почему не разделили на отдельные файлы?

Можно было бы разделить: `sensor.ini` (матрица) + `display.ini` (экран) + `record.ini` (запись). Но Rockchip выбрала модель **"один ini на CamId"** — всё про одну камеру в одном файле. Это удобно: добавил новую камеру → создал один файл, скопировал, поменял `dev_id`, `vo_chn`, `vpss_grp`. Не нужно править 5 разных файлов.

Так что `[display]` в файле сенсора — это не баг, а архитектура: **файл описывает всю камеру целиком**, от матрицы до экрана.

---

## Архитектура: где живёт конфигурационное программирование?

Система конфигурационного программирования пайплайна — **полностью в открытом коде rkadk**. Закрытая библиотека `librockit.so` ничего не знает про ini-файлы.

### Три слоя

```
┌─────────────────────────────────────────────────────┐
│  INI-файлы (rkadk_setting_sensor_0.ini)             │  ← конфигурация
│  [sensor] [vi.0] [display] [record] ...              │
├─────────────────────────────────────────────────────┤
│  rkadk (открытый C-код, ~4638 строк)                 │  ← парсинг + оркестрация
│                                                      │
│  rkadk_param.c          → RKADK_Ini2Struct()         │  читает ini → C-структуры
│  rkadk_struct2ini.c     → iniparser_load()           │  generic ini↔struct mapper
│  rkadk_param_map.h      → g_stDispCfgMapTable[]      │  "display:width" → offset в struct
│  rkadk_disp.c           → RKADK_DISP_Init()          │  берёт struct → вызывает MPI
│  rkadk_media_comm.c     → RKADK_MPI_VO_Init()        │  обёртка над MPI
├─────────────────────────────────────────────────────┤
│  librockit.so / librockit.a (ЗАКРЫТАЯ, ~1.4 МБ)      │  ← реализация MPI
│                                                      │
│  RK_MPI_VO_SetChnAttr()   → драйвер VO               │
│  RK_MPI_VPSS_Init()       → драйвер VPSS             │
│  RK_MPI_SYS_Bind()        → системный bind            │
│  RK_MPI_VI_Init()         → драйвер VI                │
└─────────────────────────────────────────────────────┘
```

### Как это работает по шагам

**1. ini → C-структуры** (открытый код, `rkadk_param.c:1791`)

```c
ret = RKADK_Ini2Struct(sensorPath[i], &pstCfg->stMediaCfg[i].stDispCfg,
                       pstMapTableCfg->pstMapTable,
                       pstMapTableCfg->u32TableLen);
```

`RKADK_Ini2Struct` (`rkadk_struct2ini.c:54`) — generic парсер. Берёт map-таблицу:

```c
// rkadk_param_map.h:344
static RKADK_SI_CONFIG_MAP_S g_stDispCfgMapTable[] = {
    DEFINE_MAP(display, tagRKADK_PARAM_DISP_CFG_S, int_e, x),
    DEFINE_MAP(display, tagRKADK_PARAM_DISP_CFG_S, int_e, y),
    DEFINE_MAP(display, tagRKADK_PARAM_DISP_CFG_S, int_e, width),
    DEFINE_MAP(display, tagRKADK_PARAM_DISP_CFG_S, int_e, height),
    ...
    DEFINE_MAP(display, tagRKADK_PARAM_DISP_CFG_S, int_e, vo_chn),
};
```

`DEFINE_MAP` генерирует запись: секция `"display"`, поле `"width"`, тип `int`, смещение `offsetof(tagRKADK_PARAM_DISP_CFG_S, width)`. Парсер читает `display:width = 540` из ini и пишет `540` по этому смещению в структуру. Чистый reflection через offset — никаких хардкод-парсеров на каждое поле.

**2. C-структуры → MPI вызовы** (открытый код, `rkadk_disp.c:65-68`)

```c
stChnAttr.stRect.s32X = pstDispCfg->x;           // из ini
stChnAttr.stRect.s32Y = pstDispCfg->y;
stChnAttr.stRect.u32Width = pstDispCfg->width;    // 540
stChnAttr.stRect.u32Height = pstDispCfg->height;  // 1920
```

Потом:

```c
// rkadk_media_comm.c:1406 — обёртка rkadk
ret = RK_MPI_VO_SetChnAttr(s32VoLay, s32VoChn, pstChnAttr);
ret = RK_MPI_VO_EnableChn(s32VoLay, s32VoChn);
```

**3. MPI → железо** (закрытая `librockit.so`)

`RK_MPI_VO_SetChnAttr` — это **только декларация** в `rk_mpi_vo.h`. Реализация — внутри `librockit.so`. Библиотека общается с kernel-драйверами VO/VPSS/VI через ioctl. Она не знает про ini — она получает готовые C-структуры (`VO_CHN_ATTR_S`).

### Что где находится

| Компонент | Где | Что делает |
|-----------|-----|------------|
| **ini-файлы** | открыто | Описание пайплайна |
| **iniparser** | открыто (`src/third-party/iniparser/`) | Generic ini-парсер (сторонняя библиотека) |
| **RKADK_Ini2Struct** | открыто (`rkadk_struct2ini.c`) | Reflection: ini → C-struct через map-таблицы |
| **Map-таблицы** | открыто (`rkadk_param_map.h`) | `"display:width"` → `offsetof(struct, width)` |
| **rkadk_param.c** | открыто (4638 строк) | Загрузка/сохранение/проверка всех конфигов |
| **rkadk_disp.c** | открыто | Оркестрация: берёт struct → вызывает MPI |
| **rkadk_media_comm.c** | открыто | Обёртки `RKADK_MPI_*` над `RK_MPI_*` |
| **RK_MPI_VO_\*** | **закрыто** (`librockit.so`) | Реализация: ioctl к kernel-драйверам |
| **RK_MPI_VPSS_\*** | **закрыто** | То же |
| **RK_MPI_SYS_Bind** | **закрыто** | То же |

### Аналогия

- **librockit.so** — это как **GStreamer daemon**: управляет hardware-блоками (VI, VPSS, VO, VENC), но через C API (MPI), не через pipeline-описание.
- **rkadk** — это как **GStreamer pipeline builder**: читает ini-описание пайплайна и вызывает MPI-функции чтобы его построить.
- **ini-файлы** — это как **GStreamer launch string**: декларативное описание графа.

Можно сказать, что rkadk — это **тонкий оркестратор над MPI**. Вся "магия" конфигурационного программирования (ini→struct→MPI) — в открытом коде. Закрытая библиотека — это просто hardware abstraction layer, она не знает ничего про конфигурацию.

---

## Что позволяет система? NPU, RGA и кастомная обработка

Через rkadk ini-конфигурацию **нельзя** взять кадр и отправить в NPU. Но в SDK есть **два разных способа** работы с медиа-пайплайном, и один из них это умеет.

### Подход 1: rkadk + MPI (то, что мы патчили) — "низкоуровневый"

MPI предоставляет **только hardware-блоки Rockchip**:

| Модуль | Что делает |
|--------|-----------|
| VI | Захват с сенсора |
| VPSS | Масштабирование (через RGA/ISP) |
| VO | Вывод на дисплей |
| VENC | Кодирование H.264/H.265/JPEG |
| VDEC | Декодирование |
| AI/AO | Аудио вход/выход |
| AENC/ADEC | Аудио кодирование |
| RGN | Регионы (OSD-оверлеи) |
| GDC | Геометрическая коррекция (fisheye) |
| VGS | Video Graphics Subsystem |
| TDE | 2D graphics engine |
| AVS | Auto Video Stitching |
| DIS | Digital Image Stabilization |
| PVS | Picture Video Sync |

**NPU здесь нет.** MPI работает с hardware-блоками SoC, а NPU — это отдельный процессор со своим API (`librknn.so`, не входит в rockit).

#### Но! Можно вытащить кадр вручную (zero-copy)

MPI позволяет получить кадр из пайплайна как `MB_BLK` (memory block):

```c
VIDEO_FRAME_INFO_S stFrame;
// Получить кадр из VPSS
RK_MPI_VPSS_GetChnFrame(grp, chn, &stFrame, timeout);

// Получить виртуальный адрес данных
void *data = RK_MPI_MB_Handle2VirAddr(stFrame.stVFrame.pMbBlk);
// или физический адрес (для DMA в NPU)
RK_U64 phys = RK_MPI_MB_Handle2PhysAddr(stFrame.stVFrame.pMbBlk);
// или fd (для DMA-BUF import в NPU)
RK_S32 fd = RK_MPI_MB_Handle2Fd(stFrame.stVFrame.pMbBlk);

// ... здесь передаёшь data/fd в rknn_api ...

// Вернуть кадр
RK_MPI_VPSS_ReleaseChnFrame(grp, chn, &stFrame);
```

Это **не через ini** — это в C-коде. Вы прерываете пайплайн, забираете кадр, обрабатываете, возвращаете. Но это **zero-copy**: кадр лежит в DMA-памяти, NPU может работать с физическим адресом или dmabuf fd напрямую.

#### Реальный пример из rkipc (RV1126B)

В `app/rkipc/src/rv1126b_ipc/` этот паттерн уже работает — rkipc получает кадры из VI и отправляет их в NPU через RockIva. Это рабочий код, который можно взять за основу.

**1. Инициализация VI канала для NPU** (`video.c:783-807`):

```c
#define VIDEO_PIPE_0 0   // основной поток (VENC)
#define VIDEO_PIPE_1 1   // второй поток (VENC)
#define VIDEO_PIPE_2 2   // JPEG
int g_vi_for_npu_ivs_id = 4;   // отдельный VI канал для NPU/IVS

VI_CHN_ATTR_S vi_chn_attr;
memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
vi_chn_attr.stIspOpt.u32BufCount = 3;                    // 2 + 1 (ping-pong для NPU)
vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
vi_chn_attr.stIspOpt.stMaxSize.u32Width  = 960;
vi_chn_attr.stIspOpt.stMaxSize.u32Height = 540;
vi_chn_attr.stSize.u32Width  = 2560;                     // видео.2:width
vi_chn_attr.stSize.u32Height = 1440;                     // видео.2:height
vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;             // NV12
vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
vi_chn_attr.u32Depth = 1;                                // +1 буфер для NPU

RK_MPI_VI_SetChnAttr(pipe_id_, g_vi_for_npu_ivs_id, &vi_chn_attr);
RK_MPI_VI_EnableChn(pipe_id_, g_vi_for_npu_ivs_id);
```

**2. Поток получения кадра и отправки в NPU** (`video.c:1410-1447`):

```c
static void *rkipc_get_vi_2_send(void *arg) {
    int ret;
    int32_t loopCount = 0;
    VIDEO_FRAME_INFO_S stViFrame;
    int npu_fps = rk_param_get_int("video.source:npu_fps", 10);
    int npu_cycle_time_ms = 1000 / npu_fps;

    while (g_video_run_) {
        long long before_time = rkipc_get_curren_time_ms();

        // 1. Получить кадр из VI (канал 4, отдельный от VENC)
        if (!enable_fec)
            ret = RK_MPI_VI_GetChnFrame(pipe_id_, g_vi_for_npu_ivs_id, &stViFrame, 1000);
        else
            ret = RK_MPI_VPSS_GetChnFrame(0, 2, &stViFrame, 1000);  // через FEC

        if (ret == RK_SUCCESS) {
            void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);

            // 2. Отправить в NPU через RockIva (zero-copy, через dmabuf fd)
            //    1126b 32bit rga only support fd
            rkipc_rockiva_write_nv12_frame_by_fd(
                stViFrame.stVFrame.u32Width,
                stViFrame.stVFrame.u32Height,
                loopCount,
                RK_MPI_MB_Handle2Fd(stViFrame.stVFrame.pMbBlk)  // ← DMA-BUF fd
            );

            // 3. Вернуть кадр
            if (!enable_fec)
                ret = RK_MPI_VI_ReleaseChnFrame(pipe_id_, g_vi_for_npu_ivs_id, &stViFrame);
            else
                ret = RK_MPI_VPSS_ReleaseChnFrame(0, 2, &stViFrame);

            loopCount++;
        } else {
            LOG_ERROR("RK_MPI_VI or VPSS_GetChnFrame timeout %x\n", ret);
            sleep(1);
        }

        // 4. Контроль FPS NPU
        long long cost_time = rkipc_get_curren_time_ms() - before_time;
        if ((cost_time > 0) && (cost_time < npu_cycle_time_ms))
            usleep((npu_cycle_time_ms - cost_time) * 1000);
    }
    return NULL;
}
```

**3. Что делает `rkipc_rockiva_write_nv12_frame_by_fd`** (`common/rockiva/rockiva.c:393-423`):

```c
int rkipc_rockiva_write_nv12_frame_by_fd(uint16_t width, uint16_t height,
                                         uint32_t frame_id, int32_t fd) {
    RockIvaImage *image = (RockIvaImage *)malloc(sizeof(RockIvaImage));
    memset(image, 0, sizeof(RockIvaImage));
    image->info.width = width;
    image->info.height = height;
    image->info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    image->info.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
    image->frameId = frame_id;
    image->dataAddr = NULL;        // не используем виртуальный адрес
    image->dataPhyAddr = NULL;     // не используем физический адрес
    image->dataFd = fd;            // ← DMA-BUF fd (zero-copy)

    int ret = ROCKIVA_PushFrame(rkba_handle, image, NULL);
    if (ret == 0)
        rk_signal_wait(rockiva_signal, 10000);  // ждать освобождения кадра
    free(image);
    return ret;
}
```

**4. Инициализация RockIva** (`common/rockiva/rockiva.c:183-322`):

```c
int rkipc_rockiva_init() {
    const char *model_type = rk_param_get_string("event.regional_invasion:rockiva_model_type", "small");
    const char *model_path = rk_param_get_string("event.regional_invasion:rockiva_model_path", "/oem/usr/lib/");

    snprintf(globalParams.modelPath, ROCKIVA_PATH_LENGTH, model_path);
    globalParams.coreMask = 0x04;
    globalParams.logLevel = ROCKIVA_LOG_ERROR;

    if (!strcmp(model_type, "small") || !strcmp(model_type, "medium"))
        globalParams.detModel |= ROCKIVA_DET_MODEL_PFP;   // Person/Face/Pet
    else if (!strcmp(model_type, "big"))
        globalParams.detModel |= ROCKIVA_DET_MODEL_CLS8;  // 8 классов

    globalParams.imageInfo.width  = 960;   // видео.2:width
    globalParams.imageInfo.height = 540;   // видео.2:height
    globalParams.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    globalParams.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_ROTATE_180;

    ROCKIVA_Init(&rkba_handle, ROCKIVA_MODE_VIDEO, &globalParams, NULL);
    ROCKIVA_BA_Init(rkba_handle, &initParams, rkba_callback);  // Behavior Analysis
    ROCKIVA_SetFrameReleaseCallback(rkba_handle, rockiva_frame_release_callback);
    return 0;
}
```

**5. Callback получения результатов AI** (`common/rockiva/rockiva.c:117-176`):

```c
void rkba_callback(const RockIvaBaResult *result, const RockIvaExecuteStatus status,
                   void *userData) {
    if (result->objNum == 0) return;

    for (int i = 0; i < result->objNum; i++) {
        // result->triggerObjects[i].objInfo.rect.topLeft.x/y
        // result->triggerObjects[i].objInfo.rect.bottomRight.x/y
        // result->triggerObjects[i].objInfo.objId
        // result->triggerObjects[i].objInfo.score
        // result->triggerObjects[i].objInfo.type   (PERSON/FACE/PET/VEHICLE)
        // result->triggerObjects[i].triggerRules
        // result->triggerObjects[i].firstTrigger.ruleID
        // result->triggerObjects[i].firstTrigger.triggerType
    }
}
```

**6. Конфиг в ini** (`rkipc-2688x1520.ini`):

```ini
[video.source]
enable_npu = 1
npu_fps = 10

[video.2]
width = 2560
height = 1440
max_width = 960
max_height = 540

[event.regional_invasion]
enabled = 1
rockiva_model_type = big          ; small | medium | big
rockiva_model_path = /oem/usr/lib/
sensitivity_level = 50
time_threshold = 1
proportion = 5
position_x = 0
position_y = 0
width = 256
height = 256
```

#### Как использовать для своего кода

Этот же паттерн работает и **без RockIva** — замените `rkipc_rockiva_write_nv12_frame_by_fd()` на прямой вызов `rknn_api`:

```c
// Вместо RockIva — напрямую rknn_api:
rknn_input input;
memset(&input, 0, sizeof(input));
input.index = 0;
input.type = RKNN_TENSOR_UINT8;
input.fmt = RKNN_TENSOR_FMT_NHWC;
input.attr = RKNN_INPUT_ATTR_TYPE_DMABUF;   // zero-copy через fd
input.fd = RK_MPI_MB_Handle2Fd(stViFrame.stVFrame.pMbBlk);
input.w = stViFrame.stVFrame.u32Width;
input.h = stViFrame.stVFrame.u32Height;
rknn_inputs_set(ctx, 1, &input);
rknn_run(ctx, nullptr);
rknn_output outputs[1];
rknn_outputs_get(ctx, 1, outputs, nullptr);
// ... обработка outputs ...
rknn_outputs_release(ctx, 1, outputs);
```

> **Важно:** `librknnmrt.so` / `librknn.so` — отдельная библиотека из RKNN-SDK, не входит в этот репо. Проверьте наличие на плате: `find / -name "librknn*"`

### Подход 2: TGI (Task Graph Interface) — высокоуровневый графовый API

TGI — это графовый движок Rockchip, который позволяет описывать медиа-пайплайны в JSON с узлами `rockx` (NPU), `rkrga` (RGA), `rkeptz` (AI PTZ), `rkisp` (ISP) и др. Официальная документация (`Rockchip_Developer_Guide_Linux_Rockit_CN.pdf`, V0.8.0, 2020-10-28) заявляет поддержку TGI на RV1126/RV1109 (Linux 4.19).

#### Что есть в SDK

В `external/rockit/tgi/sdk/`:
- **50+ заголовков** (`RTTaskGraph.h`, `RTTaskNode.h`, `RTMediaRockx.h`, `RTUVCGraph.h`, и др.)
- **14 JSON-конфигов** пайплайнов (см. ниже)
- **PDF-документация** (`tgi/doc/Rockchip_Developer_Guide_Linux_Rockit_CN.pdf`)
- `RockitConfig.cmake` для сборки

#### JSON-конфиги TGI для RV1126B

В `external/rockit/tgi/sdk/conf/` (для arm) и `conf/arch64/` (для arm64):

| Конфиг | Пайплайн |
|--------|----------|
| `aicamera_rockx_only.json` | rkisp → rockx (face_detect, face_landmark, pose_body) |
| `aicamera_rockx.json` | UVC + ZOOM + EPTZ + RockX |
| `aicamera_uvc.json` | Только UVC превью |
| `aicamera_uvc_zoom.json` | UVC + Zoom + Pan/Tilt |
| `aicamera_uvc_zoom_eptz.json` | UVC + Zoom + EPTZ |
| `aicamera_uvc_zoom_rockx.json` | UVC + Zoom + RockX |
| `aicamera_uvc_zoom_eptz_rockx.json` | Всё вместе |
| `aicamera_uvc_rkvo.json` | UVC + VO выход |
| `aicamera_stasteria.json` | ST Asteria (другой AI-движок) |
| `aicamera_uvc_zoom_stasteria.json` | UVC + Zoom + ST Asteria |
| `aicamera_faceae.json` | Face AE (экспозиция по лицу) |
| `aicamera_faceline.json` | Face line |
| `aisingle.json` | external_source → st_asteria → link_output |
| `arch64/aicamera_uvc_zoom_fec.json` | UVC + Zoom + FEC (arm64) |

#### Пример: `aicamera_rockx_only.json` — чистый AI-пайплайн

```
rkisp (rkispp_scale1, 640x360 NV12)
  → rockx (face_detect)      → link_output
  → rockx (face_landmark)    → link_output
  → rockx (pose_body_v2)     → link_output
```

Это именно то, что хочется: **кадр с камеры → NPU (inference) → результат**, декларативно в JSON.

#### Пример: `aisingle.json` — внешний источник в NPU

```
external_source (NV21) → st_asteria (face/attribute/feature) → link_output
```

Узел `external_source` принимает кадры извне — можно подавать из MPI (`RK_MPI_VI_GetChnFrame`).

#### Доступные node-типы (из JSON-конфигов и PDF, стр. 2)

| node_name | Тип | Что делает |
|-----------|-----|-----------|
| `rkisp` | Device | ISP обработка |
| `alsa_capture` | Device | Аудио вход |
| `alsa_playback` | Device | Аудио выход |
| `rkmpp_dec` | Codec | Декодер H264/H265/MJPEG |
| `rkmpp_enc` | Codec | Кодер H264/H265/MJPEG |
| `rkrga` | Filter | **RGA** — scale/crop/rotate |
| `rockx` | Filter | **NPU** — face detect, pose, landmark, gender/age |
| `st_asteria` | Filter | ST Asteria AI |
| `rkeptz` | Filter | AI-based EPTZ (электронный PTZ) |
| `alg_3a` / `alg_anr` | Filter | Audio 3A |
| `resample` | Filter | Audio resample |
| `link_output` | Sink | Выход графа |
| `external_source` | Source | Внешний источник (кадры подаёте вы) |
| `fwrite` | Sink | Запись в файл |

#### Что умеет `rockx` (NPU-узел)

Из `RTMediaRockx.h` и JSON-конфигов:

```c
rockx_face_detect       // детекция лиц
rockx_face_landmark     // ключевые точки лица
rockx_pose_body_v2      // поза тела
rockx_face_gender_age   // пол и возраст
```

Модели загружаются через `librknn.so` / `librknnmrt.so`.

#### API TGI (из `RTTaskGraph.h`)

```cpp
RTTaskGraph *graph = new RTTaskGraph();
graph->autoBuild("aicamera_rockx_only.json");  // построить граф из JSON
graph->prepare();                                // подготовить ресурсы
graph->start();                                  // запустить
// Наблюдать выходной поток:
graph->observeOutputStream("stream_name", id, [](RTMediaBuffer *buf) {
    // Получить AI-результат
    RTAIDetectResults *aiResult;
    buf->getMetaData()->findPointer(OPT_AI_DETECT_RESULT, &aiResult);
    buf->release();
    return RT_OK;
});
graph->stop();
graph->release();
```

#### Состояние в этом SDK

TGI **спроектирован для RV1126B** (документация, 14 JSON-конфигов, заголовки), но **реализация отсутствует в `librockit.so`**. Проверка `nm -D --defined-only` (1096 экспортированных символов):

| Символ | Есть в `librockit.so`? |
|--------|:---:|
| `RK_MPI_VI_GetChnFrame` и др. MPI | **Да** (все) |
| `RTTaskGraph::autoBuild` | **Нет** |
| `RTTaskGraph::start` | **Нет** |
| `RTTaskGraph::linkNode` | **Нет** |
| `rockx` | **Нет** |
| `rknn_init` / `rknn_run` | **Нет** |

Дополнительно:
- `RockitConfig.cmake:12` ссылается на `lib/lib64/librockit.so` — путь не существует (реальные пути: `lib/arm64/rv1126b/linux/`, `lib/arm/rv1126b/linux/`)
- Каталога `app/aiserver/` (на который ссылается PDF для кастомных плагинов) нет
- Отдельной `libtgi.so` нет
- TGI отсутствует во **всех** `librockit.so` в SDK (rv1126b, rk3588, rv1106)

> **Важно:** строка `rknn` в `librockit.so` — это **не** RKNN API. Это `dlopen("librknnmrt.so")` в аудио-модуле SKV (звук DOA) и лог `"can not open library(%s), it may not need NPU"`. Реальные `rknn_init`/`rknn_run` отсутствуют.

**Для использования TGI на RV1126B нужна `librockit.so` с включённым TGI** — возможно, из полного Rockchip SDK или отдельной сборки. JSON-конфиги и заголовки готовы к использованию.

### Подход 3: RockIva — готовый AI-движок rkipc (НЕ для своих моделей)

На RV1126B в `rkipc` используется **RockIva** (`librockiva.so`) — высокоуровневая AI-библиотека для IPC. Она загружает предобученные `.rknn` модели из `/oem/usr/lib/` и предоставляет API для детекции объектов и анализа поведения (area invasion, tripwire).

Полный рабочий пример использования RockIva приведён выше в разделе **"Реальный пример из rkipc (RV1126B)"** — от инициализации VI канала до callback с результатами AI.

| model_type | Модель | Классы |
|-----------|--------|--------|
| `small` / `medium` | PFP | Person, Face, Pet |
| `big` | CLS8 | Person, Face, Pet, Vehicle, + ещё |

RockIva внутри себя использует `rknn_api`, но **скрывает его**. Свою модель через RockIva загрузить нельзя — формат выходов зашит в коде (`RockIvaBaResult` с `triggerObjects`, `objInfo`, `rect`, `type`).

> **Для своих моделей** — используйте тот же паттерн (VI → `RK_MPI_MB_Handle2Fd` → NPU), но вместо `ROCKIVA_PushFrame` вызывайте `rknn_api` напрямую (см. пример выше в "Как использовать для своего кода").

### Сравнение подходов

| | rkadk + MPI | TGI | RockIva |
|---|---|---|---|
| **Спроектирован для RV1126B?** | Да | **Да** (PDF, 14 JSON-конфигов) | Да |
| **Реализация в SDK?** | **Да** (librockit.so) | **Нет** (заголовки и JSON есть, символов в .so нет) | Да (отдельная .so) |
| **Конфиг** | ini-файлы | JSON-файлы | ini rkipc |
| **NPU?** | Только вручную (rknn_api) | Да (`rockx` node) | Да (предобученные) |
| **Своя модель?** | **Да** (rknn_api) | Да (custom node) | Нет |
| **Zero-copy?** | Да (MB_BLK / dmabuf) | Да (RTMediaBuffer) | Да (dmabuf fd) |
| **Гибкость** | Полная | Высокая | Низкая (только детекция/BA) |

### Ответ: можно ли взять кадр с RGA и отправить в NPU?

**В этом SDK — единственный рабочий путь: MPI + rknn_api вручную.** TGI спроектирован для этого (JSON `aicamera_rockx_only.json`), но `librockit.so` не содержит реализации.

```c
RK_MPI_VPSS_GetChnFrame(...);          // забрать кадр из VPSS/RGA
fd = RK_MPI_MB_Handle2Fd(mb);          // получить dmabuf fd (zero-copy)
rknn_inputs_set(ctx, 1, &input);       // отправить в NPU через rknn_api
rknn_run(ctx, nullptr);                // inference
rknn_outputs_get(ctx, 1, outputs, ...);// результат
RK_MPI_VPSS_ReleaseChnFrame(...);      // вернуть кадр
```

- `librknn.so` / `librknnmrt.so` — отдельная библиотека (не входит в этот репо, нужна из RKNN-SDK)
- Кадр берётся из MPI (VPSS/VI), передаётся в NPU через dmabuf fd (zero-copy)
- Именно так `rkipc` и работает, но через RockIva как прослойку — вы можете сделать то же напрямую

**Проверить наличие NPU-библиотек на плате:**
```bash
find / -name "librknn*" 2>/dev/null       # rknn runtime
find / -name "librockiva*" 2>/dev/null    # rockiva
find / -name "*.rknn" 2>/dev/null         # модели
grep enable_npu /etc/rkipc/*.ini          # включён ли NPU в rkipc
```

---

## CLI: vi_grab_frame — сохранение кадра в файл

В `app/vi_grab_frame/` есть минимальная CLI-программа, которая получает кадр с камеры через MPI и сохраняет его в файл как raw NV12. Основано на паттерне из rkipc (см. выше).

### Использование

```bash
# Один кадр 1920x1080 → 1920x1080_nv12.raw
./vi_grab_frame -w 1920 -h 1080

# Один кадр 2560x1440 с канала 4 (как в rkipc для NPU)
./vi_grab_frame -w 2560 -h 1440 -c 4 -o frame.raw

# 10 кадров, каждый в отдельный файл: frame.raw_0000.raw, frame.raw_0001.raw, ...
./vi_grab_frame -w 640 -h 360 -c 4 -n 10 -o frame.raw -v

# Подробный вывод
./vi_grab_frame -w 1920 -h 1080 -v
```

### Параметры

| Параметр | Описание | По умолчанию |
|----------|----------|:---:|
| `-w, --width` | ширина кадра (обязательно) | — |
| `-h, --height` | высота кадра (обязательно) | — |
| `-d, --dev` | VI device id | 0 |
| `-p, --pipe` | VI pipe id (= dev) | 0 |
| `-c, --channel` | VI channel id | 0 |
| `-o, --output` | имя файла | `<w>x<h>_nv12.raw` |
| `-n, --count` | сколько кадров | 1 |
| `-t, --timeout` | таймаут GetChnFrame, мс | 1000 |
| `-v, --verbose` | подробный вывод | нет |

### Сборка

```bash
# На Linux-машине с toolchain aarch64:
cd app/vi_grab_frame
mkdir build && cd build

# Для arm64 (RV1126B 64-bit):
cmake -DTARGET_ARCH=arm64 -DTARGET_CHIP=rv1126b ..
make

# Для arm (RV1126B 32-bit):
cmake -DTARGET_ARCH=arm -DTARGET_CHIP=rv1126b ..
make

# Для cross-compile раскомментировать в CMakeLists.txt:
#   set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
```

### Что делает программа

1. `RK_MPI_SYS_Init()` — инициализация MPI
2. `RK_MPI_VI_SetDevAttr` + `RK_MPI_VI_EnableDev` — настройка VI устройства
3. `RK_MPI_VI_SetChnAttr` + `RK_MPI_VI_EnableChn` — настройка VI канала (NV12, DMABUF)
4. `RK_MPI_VI_GetChnFrame` — получение кадра
5. `RK_MPI_MB_Handle2VirAddr` — виртуальный адрес данных
6. `fwrite()` — запись в файл (NV12: `w*h*3/2` байт)
7. `RK_MPI_VI_ReleaseChnFrame` — освобождение кадра
8. `RK_MPI_VI_DisableChn` + `RK_MPI_VI_DisableDev` + `RK_MPI_SYS_Exit` — очистка

### Просмотр сохранённого файла

```bash
# На Linux: конвертация NV12 в PNG через ffmpeg
ffmpeg -pix_fmt nv12 -s 1920x1080 -i 1920x1080_nv12.raw -f image2 frame.png

# Или через gst-launch (GStreamer)
gst-launch-1.0 filesrc location=1920x1080_nv12.raw ! \
  rawvideoparse format=nv12 width=1920 height=1080 ! \
  videoconvert ! pngenc ! filesink location=frame.png

# Размер файла: w*h*3/2 (NV12)
# 1920x1080: 3110400 байт
# 2560x1440: 5529600 байт
# 640x360:   345600 байт
```

### Файлы

- `app/vi_grab_frame/vi_grab_frame.c` — исходник (~250 строк)
- `app/vi_grab_frame/CMakeLists.txt` — сборка

---

## Сборка

```bash
# Настройка окружения (на Linux-машине сборки)
export PATH=$PATH:/path/to/toolchain/bin

# Применить патчи к чистому SDK (если нужно)
git apply dual_camera_patch.diff
git apply dual_camera_patch_rv1126b.diff

# Сборка rkadk_dual_disp_test
cd build
cmake .. -DRK_MEDIA_CHIP=rv1126b -DARCH64=ON
make rkadk_dual_disp_test
```

## Запуск

```bash
# По умолчанию: 256px ширина, высота пропорциональна, два окна рядом
rkadk_dual_disp_test -a /etc/iqfiles -p /data/rkadk

# Свои параметры
rkadk_dual_disp_test -W 256 -H 0 -g 8 -s 0

# Параметры:
#   -a  путь к IQ-файлам (default: /etc/iqfiles)
#   -p  путь к ini-параметрам (default: /data/rkadk)
#   -W  ширина окна в пикселях (0 = использовать высоту)
#   -H  высота окна в пикселях (0 = пропорционально ширине)
#   -s  начальный offset первого окна (default: 0)
#   -g  зазор между окнами в пикселях (default: 8)
```

---

## Важные замечания

- **Sensor 1** по умолчанию `used_isp = FALSE`. Если вторая матрица — ISP-сенсор, поставь `used_isp = TRUE` в `rkadk_setting_sensor_1.ini` и проверь `device_name`.
- Для **чёрно-белой матрицы** без IR-фильтра могут понадобиться отдельные IQ-файлы.
- `rotation=1` (поворот 90°) остаётся у обоих сенсоров RV1126B — если поворот не нужен, уберите его в ini.
- `RKADK_MAX_SENSOR_CNT` должен быть ≥ 2 для двухкамерной конфигурации.

---

## Структура репозитория

```
.
├── app/
│   ├── ipcweb-backend/     # REST API backend для IP-камер
│   ├── lvgl_demo/          # LVGL UI демо
│   ├── rkadk/              # Rockchip ADK (media framework)
│   │   ├── examples/       # Тестовые приложения
│   │   │   └── rkadk_dual_disp_test.c   # ← NEW: dual camera test
│   │   ├── inicfg/
│   │   │   ├── rv1106_1103/  # ini для RV1106/RV1103B
│   │   │   └── rv1126b/      # ini для RV1126B
│   │   └── src/display/rkadk_disp.c   # ← MODIFIED: per-CamId handles
│   └── rkipc/              # Rockchip IPC приложение
│       └── src/
│           ├── rv1126b_ipc/
│           ├── rv1126b_dv/
│           ├── rv1126b_dual_ipc/
│           └── ...         # конфиги для разных платформ
├── external/rockit/        # Rockit MPI (медиа API)
│   ├── lib/arm/rv1126b/    # 32-бит библиотеки
│   ├── lib/arm64/rv1126b/  # 64-бит библиотеки
│   └── mpi/                # MPI headers и examples
├── dual_camera_patch.diff            # патч RV1106
├── dual_camera_patch_rv1126b.diff    # патч RV1126B
└── dual_camera_patch_README.md       # оригинальное описание
```

## Git-история

| Коммит | Описание |
|--------|----------|
| `36f0c5c` | Initial commit — чистый SDK из `rv1126b-linux6.1 @ 7285d5c2f` |
| `dac2b43` | feat(dual_camera) — двухкамерный дисплей для RV1106 и RV1126B |
