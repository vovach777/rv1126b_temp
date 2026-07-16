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

### Подход 2: TGI (Task Graph Interface) — "высокоуровневый"

В `external/rockit/tgi/` есть **полноценный графовый движок** — `RTTaskGraph`. Это гораздо ближе к идее "описал граф — оно само работает":

```cpp
// RTTaskGraph.h
class RTTaskGraph {
    RT_RET autoBuild(const char* config, RT_BOOL isFileType = RT_TRUE);
    RT_RET prepare(RtMetaData *params);
    RT_RET start();
    RT_RET stop();
    RTCBHandle observeOutputStream(string streamName,
                                   function<RT_RET(RTMediaBuffer*)> callback);
    RTTaskNode* createNode(string nodeConfig, string streamConfig);
    RT_RET linkNode(RTTaskNode *src, RTTaskNode *dst);
};
```

Граф описывается **JSON-файлом**, а не ini. И вот там **есть NPU**:

```json
// aicamera_rockx.json — реальный пример из SDK
"node_4": {
    "node_opts": {
        "node_name": "rockx"                    // ← NPU-узел!
    },
    "stream_opts": {
        "stream_input":  "eptz_face_detect_in",
        "stream_output": "eptz_face_detect_out",
        "stream_fmt_in":  "image:nv12",
        "stream_fmt_out": "image:rect"
    },
    "stream_opts_extra": {
        "opt_rockx_model": "rockx_face_detect"  // ← модель RKNN
    }
}
```

Доступные node-типы (из JSON-конфигов):

| node_name | Что делает |
|-----------|-----------|
| `rkisp` | ISP обработка |
| `rkzoom` | Zoom контроль |
| `rkrga` | **RGA** — scale/crop/rotate |
| `rockx` | **NPU** — face detect, pose, landmark, gender/age |
| `rkeptz` | AI-based EPTZ (электронный PTZ) |
| `link_output` | Выход графа |
| и др. | |

#### Полный пайплайн из JSON (реальный пример)

```
rkisp → rkzoom → rockx(face_detect) → rkeptz → rkrga(scale) → link_output
```

Это именно то, что хочется: **кадр с камеры → RGA (scale) → NPU (inference) → результат**. И всё это декларативно в JSON.

#### Что умеет rockx (NPU-узел)

Из `RTMediaRockx.h`:

```c
ROCKX_FACE_DETECT       // детекция лиц
ROCKX_FACE_LANDMARK     // ключевые точки лица
ROCKX_POSE_BODY         // поза тела
ROCKX_POSE_BODY_V2      // поза тела v2
ROCKX_POSE_FINGER       // жесты пальцами
ROCKX_FACE_GENDER_AGE   // пол и возраст
```

Это предобученные модели через `librknn.so`. Для своих моделей нужен `rknn_api` напрямую.

### Сравнение двух подходов

| | rkadk + MPI | TGI (Task Graph) |
|---|---|---|
| **Конфиг** | ini-файлы | JSON-файлы |
| **NPU?** | Нет (только через ручной GetChnFrame + rknn_api) | Да (`rockx` node) |
| **RGA?** | Да (VPSS через `VIDEO_PROC_DEV_RGA`) | Да (`rkrga` node) |
| **Гибкость** | Низкая — фиксированные пайплайны | Высокая — произвольный граф |
| **Уровень** | C API, оркестрация в rkadk | C++ API, графовый движок |
| **Свой код в графе?** | Да (GetChnFrame → свой код → SendFrame) | Да (custom RTTaskNode) |
| **Zero-copy?** | Да (MB_BLK / dmabuf) | Да (RTMediaBuffer) |

### Ответ: можно ли взять кадр с RGA и отправить в NPU?

**Два пути:**

1. **TGI (рекомендуемый)** — описать граф в JSON: `rkisp → rkrga → rockx → ...`. NPU встроен как node. Но предустановленные модели — только `rockx_*`. Для своих моделей нужен custom node.

2. **MPI + rknn_api (ручной)** — в C-коде:
   ```c
   RK_MPI_VPSS_GetChnFrame(...);          // забрать кадр из VPSS/RGA
   fd = RK_MPI_MB_Handle2Fd(mb);          // получить dmabuf fd
   rknn_inputs_set(ctx, 1, &input);       // отправить в NPU через rknn_api
   rknn_run(ctx, nullptr);                // inference
   rknn_outputs_get(ctx, 1, outputs, ...);// результат
   RK_MPI_VPSS_ReleaseChnFrame(...);      // вернуть кадр
   ```
   Это **не через ini**, но zero-copy и полный контроль. `librknn.so` — отдельная библиотека (не входит в этот репо, нужна из RKNN-SDK).

Через rkadk ini **нельзя** — rkadk не знает про NPU. TGI JSON — можно, но только с предустановленными rockx-моделями. Для своих моделей — MPI + rknn_api вручную.

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
