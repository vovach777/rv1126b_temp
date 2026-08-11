# Документ 2: План портирования percomedia на RV1126B + лицензионная IVA

## 1. Цель порта

Перенести `percomedia` (библиотека распознавания лиц) с RV1109/1126 + старая `rockx`
на RV1126B + новая `ROCKIVA` (лицензионная). GUI (`s30gui`) переносится с минимальными
изменениями — он общается с percomedia через стабильный API (cb.h).

**Принцип:** минимальные затраты = максимум переиспользования старого кода +
замена только тех компонентов, которые несовместимы по API.

---

## 2. Что меняется и что остаётся

### 2.1 Остаётся без изменений (переиспользуется 1:1)

| Компонент | Файлы | Почему |
|-----------|-------|--------|
| **API библиотеки** | `percomedia.h`, `cb.h` | Стабильный интерфейс к GUI, не зависит от платформы |
| **ThreadController** | `threadcontroller.h/.cpp` | Логика потоков/таймеров та же |
| **Users / UserList** | `userclass.h/.cpp` | Логика выбора текущего, автомат состояний — не зависит от rockx API (только типы) |
| **MotionTrigger** | `motion_trigger.h/.cpp` | Логика движения по трекингу — не зависит от API |
| **ImageBuf** | `imagebuf.h/.cpp`, `lockedbuf.h/.cpp` | Синхронный обмен кадрами — не зависит от API |
| **Конфигурация** | `percomedia_config.h/.cpp` | Структуры настроек — те же |
| **DbgLogger** | `dbg.h/.cpp` | Логирование — не зависит от API |
| **GUI (s30gui)** | `s30gui/*` | Общается через PercoMedia API, не зависит от платформы |
| **Рисование боксов** | `draw_rects` в `ispclass.cpp` | `imfill` + `imcomposite` — librga im2d, тот же API |
| **process.cpp** | `process.h/.cpp` | Утилиты (getPidByName и т.д.) |

### 2.2 Заменяется (новый API)

| Компонент | Старый API | Новый API | Причина |
|-----------|-----------|-----------|---------|
| **VI** | `rkmedia RK_MPI_VI_*` | `RKMPI RK_MPI_VI_*` | Новый SDK использует RKMPI |
| **VO** | `rkmedia RK_MPI_VO_CreateChn` | `RKMPI RK_MPI_VO_*` | Тот же класс, другая реализация |
| **VENC** | `rkmedia RK_MPI_VENC_*` | `RKMPI RK_MPI_VENC_*` | Снимки JPEG |
| **RGA (видео)** | `rkmedia RK_MPI_RGA_CreateChn` (4 канала) | `librga im2d improcess()` | RKMPI не имеет RGA как MPI-модуля |
| **ISP (rk_aiq)** | `rk_aiq_*` (Isp_LL) | `rk_aiq_*` (через rkaiq_server) | Возможно оставить, но rkaiq_server уже запущен |
| **Нейросеть** | `rockx_*` (6 модулей) | `ROCKIVA_FACE_*` (один init) | rockx нет на новой платате |

### 2.3 Уходит полностью (не нужно портировать)

| Компонент | Почему уходит |
|-----------|---------------|
| **RGA1, RGA2 (NN каналы)** | ROCKIVA сама вращает кадр через `transformMode` — не нужен RGA для нейросети |
| **rockx_face_detect** | Внутри `ROCKIVA_FACE_Init` (faceCaptureEnable) |
| **rockx_face_quality** | Внутри ROCKIVA (настраивается через `RockIvaFaceQualityConfig`) |
| **rockx_face_landmark(5)** | Внутри ROCKIVA (faceLandmarkEnable=1) |
| **rockx_face_pose** | Внутри ROCKIVA (quality config с углами) |
| **rockx_face_align** | Внутри ROCKIVA (автоматически перед recognize) |
| **rockx_face_recognize** | Внутри ROCKIVA → feature в callback |
| **rockx_face_liveness** | Внутри ROCKIVA (faceLivenessEnable=1) |
| **rockx_object_track** | Внутри ROCKIVA (трекер встроен) |
| **rockx_image_equalize_hist** | Внутри ROCKIVA (если нужно) |
| **rockx_image_convert_with_crop** | Заменяется на RGA `improcess` (для фото) |
| **ImageBuf (для NN)** | ROCKIVA берёт кадр напрямую из VI через `PushFrame` — не нужен промежуточный буфер для нейросети |
| **Isp_LL (rk_aiq)** | На новой плате `rkaiq_server` запущен как сервис — не нужно поднимать ISP вручную |
| **easymedia (rkmedia)** | Заменяется на RKMPI |

---

## 3. Новая архитектура (после порта)

```
┌─────────────────────────────────────────────────────────┐
│  s30gui (Qt) — БЕЗ ИЗМЕНЕНИЙ                             │
│  AIService → PercoMedia API (cb.h)                       │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────┴────────────────────────────────────┐
│  percomedia (libpercomedia.so) — ПОРТ                   │
│  ├─ ThreadController — БЕЗ ИЗМЕНЕНИЙ                    │
│  ├─ Users/UserList — БЕЗ ИЗМЕНЕНИЙ (типы адаптировать)   │
│  ├─ MotionTrigger — БЕЗ ИЗМЕНЕНИЙ                       │
│  ├─ ImageBuf — ТОЛЬКО для фото (не для NN)              │
│  ├─ ImageSignalProcessing (ISP) — ПОРТ:                │
│  │   ├─ VI: RKMPI (3 канала: VO, NN, ENC)               │
│  │   ├─ RGA: im2d improcess (только VO + фото)          │
│  │   ├─ VO: RKMPI (DRM overlay)                         │
│  │   ├─ VENC: RKMPI (JPEG)                              │
│  │   └─ draw_rects: БЕЗ ИЗМЕНЕНИЙ (imfill+imcomposite)  │
│  ├─ FaceRecogn (NNC) — ПОРТ:                            │
│  │   ├─ ROCKIVA_Init (transformMode=ROTATE_90)          │
│  │   ├─ ROCKIVA_FACE_Init (все флаги)                   │
│  │   ├─ ROCKIVA_PushFrame (из VI напрямую)              │
│  │   └─ callback → Users (вместо ручного pipeline)       │
│  └─ rkaiq_server — уже запущен (не Isp_LL)              │
└─────────────────────────────────────────────────────────┘
```

### Поток данных (новая схема)

```
RGB камера 1920x1080 NV12
    │
    ▼
   VI (RKMPI, 3 канала из одного ISP)
    │
    ├── VI ch_VO (1920x1080) → GetChnFrame
    │       → RGA improcess(rot90, NV12→ARGB8888, 800x1280)
    │       → draw_rects (imfill + imcomposite)  ← Users::UserList
    │       → RK_MPI_VO_SendFrame → DRM → дисплей 800x1280 portrait
    │
    ├── VI ch_NN (960x540, scale через ISP) → GetChnFrame
    │       → ROCKIVA_PushFrame(dataFd, transformMode=ROTATE_90)
    │       ↑ IVA сама вращает + detect + quality + landmark + align + recognize
    │       → callback: face box + feature + trackId + liveness
    │       → Users::UserList (update)
    │
    └── VI ch_ENC (1920x1080) → bind → VENC (JPEG) → /tmp/rgb.jpeg
```

**Ключевое отличие от old:** ROCKIVA берёт кадр напрямую из VI (через
`PushFrame` с dmabuf fd), сама вращает и обрабатывает. Не нужны RGA1/RGA2
для нейросети и не нужен ImageBuf для передачи кадров в NNC.

---

## 4. Детальный план портирования по файлам

### 4.1 Файлы БЕЗ ИЗМЕНЕНИЙ (переиспользуются как есть)

| Файл | Строк | Что делает |
|------|-------|-----------|
| `percomedia.h` | 50 | API библиотеки |
| `percomedia.cpp` | 111 | Реализация API (делегирует ThreadController) |
| `cb.h` | 33 | Типы callbacks + t_userdata |
| `threadcontroller.h` | 87 | Каркас потоков/таймеров |
| `threadcontroller.cpp` | 220 | Логика управления потоками |
| `percomedia_config.h` | 153 | Структуры конфигов |
| `percomedia_config.cpp` | — | Загрузка JSON |
| `dbg.h/.cpp` | — | Логирование |
| `process.h/.cpp` | — | Утилиты |
| `motion_trigger.h` | 112 | Детектор движения |
| `motion_trigger.cpp` | — | — |
| `imagebuf.h` | 45 | Синхронный буфер (для фото) |
| `imagebuf.cpp` | — | — |
| `lockedbuf.h/.cpp` | — | Мьютекс-обёртка |
| `json.hpp` | — | nlohmann/json |
| `drmconf.h/.cpp` | — | DRM конфигурация |

**Итого без изменений:** ~16 файлов, ~1000 строк.

### 4.2 Файлы с МИНИМАЛЬНЫМИ изменениями (адаптация типов)

| Файл | Изменения |
|------|-----------|
| `userclass.h` | Заменить `rockx_rect_t` → свой `t_rect` (или `RockIvaRectangle`). Заменить `rockx_object_t*` → структура-обёртка. Заменить `rockx_object_array_t` → `std::vector<face_info>`. Логика выбора текущего — без изменений. |
| `userclass.cpp` | Адаптировать `tracklist()` под новые типы. Логика — без изменений. |
| `nnclass.h` | Убрать `rockx_*` includes. Добавить `rockiva_face_api.h`. Заменить `ModMap` → `RockIvaHandle`. |
| `threadcontroller.cpp` | `get_face()` — заменить `rockx_image_t` на получение кадра из VO/VI. Остальное без изменений. |

### 4.3 Файлы с СУЩЕСТВЕННЫМИ изменениями (замена API)

#### `ispclass.h` / `ispclass.cpp` — видеопайплайн

**Уходит:**
- `rkmedia_api.h`, `rkmedia_rga.h` — старый API
- 4 канала RGA (RGA0/RGA1/RGA2/RGA3) — заменить на im2d
- `RK_MPI_RGA_CreateChn`, `RK_MPI_SYS_Bind(VI→RGA)` — нет в RKMPI
- `RK_MPI_SYS_RegisterOutCb` для RGA — нет в RKMPI

**Приходит:**
- `rk_mpi_vi.h`, `rk_mpi_vo.h`, `rk_mpi_venc.h` — RKMPI
- `im2d.h` — `improcess()`, `imfill()`, `imcomposite()` (уже используется в draw_rects!)
- VI: 3 канала (VO, NN, ENC) через `RK_MPI_VI_SetChnAttr` + `EnableChn`
- VO: `RK_MPI_VO_CreateChn` + `SendFrame` (вместо bind)
- VENC: `RK_MPI_VENC_CreateChn` + bind VI→VENC

**draw_rects — БЕЗ ИЗМЕНЕНИЙ** (imfill + imcomposite — тот же librga im2d).

#### `isp_vi.h` / `isp_vi.cpp` — VI каналы

**Old:** 2 канала (RGB dev=0, IR dev=1), `rkispp_scale0`, bind к RGA.

**New:** 3 канала из одного ISP (как в rkipc rv1126b_ipc):
```c
// ch_VO: 1920x1080 NV12 для дисплея
vi_chn_attr.stSize = {1920, 1080};
vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
RK_MPI_VI_SetChnAttr(pipe_id, ch_VO, &vi_chn_attr);
RK_MPI_VI_EnableChn(pipe_id, ch_VO);

// ch_NN: 960x540 NV12 для нейросети (маленький — ROCKIVA крутит сама)
vi_chn_attr.stSize = {960, 540};
vi_chn_attr.u32Depth = 1;  // +1 для NPU
RK_MPI_VI_SetChnAttr(pipe_id, ch_NN, &vi_chn_attr);
RK_MPI_VI_EnableChn(pipe_id, ch_NN);

// ch_ENC: 1920x1080 NV12 для JPEG (bind к VENC)
RK_MPI_VI_SetChnAttr(pipe_id, ch_ENC, &vi_chn_attr);
RK_MPI_VI_EnableChn(pipe_id, ch_ENC);
```

**IR камера:** на RV1126B одна камера (GC2093) — IR не нужен (или отдельный
VI dev=1 если есть). Liveness через ROCKIVA (faceLivenessEnable) — 2D, без IR.

#### `isp_rga.h` / `isp_rga.cpp` — RGA (ЗНАЧИТЕЛЬНО УПРОЩАЕТСЯ)

**Old:** 4 канала RGA с bind'ами, callbacks, конфигами.

**New:** один вызов `improcess()` для дисплея + `imfill`/`imcomposite` для боксов.
```c
// Для дисплея (вместо RGA3+RGA0):
rga_buffer_t src = wrapbuffer_fd(vi_fd, 1920, 1080, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_fd(vo_fd, 800, 1280, RK_FORMAT_RGBA_8888);
im_rect src_rect = {0, 0, 1920, 1080};
im_rect dst_rect = {0, 0, 800, 1280};
im_opt_t opt; opt.rotate_mode = ROTATE_90;
improcess(src, dst, src_rect, dst_rect, &opt, IM_ALPHA_BLEND_DST_OVER);
imsync();
```

**RGA1/RGA2 (для NN) — УБИРАЮТСЯ** (ROCKIVA сама вращает).

#### `isp_vo.h` / `isp_vo.cpp` — видеовыход

**Old:** `RK_MPI_VO_CreateChn` + bind RGA0→VO0.

**New:** `RK_MPI_VO_CreateChn` + `RK_MPI_VO_SendFrame` (без bind, кадры шлём вручную
после RGA). DRM overlay plane — та же `/dev/dri/card0`.

#### `isp_enc.h` / `isp_enc.cpp` — JPEG энкодер

**Old:** `RK_MPI_VENC_CreateChn` + bind VI→VENC + callback.

**New:** `RK_MPI_VENC_CreateChn` + `RK_MPI_SYS_Bind(VI_ch_ENC→VENC)` + callback.
API почти идентичен (RKMPI), изменения минимальны.

#### `ispll.h` / `ispll.cpp` — rk_aiq ISP

**Уходит полностью.** На RV1126B `rkaiq_server` запущен как системный сервис.
ISP настраивается через `rkaiq` IPC, не нужно поднимать в процессе.
Настройки экспозиции/баланса белого — через `rk_aiq_*` API к серверу (если нужно).

#### `nnclass.h` / `nnclass.cpp` — нейросеть (ЗНАЧИТЕЛЬНО УПРОЩАЕТСЯ)

**Old (22KB, ~575 строк):** ручной pipeline из 10 шагов с rockx.

**New (~200 строк):** один init + PushFrame + callback.

```c
// proc_init:
RockIvaInitParams globalParams = {0};
globalParams.imageInfo.width = 960;
globalParams.imageInfo.height = 540;
globalParams.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
globalParams.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_ROTATE_90;
snprintf(globalParams.modelPath, sizeof(globalParams.modelPath), "/usr/lib/");
globalParams.detModel = ROCKIVA_DET_MODEL_PFP;  // или CLS8

ROCKIVA_Init(&handle, ROCKIVA_MODE_VIDEO, &globalParams, NULL);

RockIvaFaceTaskParams faceParams = {0};
faceParams.faceTaskType.faceCaptureEnable = 1;
faceParams.faceTaskType.faceRecognizeEnable = 1;
faceParams.faceTaskType.faceLandmarkEnable = 1;  // 5 точек
faceParams.faceTaskType.faceLivenessEnable = 1;
// Пороги качества:
faceParams.faceCaptureRule.qualityConfig.blurThreshold = 0.7;
faceParams.faceCaptureRule.qualityConfig.faceFilterThreshold = 0.99;
// Углы (pitch/yaw/roll) — через qualityConfig

RockIvaFaceCallback cb = {
    .detCallback = on_face_det,       // детекция (box + trackId)
    .analyseCallback = on_face_analyse,  // анализ (feature + liveness)
    .postureCallback = NULL
};
ROCKIVA_FACE_Init(handle, &faceParams, cb);

// Поток кадров (вместо таймера onTRecognizeTask):
void vi_thread() {
    while (running) {
        RK_MPI_VI_GetChnFrame(pipe, ch_NN, &vf, 1000);
        RockIvaImage img = {
            .info = {.width=960, .height=540, .format=NV12},
            .dataFd = RK_MPI_MB_Handle2Fd(vf.stVFrame.pMbBlk)
        };
        ROCKIVA_PushFrame(handle, &img, NULL);
        RK_MPI_VI_ReleaseChnFrame(pipe, ch_NN, &vf);
    }
}

// Callback (вместо check_face + nn_face):
void on_face_analyse(const RockIvaFaceCapResults *result, status, userdata) {
    for (int i = 0; i < result->num; i++) {
        RockIvaFaceCapInfo *info = &result->faceCapInfo[i];
        // info->faceRect — bounding box (в повёрнутой системе 960x540)
        // info->feature — feature vector
        // info->liveness — liveness score
        // info->trackId — id для трекинга
        // → обновить Users::UserList
    }
}
```

**Упраздняется:**
- `check_face()` — ROCKIVA сама делает quality + pose фильтрацию
- `nn_face()` — ROCKIVA сама делает align + recognize
- `track()` — ROCKIVA сама трекает
- `onTRecognizeTask()` — не нужен таймер, кадры идут потоком
- `get_facepic()` — заменяется на RGA improcess из VI кадра
- Все `rockx_*` вызовы

**Остаётся:**
- Логика `Users::tracklist()` (адаптировать типы)
- Автомат состояний CHECK→NN→OK
- `centerzoom_bigside()` и геометрические утилиты (для фото)

---

## 5. Сравнение объём кода (old vs new)

| Компонент | Old (строк) | New (строк) | Экономия |
|-----------|-------------|-------------|----------|
| nnclass.cpp | 575 | ~200 | **-375** |
| isp_rga.cpp | 301 | ~80 | **-221** |
| isp_vi.cpp | 98 | ~120 | +22 |
| isp_vo.cpp | 148 | ~120 | -28 |
| isp_enc.cpp | 233 | ~200 | -33 |
| ispll.cpp | ~500 | 0 (убрать) | **-500** |
| ispclass.cpp | 291 | ~250 | -41 |
| userclass.cpp | 191 | ~180 | -11 |
| **Итого порт:** | **~2337** | **~1150** | **-1187 (≈50%)** |

**Библиотека становится вдвое меньше** за счёт того, что ROCKIVA забирает
всю ручную логику face pipeline, а im2d `improcess` заменяет 4 канала RGA.

---

## 6. Что уходит из-за того, что IVA покрывает больше функций

### 6.1 Уходит из nnclass.cpp (face pipeline)

| Функция old | Что делала | Замена в ROCKIVA |
|-------------|-----------|------------------|
| `rockx_face_detect` | детекция лица | `faceCaptureEnable=1` |
| `rockx_face_quality` (×2) | качество RGB + IR | `RockIvaFaceQualityConfig` |
| `rockx_face_landmark(5)` | 5 точек | `faceLandmarkEnable=1` |
| `rockx_face_pose` | углы pitch/yaw/roll | `qualityConfig` (углы) |
| `rockx_face_align` | выравнивание 112x112 | автоматически перед recognize |
| `rockx_face_recognize` | feature vector | `faceRecognizeEnable=1` |
| `rockx_face_liveness` | 2D liveness | `faceLivenessEnable=1` |
| `rockx_object_track` | трекинг id | встроенный трекер |
| `rockx_image_equalize_hist` | эквализация | внутри ROCKIVA |
| `rockx_image_convert_with_crop` | фото | RGA `improcess` (для фото) |
| `rockx_face_feature_compare` | сравнение 1:1 | `ROCKIVA_FACE_FeatureCompare` |
| `rockx_face_feature_search` | поиск 1:N | `ROCKIVA_FACE_SearchFeature` |

**Уходит ~400 строк ручной логики** (check_face + nn_face + track).

### 6.2 Уходит из isp_rga.cpp (RGA каналы для NN)

| Канал | Что делал | Замена |
|-------|-----------|--------|
| RGA1 | RGB для NN (rot270, BGR888 720x1280) | ROCKIVA сама вращает (transformMode) |
| RGA2 | IR для NN (rot270, BGR888 720x1280) | IR не нужен (liveness 2D) |
| `video_packet_callback_rgb` | кадр в ImageBuf для NN | `ROCKIVA_PushFrame` напрямую из VI |
| `video_packet_callback_ir` | кадр в ImageBuf для NN | — |

**Уходит ~200 строк** (2 канала RGA + 2 callback + конфиги).

### 6.3 Уходит ImageBuf (для NN)

`ImageBuf` использовался для синхронной передачи пары кадров ISP→NNC.
ROCKIVA берёт кадр напрямую из VI через `PushFrame` — **ImageBuf не нужен для NN**.

Остаётся только для `get_face()` (фото текущего пользователя для GUI) —
можно упростить до одного буфера.

### 6.4 Уходит Isp_LL (rk_aiq)

На новой плате `rkaiq_server` запущен как сервис. Не нужно поднимать ISP
в процессе percomedia. Настройки экспозиции/баланса белого — через IPC к серверу.

**Уходит ~500 строк** (ispll.h + ispll.cpp).

### 6.5 Уходит таймер сканирования (onTRecognizeTask)

В old: таймер 100ms опрашивал ImageBuf и запускал check_face/nn_face.
В new: кадры идут потоком через `PushFrame`, ROCKIVA шлёт callback когда
готов результат. Таймер не нужен.

### 6.6 Уходит ручной трекинг (track)

`rockx_object_track` + `Users::tracklist()` синхронизировались с таймером.
ROCKIVA имеет встроенный трекер — `trackId` приходит в каждом callback.
`Users::tracklist()` упрощается (не нужно вызывать отдельно).

### 6.7 Полное сравнение API libPM (PercoMedia) до/после порта

**Принцип:** API `PercoMedia` (percomedia.h, cb.h) — **стабильный интерфейс**
между библиотекой и GUI. GUI (s30gui/AIService) общается с percomedia
**только** через этот API. Цель порта — **сохранить API неизменным**, заменив
только внутреннюю реализацию (rockx → ROCKIVA, rkmedia → RKMPI).

#### 6.5.1 API PercoMedia — БЕЗ ИЗМЕНЕНИЙ (стабильный контракт)

Все 17 функций `PercoMedia` (percomedia.h) **остаются без изменений** —
сигнатуры, типы, поведение. GUI не нужно переписывать.

| Функция | Назначение | Изменение |
|---------|-----------|----------|
| `PercoMedia()` / `~PercoMedia()` | ctor/dtor | Без изменений |
| `set_personid_cb(onPersonId)` | callback: распознан человек | Без изменений |
| `set_recheck_cb(onPersonId)` | callback: перепроверка | Без изменений |
| `set_lost_cb(onLost)` | callback: потерян текущий | Без изменений |
| `set_wdt_timeout_cb(onWDT)` | callback: watchdog | Без изменений |
| `set_motion_cb(onMotion)` | callback: движение | Без изменений |
| `setFaceCatchRect(QRect)` | зона детекции лиц | Без изменений |
| `setFaceCatch(bool)` | вкл/выкл зону | Без изменений |
| `motion_fake_trig()` | тестовый триггер движения | Без изменений |
| `signalHandler(int)` | обработчик сигналов | Без изменений |
| `face()` → QImage | текущий кадр лица (RGB) | Без изменений |
| `setNNRun(bool)` | старт/стоп сканирования | Без изменений |
| `resetCurrentUser()` | сбросить текущего | Без изменений |
| `getUserData()` → t_userdata | всё сразу: id, acc, photo, rect, feature | Без изменений |
| `getVector()` → vector<float> | feature vector текущего | Без изменений |
| `getRect()` → t_rect | bounding box текущего | Без изменений |
| `getId()` → int | track_id текущего | Без изменений |
| `setAcc(id, acc)` → bool | привязать аккаунт | Без изменений |
| `setPass(id, PassState)` → bool | отметить результат прохода | Без изменений |

#### 6.5.2 Типы cb.h — БЕЗ ИЗМЕНЕНИЙ

```c
typedef struct { int x; int y; } t_point;                    // без изменений
typedef struct { int left, top, right, bottom; } t_rect;    // без изменений
typedef struct {
    int track_id = -1;
    std::string acct = "";
    std::string photo = "";        // base64 PNG
    t_rect r = {0,0,0,0};
    std::vector<float> face;       // feature vector
} t_userdata;                                              // без изменений

typedef void (*onPersonId)(t_userdata d);  // без изменений
typedef void (*onLost)();                  // без изменений
typedef void (*onWDT)();                   // без изменений
typedef void (*onMotion)(bool st);         // без изменений
```

#### 6.5.3 Что меняется ВНУТРИ (реализация, не API)

API остаётся, но **внутренняя реализация** каждой функции меняется:

| Функция API | Old реализация | New реализация | Заметка |
|-------------|----------------|----------------|---------|
| `set_personid_cb` | rockx pipeline → emit | ROCKIVA callback → emit | Сигнатура та же |
| `face()` | `ImageBuf::lockPic` (rockx_image_t) | `RK_MPI_VI_GetChnFrame` → QImage | Внутри меняется |
| `getVector()` | `Users::user_list[id].list` (512 float) | То же, но feature из ROCKIVA (до 4096 байт) | Размер feature может отличаться |
| `getRect()` | `Users::user_list[id].rect` (rockx_rect_t) | То же, но rect из RockIvaFaceInfo | Конверсия RockIvaRect → t_rect |
| `getId()` | `Users::user_list[id].id` (trackId) | То же, но trackId из ROCKIVA | То же |
| `setNNRun(bool)` | `tscan->start/stop` + `onMute` | То же (tscan/onMute остаются) | Без изменений |
| `setAcc/setPass` | `Users::user_list.set_acc/pass` | То же | Без изменений |

#### 6.5.4 Критичные отличия в данных (внутри API)

**1. Feature vector размер:**
- Old: rockx `rockx_face_recognize` → 512 float
- New: ROCKIVA `RockIvaFaceAnalyseInfo.feature` → до 4096 байт
  (`ROCKIVA_FACE_FEATURE_SIZE_MAX`)
- **Влияние:** `t_userdata.face` (vector<float>) — тип тот же, но размер
  может отличаться. `Users::UserList` хранит `std::vector<float> list` —
  работает с любым размером.
- **Риск:** сравнение features (если делается вручную) — нужно
  использовать `ROCKIVA_FACE_FeatureCompare` вместо rockx-метода.
  Но в old коде сравнение делает S30 (сервер), не percomedia —
  percomedia только отдаёт feature в callback.

**2. Bounding box координаты:**
- Old: rockx_rect_t (left, top, right, bottom) в координатах кадра NN
  (720×1280 от RGA)
- New: RockIvaFaceInfo.box (x, y, width, height) в координатах кадра NN
  (540×960 от ROCKIVA)
- **Влияние:** `getRect()` возвращает `t_rect` — конверсия
  RockIvaRect → t_rect + масштабирование 540×960 → 800×1280 (дисплей)
- **Решение:** масштабировать координаты в callback (см. 6.3)

**3. Track ID:**
- Old: `rockx_object_track` → trackId (внешний трекер)
- New: `RockIvaFaceInfo.trackId` (встроенный трекер ROCKIVA)
- **Влияние:** `getId()` возвращает тот же int — тип тот же
- **Упрощение:** `Users::tracklist()` упрощается — ROCKIVA сама трекает

**4. Liveness:**
- Old: `rockx_face_liveness` (отдельный вызов, IR+RGB сравнение)
- New: `RockIvaFaceCapInfo.livenessResult` (встроено в callback)
- **Влияние:** liveness приходит в callback, не нужно отдельного вызова
- **В API:** liveness не exposed напрямую — влияет на `set_personid_cb`
  (только живые лица проходят)

**5. Качество и угол (quality/pose):**
- Old: `rockx_face_quality` + `rockx_face_pose` (отдельные вызовы)
- New: `RockIvaFaceQualityConfig` (пороги) + ROCKIVA фильтрует сама
- **Влияние:** фильтрация внутри ROCKIVA, не в percomedia
- **В API:** не exposed — влияет на `set_personid_cb` (только качественные
  лица проходят)

#### 6.5.5 Что НОВОГО в API (добавляется)

| Новая функция | Назначение | Причина |
|---------------|-----------|--------|
| `set_light_brightness(int)` | управление IR LED яркостью | Перенос подсветки из S30 в percomedia (см. 11.10) |
| `set_light_enable(bool)` | вкл/выкл IR LED (strobe) | Перенос подсветки из S30 в percomedia |
| `set_ircut(bool)` | вкл/выкл IR-cut фильтр | Если ircut переносится в percomedia |

**Это ДОБАВЛЕНИЯ к API**, не замены. Старые функции остаются.
GUI (s30gui) может использовать их опционально (ползунок яркости
IR подсветки → `set_light_brightness` напрямую, без round-trip через S30).

#### 6.5.6 Что УХОДИТ из API (недоступно на новой плате)

| Функция | Причина | Замена |
|---------|---------|-------|
| `set_facebox_cb` (закомментировано) | Не используется | — (уже убрано) |
| `set_qual_cb` (закомментировано) | Не используется | — (уже убрано) |
| `get_motion_st` (закомментировано) | Не используется | — (уже убрано) |

Все закомментированные функции в old API — **уже не часть API**.
Активный API (17 функций) полностью сохраняется.

#### 6.5.7 Сводная таблица: API стабильность

| Категория | Количество | Изменение |
|-----------|-----------|-----------|
| Callbacks (set_*_cb) | 5 | **Без изменений** |
| Getters (get*/face) | 5 | **Без изменений** (внутри — новая реализация) |
| Setters (set*) | 4 | **Без изменений** |
| Управление (setNNRun, reset, motion_fake) | 3 | **Без изменений** |
| **Итого активных** | **17** | **Все без изменений** |
| Новые (подсветка) | +3 | **Добавляются** (опционально) |
| Удалённые (закомм.) | 3 | **Уже не часть API** |

**Вывод:** API `PercoMedia` — **стабильный контракт** между percomedia и GUI.
При порте на RV1126B:
- **17 активных функций — без изменений** (GUI не переписывается)
- **Внутренняя реализация** меняется (rockx → ROCKIVA, rkmedia → RKMPI)
- **3 новые функции** добавляются (подсветка, ircut — перенос из S30)
- **3 закомментированные функции** — уже не часть API

Это означает, что **s30gui (AIService) можно перенести почти без изменений** —
он общается с percomedia через стабильный API. Меняется только
внутренняя реализация percomedia (ispclass, nnclass, userclass).

---

## 7. Порядок портирования (этапы)

### Этап 0: Заглушка libPM + сборка s30gui на RV1126B (1 день)

**Цель:** пересобрать s30gui для RV1126B (aarch64) с ROCKCHIP define,
используя заглушку libpercomedia.so вместо реальной percomedia.

#### 0.1 Что такое ROCKCHIP define (детальный разбор)

`ROCKCHIP` — это препроцессорный define, который **переключает режимы
компиляции** между «платой Rockchip» и «ПК разработчика». Это НЕ платформо-
зависимый код — это **условная компиляция** одних и тех же исходников.

**Где определяется** (s30gui.pro:161-172):
```pro
unix {
    equals(QT_ARCH, "arm") {
        DEFINES += ROCKCHIP          # ТОЛЬКО на arm!
        LIBS += -L$$OUT_PWD/../percomedia/ -lpercomedia
        INCLUDEPATH += $$PWD/../percomedia
    }
}
```
- **На arm Linux** (RV1109/1126, RV1126B): `QT_ARCH == "arm"` → ROCKCHIP
  определён → линкуется libpercomedia.so
- **На Windows** (x86/x64): `QT_ARCH == "x86_64"` → ROCKCHIP **НЕ** определён
  → libpercomedia **НЕ** линкуется
- **На ПК Linux** (x86/x64): то же — ROCKCHIP **НЕ** определён

**Влияние ROCKCHIP на каждый файл (проверено по коду):**

| Файл | Что делает ROCKCHIP | Без ROCKCHIP |
|------|---------------------|--------------|
| `main.cpp:46-56` | qputenv для linuxfb, DRM, DPI, touch | Пропускается (нет qputenv) |
| `mainwin.cpp:10-12` | `#include "percomedia.h"` | Не включает (лишний include) |
| `mainwin.cpp:46-55` | Frameless, geometry(0,0), transparent bg | geometry(1920,0), window deco |
| `aiservice.h` (26 блоков) | Все getters/setters вызывают libPM | Возвращают пустые значения |
| `perco.cpp:5-50` | Шрифты через PT(x) масштабирование | Фиксированные размеры шрифтов |
| `jsonservice.h:20-24` | `is_connected()` → `client != nullptr` | `is_connected()` → `true` |
| `jsonservice.h:82-85` | Ждёт подключения S30 | `ConnectChanged(true)` через 1 сек |
| `devicesrc.h:19-29` | `apply_display_backlight` пишет в sysfs | Пропускается |
| `devicesrc.h:77-81` | `req_conf` запрашивает S30 | Возвращает пустой через 2 сек |
| `svideo.h:25-28` | `AcessMode` ждёт S30 для mode | `CONTROL_MODE` через 1 сек |
| `userlistsrc.h` (8 блоков) | Запросы user_count/load к S30 | Тестовые данные ("acct N") |
| `eventlistsrc.h` (7 блоков) | Запросы event_count/load к S30 | Тестовые данные |
| `pinsrc.h:29-47` | Запрос pad к S30 | Читает `://pintest.txt` |
| `replacesrc.h:40` | `req_conf` запрашивает S30 | Возвращает пустой через 1 сек |

**Главный вывод:** Без ROCKCHIP s30gui работает в **режиме симуляции** —
`is_connected()` возвращает true, `ConnectChanged(true)` эмитится через 1 сек,
поэтому `setNNRun(true)` вызывается (aiservice.h:203), но libPM — пустышка,
поэтому распознавание ничего не делает. Все источники данных (users, events,
pads) возвращают тестовые данные.

#### 0.2 Можно ли собрать на Windows? ДА (но с оговорками)

**Что собирается на Windows БЕЗ проблем (без ROCKCHIP):**
- ✅ Весь s30gui компилируется (это Qt приложение, кроссплатформенное)
- ✅ Qt5 + widgets + scxml + websockets + svg — всё доступно на Windows
- ✅ WebSocket сервер (QWsServer) — Qt Network, кроссплатформенный
- ✅ UI сцены, SCXML, touch — Qt, кроссплатформенные
- ✅ Заглушка libPM (percomedia_stub.cpp) — чистый C++ + Qt

**Что НЕ собирается на Windows (нужен ROCKCHIP):**
- ❌ `percomedia.h` include в mainwin.cpp:11 — но он под `#ifdef ROCKCHIP`
- ❌ AIService getters к libPM — но они под `#ifdef ROCKCHIP`
- ❌ sysfs (`/sys/class/backlight/...`) — но под `#ifdef ROCKCHIP`
- ❌ linuxfb/DRM qputenv — но под `#ifdef ROCKCHIP`

**Итог:** Без ROCKCHIP (на Windows) **всё компилируется** — все платформо-
зависимые части изолированы `#ifdef ROCKCHIP`. Это и есть **режим ПК**,
в котором разработчики УЖЕ работают.

**Два варианта сборки на Windows:**

**Вариант A: Без ROCKCHIP (режим ПК, симуляция)**
- Условие: `QT_ARCH != "arm"` (Windows = x86_64)
- ROCKCHIP **не** определяется
- libpercomedia **не** линкуется
- Результат: s30gui запускается в режиме симуляции
  (тестовые данные, is_connected=true, setNNRun работает с пустышкой)
- **Цель:** проверить компиляцию, UI, SCXML, WebSocket

**Вариант B: С ROCKCHIP (принудительно, для теста порта)**
- Условие: добавить в .pro `DEFINES += ROCKCHIP` вручную
- Нужна заглушка libpercomedia (percomedia_stub.cpp → libpercomedia.so/dll)
- Результат: s30gui думает, что на плате, но libPM — пустышка
- **Проблема:** sysfs, linuxfb, DRM — не работают на Windows
  (но они под `#ifdef ROCKCHIP`, поэтому **выполнятся и упадут** в runtime)
- **Цель:** проверить, что код с ROCKCHIP компилируется (не runtime)

**Рекомендация для Windows:**
- Собирать **без ROCKCHIP** (Вариант A) — это рабочий режим ПК
- Если нужна проверка ROCKCHIP-кода — компилировать на **Linux x86_64**
  с принудительным `DEFINES += ROCKCHIP` + заглушкой libPM
- На Windows ROCKCHIP-режим **не имеет смысла** (sysfs/DRM/linuxfb мертвы)

#### 0.3 Что нужно для заглушки libPM (5 файлов)

| Файл | Действие | Строк |
|------|----------|-------|
| `percomedia.h` | Копия оригинала (без изменений) | 50 |
| `cb.h` | Копия оригинала (без изменений) | 33 |
| `percomedia_global.h` | Копия оригинала (без изменений) | 8 |
| `percomedia_config.h` | Копия оригинала (без изменений, нужны типы PassState, defines) | 153 |
| `percomedia_stub.cpp` | НОВАЯ — пустые реализации 17 функций | ~50 |

**percomedia_stub.cpp — скелет:**
```cpp
#include "percomedia.h"
#include "cb.h"

PercoMedia::PercoMedia() {}
PercoMedia::~PercoMedia() {}
void PercoMedia::set_personid_cb(onPersonId cb) {}
void PercoMedia::set_recheck_cb(onPersonId cb) {}
void PercoMedia::set_lost_cb(onLost cb) {}
void PercoMedia::set_wdt_timeout_cb(onWDT cb) {}
void PercoMedia::set_motion_cb(onMotion cb) {}
void PercoMedia::setFaceCatchRect(QRect r) {}
void PercoMedia::setFaceCatch(bool st) {}
void PercoMedia::motion_fake_trig() {}
void PercoMedia::signalHandler(int signum) {}
QImage PercoMedia::face() { return QImage(); }
void PercoMedia::setNNRun(bool st) {}
void PercoMedia::resetCurrentUser() {}
t_userdata PercoMedia::getUserData() { return t_userdata{}; }
std::vector<float> PercoMedia::getVector() { return {}; }
t_rect PercoMedia::getRect() { return {0,0,0,0}; }
int PercoMedia::getId() { return -1; }
bool PercoMedia::setAcc(int id, std::string acc) { return false; }
bool PercoMedia::setPass(int id, PassState en) { return false; }
```

**CMakeLists.txt для заглушки:**
```cmake
add_library(percomedia_stub SHARED percomedia_stub.cpp)
target_include_directories(percomedia_stub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(percomedia_stub Qt5::Core Qt5::Gui)  # QImage, QRect
set_target_properties(percomedia_stub PROPERTIES OUTPUT_NAME percomedia)
```
- `OUTPUT_NAME percomedia` — чтобы s30gui.pro нашёл `-lpercomedia`

#### 0.4 Стратегия сборки (3 платформы)

| Платформа | ROCKCHIP | libPM | Цель | Что проверяем |
|-----------|----------|-------|------|---------------|
| **Windows x64** | ❌ Нет | ❌ Не нужно | Компиляция UI + отладка вывода | Qt, SCXML, WebSocket, UI, мышь |
| **Linux x64** | ✅ Да (forced) | Заглушка | Компиляция ROCKCHIP | Код под #ifdef ROCKCHIP |
| **RV1126B (arm64)** | ✅ Да (auto) | Заглушка → реальная | Запуск на железе | Экран, touch, S30 |

**Шаги:**
1. **Windows:** собрать s30gui без ROCKCHIP (проверка компиляции UI)
2. **Linux x64:** собрать s30gui с ROCKCHIP + заглушка (проверка ROCKCHIP-кода)
3. **RV1126B:** собрать s30gui с ROCKCHIP + заглушка (запуск на железе)
4. Заменять заглушку на реальную percomedia поэтапно (этапы 1-6)

#### 0.5 Что можно проверить и отладить на Windows (вывод/UI)

**Принцип:** s30gui — это Qt приложение с QGraphicsView + SCXML.
Видео идёт через percomedia (DRM overlay, под ROCKCHIP), но **весь UI
рисуется Qt** — и он кроссплатформенный. На Windows можно отладить
**всё, что рисуется Qt**, без видео-фона.

**Что РАБОТАЕТ на Windows (можно отладить):**

| Что | Как | Доказательство |
|-----|-----|----------------|
| **Запуск GUI** | `main.cpp` без ROCKCHIP — окно на рабочем столе | main.cpp:59 `QApplication a` |
| **Все UI сцены** | 26 сцен через Factory + SCXML | mainwin.cpp:79-113 `ffactory.add<...>` |
| **SCXML state machine** | MenuState.scxml — переходы между сценами | mainwin.cpp:119 `mMachine.start()` |
| **Мышь как touch** | GView: mousePressEvent → TouchService | mainwin.h:33-48 |
| **Кнопки/меню** | SvgBtn, PixBtn, GList — Qt graphics items | components/*.h |
| **Текст/шрифты** | Perco::f_big и др. (фиксированные без ROCKCHIP) | perco.cpp:41-50 |
| **WebSocket сервер** | QWsServer на порту 3002 | jsonservice.h:75-76 |
| **Симуляция S30** | `is_connected()` → true, `ConnectChanged(true)` через 1 сек | jsonservice.h:20-24,84 |
| **Тестовые данные** | users "acct N", events, pads из pintest.txt | userlistsrc.h:262-265 |
| **Настройки (пустые)** | `req_conf` возвращает пустой DeviceData через 2 сек | devicesrc.h:77-81 |
| **Оверлеи режимов** | pic_overlay: Wait/Guard/Open/Closed/FireAlarm PNG | svideo.h:145,317-344 |
| **Полоска меню** | barbox (QGraphicsRectItem) сверху | svideo.h:157-162 |
| **Часы** | BarClock в полоске меню | svideo.h:178 |
| **Скриншот** | clock click → render scene → PNG | svideo.h:179-186 |
| **Popup сообщения** | PopupItem, msglist | svideo.h:151-152 |
| **Screensaver** | Screensaver (WORK/SLEEP) | svideo.h:287-310 |
| **Регистрация лица** | SFaceReg, SAddUser — UI без реального захвата | sfacereg.h, sadduser.h |
| **Список пользователей** | SUserList с тестовыми данными | suserlisrt.h |
| **Список событий** | SEventList с тестовыми данными | seventlist.h |
| **Настройки устройства** | SDevice (пустые, без S30) | sdevice.h |
| **Поиск лица** | SFaceSearch — UI | sfacesearch.h |

**Что НЕ работает на Windows (нужна плата):**

| Что | Почему |
|-----|--------|
| ❌ Видео-фон | percomedia → DRM overlay (под ROCKCHIP) |
| ❌ Прямоугольники лиц на видео | `AIService::getRect()` → libPM (под ROCKCHIP) |
| ❌ Реальное распознавание | `onFace` callback от libPM (под ROCKCHIP) |
| ❌ Реальные настройки | S30 не подключён (симуляция) |
| ❌ Реальные пользователи | Тестовые данные "acct N" |
| ❌ Реальные события | Тестовые данные |
| ❌ Touch экран | Но мышь работает как замена |
| ❌ Backlight | sysfs (под ROCKCHIP) |

**Ключевое: видео-фон vs UI-оверлеи**

SVideo — главная сцена. На плате:
- **Видео-фон** — percomedia пишет в DRM overlay (в обход Qt)
- **UI-оверлеи** — Qt рисует поверх: pic_overlay (PNG режимов), barbox
  (полоска меню), clock, popup, FaceItem (рамки лиц)

На Windows:
- **Видео-фона нет** (окно прозрачное/серое)
- **UI-оверлеи работают** — pic_overlay показывает Wait.png, barbox
  рисуется, clock работает, popup работает

**Что отладить на Windows (конкретные задачи):**

1. **Компиляция всех 26 сцен** — проверить, что нет ошибок компиляции
   на Windows (Qt5 + scxml + websockets + svg)
2. **SCXML state machine** — переходы между сценами (Video → Menu →
   AddUser → FaceReg → ...) — проверить логику переходов
3. **Мышь как touch** — GView перенаправляет mouse events в
   TouchService — проверить клики по кнопкам, скролл
4. **Оверлеи режимов** — pic_overlay показывает 5 PNG (Wait, Guard,
   Open, Closed, FireAlarm) — проверить переключение
5. **Полоска меню** — barbox + clock + burger/logo — проверить
   отображение
6. **Popup сообщения** — PopupItem + msglist — проверить показ
   сообщений
7. **Screensaver** — WORK/SLEEP переходы — проверить таймер
8. **Шрифты** — Perco::f_big и др. (фиксированные размеры без ROCKCHIP)
   — проверить читаемость
9. **WebSocket сервер** — порт 3002, проверить подключение (curl,
   wscat, браузер)
10. **Симуляция S30** — `is_connected()` → true через 1 сек —
    проверить, что `setNNRun(true)` вызывается (qDebug)
11. **Тестовые данные** — SUserList показывает "acct N", SEventList
    показывает тестовые события — проверить отображение
12. **Скриншот** — click по часам → PNG файл — проверить запись

**Особое: отладка вывода видео (когда будет percomedia)**

Когда заглушка будет заменена на реальную percomedia (этап 2-3),
видео пойдёт через DRM overlay. На Windows DRM нет, поэтому:
- **Видео нельзя отладить на Windows** — нужен Linux/RV1126B
- **Но UI-оверлеи можно отладить заранее** — они не зависят от видео

**Стратегия отладки вывода:**
1. **Windows:** отладить все UI-оверлеи (pic_overlay, barbox, clock,
   popup, FaceItem) — без видео-фона
2. **Linux x64:** отладить ROCKCHIP-код (sysfs, qputenv) — без реального
   видео, но с проверкой компиляции
3. **RV1126B:** отладить видео-фон + UI-оверлеи вместе — реальный вывод

#### 0.6 Что будет работать с заглушкой

**На Windows (без ROCKCHIP):**
- ✅ GUI запускается (окно на рабочем столе)
- ✅ UI сцены (Qt, SCXML) — все 26 сцен
- ✅ Мышь как touch (GView → TouchService)
- ✅ WebSocket сервер (порт 3002)
- ✅ Тестовые данные (users "acct N", events, pads из pintest.txt)
- ✅ `is_connected()` → true (симуляция S30)
- ✅ `setNNRun(true)` вызывается (но libPM пустышка)
- ✅ Оверлеи режимов (Wait/Guard/Open/Closed/FireAlarm PNG)
- ✅ Полоска меню, часы, popup, screensaver
- ✅ Скриншот (click по часам → PNG)
- ❌ Видео-фон (нужен DRM, под ROCKCHIP)
- ❌ Прямоугольники лиц (нужен libPM, под ROCKCHIP)
- ❌ Реальное распознавание (нужен libPM, под ROCKCHIP)

**На RV1126B (с ROCKCHIP + заглушка):**
- ✅ GUI запускается (main.cpp не блокируется)
- ✅ UI сцены (Qt, SCXML, touch)
- ✅ WebSocket сервер (порт 3002, ждёт S30)
- ✅ Дисплей backlight (sysfs)
- ✅ Сборка и запуск на реальном железе RV1126B
- ❌ Видео на экране (нет реального видеопайплайна)
- ❌ Распознавание лиц (нет реальной NN)
- ❌ Прямоугольники лиц (нет данных)
- ❌ Настройки от S30 (если S30 не подключён)

**Цель этапа 0 — доказать, что GUI-оболочка собирается и запускается
на RV1126B.** Это базис для всех следующих этапов — каждый этап будет
заменять заглушку на реальную реализацию.

**Что проверяем на Windows:**
- Компилируется ли s30gui (Qt5 + scxml + websockets + svg)
- Запускается ли GUI (окно на рабочем столе)
- Работают ли переходы SCXML (Video → Menu → AddUser → ...)
- Работает ли мышь как touch (клики по кнопкам)
- Отображаются ли оверлеи (Wait.png, полоска меню, часы)
- Работает ли WebSocket сервер (порт 3002)
- Работает ли симуляция S30 (is_connected → true)

**Что проверяем на RV1126B:**
- Компилируется ли s30gui на aarch64 (Qt5 cross-compile)
- Запускается ли на RV1126B (экран, touch, UI)
- Работает ли WebSocket сервер (порт 3002)
- Подключается ли S30 (если есть)

### Этап 1: Каркас + компиляция (1-2 дня)
1. Создать CMakeLists.txt (вместо .pro) для aarch64
2. Скопировать без изменений: percomedia.h/.cpp, cb.h, threadcontroller.*, dbg.*, process.*, motion_trigger.*, percomedia_config.*, json.hpp, drmconf.*
3. Адаптировать userclass.h (заменить rockx типы на свои)
4. Заглушки для ispclass/nnclass (пустые proc_init/proc_deinit)
5. Компиляция libpercomedia.so

### Этап 2: Видеопайплайн (2-3 дня)
1. isp_vi.cpp — 3 VI канала (RKMPI)
2. isp_vo.cpp — VO + DRM (RKMPI)
3. isp_rga.cpp — improcess для дисплея (im2d)
4. ispclass.cpp — draw_rects (перенос 1:1) + display loop
5. isp_enc.cpp — VENC JPEG (RKMPI)
6. Тест: видео на дисплее 800x1280 portrait

### Этап 3: Нейросеть (2-3 дня)
1. nnclass.cpp — ROCKIVA_Init + FACE_Init + PushFrame
2. Callback → Users::UserList (адаптировать)
3. Лицензия: загрузка через rkauth
4. Тест: детекция лиц, bounding box на дисплее

### Этап 4: Распознавание (1-2 дня)
1. ROCKIVA_FACE_FeatureCompare / SearchFeature
2. База пользователей (feature library)
3. Автомат состояний CHECK→NN→OK
4. Тест: распознавание в реальном времени

### Этап 5: Интеграция с GUI (1-2 дня)
1. Собрать s30gui (Qt для aarch64)
2. AIService — проверить callbacks
3. WebSocket сервер — тест с PERCo-S30
4. Тест на устройстве

### Этап 6: Полировка (1-2 дня)
1. Настройки экспозиции (rkaiq IPC)
2. IR LED управление (если есть)
3. Конфиг JSON — горячая перезагрузка
4. Watchdog
5. Производительность (fps, latency)

**Итого: ~10-14 дней** на полный порт.

---

## 8. Риски и открытые вопросы

1. **Лицензия ROCKIVA** — нужна валидная лицензия для FACE_Init. Без неё только
   BA (детекция без распознавания). Загрузка через `rkauth_tool_bin`.

2. **IR камера** — на RV1126B одна камера (GC2093). Liveness будет 2D (без IR).
   Если нужна IR — проверить наличие второго VI dev.

3. **Координаты** — ROCKIVA с `transformMode=ROTATE_90` даёт координаты в
   повёрнутой системе (portrait 960x540). Нужно масштабировать в 800x1280
   для draw_rects. Проверить эмпирически.

4. **Feature vector** — old rockx давал 512 float. ROCKIVA — до 4096
   (`ROCKIVA_FACE_FEATURE_SIZE_MAX`). База пользователей — пересоздать.

5. **База пользователей** — old хранила feature в `user.list` (std::vector<float>).
   ROCKIVA имеет `FeatureLibraryControl` (встроенная база). Решить: использовать
   встроенную или свою.

6. **Производительность** — ROCKIVA на 960x540 (маленький кадр) должна быть
   быстрее, чем old rockx на 720x1280. Проверить fps.

7. **Qt на aarch64** — нужен Qt 5/6 для aarch64. Проверить наличие в SDK.

---

## 9. Масштабирование координат и размеров (КРИТИЧНО!)

### 9.1 Проблема масштабирования NN → дисплей

**Old (работает «как есть»):**
- NN кадр: 720×1280 (после rot270)
- Дисплей: 800×1280
- Координаты боксов из rockx в 720×1280
- В draw_rects: `VO_DISP_W - u.rect.right` = `800 - rect.right(≤720)`
- **Нет явного масштабирования** — работает потому что 720≈800 (разница 10%)
  и по высоте 1280=1280 точно

**New (нужно масштабирование):**
- ROCKIVA кадр: 960×540 (landscape) → `transformMode=ROTATE_90` → **540×960** (portrait)
- Дисплей: 800×1280
- Координаты боксов из ROCKIVA callback в **540×960**
- **Масштаб ЯВНО нужен:**
  - X: 540 → 800 (множитель **1.481**)
  - Y: 960 → 1280 (множитель **1.333**)

**Два варианта решения:**

**Вариант A: Масштабировать координаты в callback**
```c
void on_face_det(const RockIvaFaceDetResult *result, status, userdata) {
    for (int i = 0; i < result->num; i++) {
        RockIvaFaceInfo *info = &result->faceInfo[i];
        // Масштабирование 540×960 → 800×1280
        float sx = (float)VO_DISP_W / 540;  // 1.481
        float sy = (float)VO_DISP_H / 960;  // 1.333
        t_rect r = {
            (int)(info->faceRect.left   * sx),
            (int)(info->faceRect.top    * sy),
            (int)(info->faceRect.right  * sx),
            (int)(info->faceRect.bottom * sy)
        };
        // → user_list
    }
}
```

**Вариант B: Подать в ROCKIVA кадр 800×1280 (повёрнутый)**
- VI ch_NN: 1280×800 (landscape) → rot90 → 800×1280 (portrait)
- Координаты сразу в 800×1280 — масштаб не нужен
- Минус: кадр больше (800×1280 vs 960×540), ROCKIVA медленнее

**Рекомендация:** Вариант A (масштабировать координаты) — быстрее, проще.
Масштабирование — 4 умножения на бокс, пренебрежимо.

### 9.2 Размеры кадра для ROCKIVA

**ROCKIVA `imageInfo` (при `ROCKIVA_Init`):**
```c
globalParams.imageInfo.width = 960;   // landscape, как из VI
globalParams.imageInfo.height = 540;
globalParams.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
globalParams.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_ROTATE_90;
// После поворота: 540×960 (portrait)
```

**VI ch_NN:** 960×540 NV12 (маленький, как в rkipc rv1126b_ipc).
VI сам масштабирует 1920×1080 → 960×540 через ISP.

### 9.3 Размеры для дисплея (RGA improcess)

```c
// VI ch_VO: 1920×1080 NV12 (полный кадр)
// RGA improcess: rot90 + scale 1920×1080 → 800×1280 ARGB8888
rga_buffer_t src = wrapbuffer_fd(vi_fd, 1920, 1080, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_fd(vo_fd, 800, 1280, RK_FORMAT_RGBA_8888);
im_rect src_rect = {0, 0, 1920, 1080};
im_rect dst_rect = {0, 0, 800, 1280};
im_opt_t opt; opt.rotate_mode = ROTATE_90;
improcess(src, dst, src_rect, dst_rect, &opt, IM_ALPHA_BLEND_DST_OVER);
```

**Важно:** RGA делает scale 1920×1080 → 800×1280 + rot90 за один проход.
Соотношение сторон: 1920/1080 = 1.778, 800/1280 = 0.625.
После rot90: 1080×1920 → 800×1280, масштаб 0.741×0.667 — лёгкое растяжение.

### 9.4 Размеры для VENC (JPEG)

```c
// VI ch_ENC: 1920×1080 NV12 → VENC JPEG 1920×1080
// Без поворота (landscape, как в old)
```

---

## 10. Двухкамерность — критический вопрос архитектуры

### 10.1 В old: ДВЕ камеры (RGB + IR) — зафиксировано везде

Двухкамерность пронизывает **весь код** percomedia (см. Документ 1, раздел 12.2):
- VI: 2 канала (VI0=RGB, VI1=IR)
- RGA: 2 канала для NN (RGA1=RGB, RGA2=IR)
- ImageBuf: 2 буфера (bRGB, bIR), синхронный `lockPic(ir, rgb)`
- Users: 2 массива трекинга (tracked_face_array_rgb/ir)
- FaceRecogn::check_face: `rockx_face_quality` × 2 (RGB + IR), ОБА должны PASS
- Users::tracklist: проверка `rgb.count == ir.count`, `rgb_score && ir_score`
- VENC: 2 энкодера (ENC0=RGB 1fps, ENC1=IR 10fps)
- Конфиг: `FPS_IR_*`, `FPS_RGB_*` отдельно

**Назначение IR камеры:** liveness detection (живой ли человек) —
IR не зависит от освещения, видит тепло. RGB+IR вместе = надёжный liveness.

### 10.2 На RV1126B: ОДНА камера (GC2093) — нужно решить

**Вопрос:** есть ли на целевой плате RV1126B вторая IR камера?

**Если ОДНА камера (только RGB):**
- Liveness будет **2D** (через `faceLivenessEnable=1` в ROCKIVA)
- 2D liveness менее надёжный, чем RGB+IR (можно обманеть фото/видео)
- Уходит: VI1, RGA2, bIR, tracked_face_array_ir, ir_obj, ENC1
- Упрощается: check_face (один quality вместо двух), tracklist (без синхронизации)
- **Убирается ~30% логики** — нужно адаптировать Users, ImageBuf, FaceRecogn

**Если ДВЕ камеры (RGB + IR):**
- Можно использовать ROCKIVA liveness 2D + своя проверка IR
- Но ROCKIVA `PushFrame` принимает один кадр — не поддерживает пару RGB+IR
- Придётся: ROCKIVA для RGB (распознавание) + своя IR проверка (через RGA + rockx?)
- **Сложнее** — ROCKIVA не заточена под двухкамерный liveness

### 10.3 Рекомендация по двухкамерности

**Для минимального порта (одна камера):**
1. Убрать IR-зависимости: VI1, RGA2, bIR, tracked_face_array_ir, ir_obj, ENC1
2. `check_face`: убрать IR quality (только RGB quality через ROCKIVA)
3. `tracklist`: убрать синхронизацию rgb/ir (только rgb массив)
4. Liveness: `faceLivenessEnable=1` в ROCKIVA (2D)
5. VENC: один энкодер (RGB JPEG)

**Убираемые файлы/код:**
- `isp_vi.cpp`: убрать VI1 (остаётся VI0 + ch_NN + ch_ENC)
- `isp_rga.cpp`: убрать RGA2 (IR для NN)
- `imagebuf.cpp`: убрать `bIR`, `IR()`, упростить `lockPic` (только rgb)
- `userclass.h`: убрать `tracked_face_array_ir`, `ir_obj`, `ir_objprt`
- `userclass.cpp`: убрать IR-часть из `tracklist`
- `nnclass.cpp`: убрать IR quality из `check_face`
- `isp_enc.cpp`: убрать ENC1 (IR JPEG)

**Объём изменений:** ~200 строк удалится, ~50 строк адаптации.

### 10.4 Альтернатива: оставить двухкамерность

Если на плате есть IR камера и liveness критичен:
- ROCKIVA для RGB (распознавание + 2D liveness)
- Отдельный pipeline для IR: VI1 → RGA → своя проверка (blur/score)
- Синхронизация по timestamp (как в old ImageBuf)
- **Сложнее**, но liveness надёжнее

**Решение нужно принять до начала порта** — влияет на архитектуру.

---

## 11. IR LED / подсветка — НЕ ПОРТИРОВАТЬ (мёртвый код)

### 11.1 Что нашли в old коде

**ДВА механизма управления IR подсветкой, ОБА НЕ РАБОТАЮТ:**

#### A. GPIO-импульсы (userclass.h, строки 455-488) — ЗАКОММЕНТИРОВАНО

```c
/*  void ir_led_tick(){
      if(ir_led != nullptr){
          if(!motion_st) ir_led->value(0);
          else {
              cr ++; cr %= period;                  // period=20
              ir_led->value((cr < ccr)?1:0);         // ШИМ 50%, 200ms период
          }
      } else {
          ir_led = new pad(IR_PAD);                   // pad НЕ определён в проекте!
          ...
      }
  }*/
```

**Проблемы:**
- Весь блок в `/* ... */` — **закомментирован**
- Класс `pad` — **НЕ определён** в проекте (нет `class pad` нигде в `s30guiproj`)
- `IR_PAD` — **НЕ определён** (нет `#define IR_PAD`)
- Таймер `ledtim` в конструкторе `Users()` — **закомментирован**
- **Код никогда не компилировался и не работал**

**Что пытался сделать автор (невнятная идея):**
- Таймер 10ms дёргает GPIO IR LED по ШИМ (50% скважность, период 20 тиков = 200ms)
- Светить только при `motion_st == true` (есть движение)
- Видимо, идея была «мигать IR фонариком синхронно с кадрами» — но
  **привязки к кадрам нет**, просто ШИМ с фиксированным периодом
- Зачем — непонятно (экономия питания? анти-засветка сенсора?)

#### B. CPSL через rk_aiq (ispll.cpp, строки 512-546) — ОПРЕДЕЛЕНО, НЕ ВЫЗЫВАЕТСЯ

```c
RK_S32 Isp_LL::SAMPLE_COMM_ISP_SET_OpenColorCloseLed(RK_S32 CamId);  // выключить IR
RK_S32 Isp_LL::SAMPLE_COMM_ISP_SET_GrayOpenLed(RK_S32 CamId, RK_U8 strength);  // вкл IR
```

- Функции **определены** в ispll.cpp и объявлены в ispll.h
- **НИКТО ИХ НЕ ВЫЗЫВАЕТ** — `grep` по всему `s30guiproj` находит только определения
- Это API rk_aiq для управления IR/LED через ISP (CPSL = Color/light Source)
- `GrayOpenLed(strength)` — включить IR с заданной силой
- `OpenColorCloseLed()` — выключить IR, перейти в цветной режим

### 11.2 Вывод: управление IR подсветкой в old percomedia ОТСУТСТВУЕТ в runtime

IR фонарь (если есть аппаратно) управляется **вне percomedia**:
- Отдельным процессом/драйвером
- Либо не управляется вообще (всегда вкл/выкл аппаратно)
- Либо через `ispserver` (если он запущен — он сам рулит ISP и подсветкой)

### 11.3 План для порта: НЕ ПОРТИРОВАТЬ управление подсветкой

**Рекомендация: вообще не портировать управление IR LED.**

**Причины:**
1. **Код мёртвый** — никогда не работал, нет даже определения класса `pad`
2. **Идея сомнительная** — ШИМ на IR фонаре без реальной синхронизации с кадрами
   не даёт преимуществ, только мерцание
3. **Бесплатный геморрой** — портировать мёртвый код = создавать проблему из ничего
4. **На RV1126B** — если IR подсветка нужна, управлять через:
   - `rkaiq_server` (CPSL API, как в `rkipc`)
   - Либо через sysfs GPIO напрямую (отдельный простой модуль, не в percomedia)
   - Либо вообще не управлять (аппаратный always-on)

**Что убрать из порта:**
- `userclass.h`: закомментированный блок `ir_led_tick` / `onLedTick` — **не копировать**
- `userclass.cpp`: закомментированный таймер `ledtim` — **не копировать**
- `ispll.h/.cpp`: `SAMPLE_COMM_ISP_SET_*Led` функции — **не копировать**
  (всё равно `ispll.cpp` целиком уходит, см. раздел 4.3)

**Если IR подсветка реально нужна на устройстве:**
- Вынести в отдельный простой модуль (50 строк): sysfs GPIO on/off по motion
- Или использовать `rk_aiq_uapi_sysctl_setCpsLtCfg` через rkaiq_server
- **Не встраивать в percomedia** — это отдельная ответственность

### 11.4 Связь с MotionTrigger

`MotionTrigger` (см. док 1, раздел 7.4) — детектор движения по трекингу.
В old коде `motion_st` использовался бы для включения IR LED (в закомментированном коде).
В порте `MotionTrigger` **остаётся** (он используется для других целей —
активация сканирования), но **не связывается с IR LED**.

Если IR LED нужен по движению — связать `MotionTrigger::motion_changed`
с отдельным GPIO модулем (не в percomedia).

### 11.5 Гипотеза: ШИМ как регулятор яркости (НЕ синхронизация с кадрами)

**Пользовательская гипотеза (подтверждена частично):**
«мигания IR фонарика — это попытка регулировать яркость через ШИМ,
а не синхронизация с кадрами. Яркость задана в Qt UI, но физически
мощность фонаря не регулируется напрямую — только через ШИМ.»

**Проверка в old коде:**

1. **В Qt UI (s30gui) есть 3 ползунка яркости** (`sscreen.h`):
   ```c
   display_backlight = new GSlider(r1, "Яркость подсветки экрана", d.licon_light);
   rgb_backlight = new GSlider(r2, "Яркость RGB подсветки", d.led_white_level);
   ir_backlight = new GSlider(r3, "Яркость IR подсветки", d.led_ir_level);
   ```
   - `licon_light` (0-100) — дисплей, **применяется физически** через
     `/sys/class/backlight/backlight/brightness` (`apply_display_backlight`)
   - `led_white_level` (0-10) — RGB подсветка, **НЕ применяется физически в s30gui**
   - `led_ir_level` (0-8) — IR подсветка, **НЕ применяется физически в s30gui**

2. **`led_ir_level` и `led_white_level` только сохраняются в JSON и
   отправляются на WebSocket сервер** (PERCo-S30):
   ```c
   void on_ir(int val){
       d.led_ir_level.setVal(val);
       jo["led_ir_level"] = d.led_ir_level.val();
       send(jo);  // → WebSocket → сервер
   }
   ```
   Сервер PERCo-S30 (внешний процесс) применяет их через свой механизм.

3. **В percomedia (библиотека) — мёртвый код ШИМ** (см. 11.1):
   ```c
   cr ++; cr %= period;                  // period=20
   ir_led->value((cr < ccr)?1:0);         // ccr=10 → ШИМ 50%
   ```
   - `ccr` и `period` — **константы** (10 и 20), НЕ настраиваются из `led_ir_level`
   - **Нет связи** между `led_ir_level` (0-8 из UI) и `ccr` (10 в коде)
   - ШИМ **не реализует** регулировку яркости из UI — это просто 50% скважность

**Вывод по гипотезе:**
- **Часть про «регулировку яркости через UI» — верно**: в Qt UI есть ползунок
  «Яркость IR подсветки» (0-8), он отправляется на сервер
- **Часть про «ШИМ как регулятор» — НЕ подтверждена**: закомментированный код
  ШИМ в percomedia **не связан** с `led_ir_level` из UI, `ccr` константа
- **Реальная регулировка яркости IR LED** в old системе происходит **вне percomedia**
  (сервер PERCo-S30 или отдельный процесс), не через этот мёртвый код

**Что это значит для порта:**
- Мёртвый код ШИМ в percomedia **точно не портировать** — он не реализует
  регулировку яркости, это просто незаконченный эксперимент автора
- Регулировку яркости IR LED нужно реализовать **заново** на новой плате,
  через правильный API (см. 11.6)

### 11.6 Как управлять IR LED на RV1126B (новая плата)

**Аппаратное обеспечение RV1126B (из README.md):**
- Камерный модуль **BFC105-DUAL-L** — OEM-модуль с двумя GC2093 и
  **встроенной IR-подсветкой** («L» = с IR-подсветкой)
- Cam0 (0x37) — IR матрица (монохром, `module-name="IR"`)
- Cam1 (0x7e) — цветная матрица (`module-name="default"`)
- **IR LED физически встроен в камерный модуль** (не отдельный компонент платы)

**Программное управление IR LED в SDK rkipc (rv1126b):**

1. **PWM драйвер** (`rk_pwm.c`):
   ```c
   int rk_pwm_init(uint32_t pwm, uint32_t period, uint32_t duty, enum pwm_polarity);
   int rk_pwm_set_duty(uint32_t pwm, uint32_t duty);   // регулировка яркости!
   int rk_pwm_set_enable(uint32_t pwm, bool enabled);
   ```
   Через sysfs `/sys/class/pwm/pwmchipN/pwmM/duty_cycle` — **аппаратный ШИМ**
   (не софтверный как мёртвый код в old percomedia).

2. **rkipc API** (`isp.c` для rv1126b):
   ```c
   int rk_isp_set_light_strength(uint32_t pwm, uint32_t period, uint32_t duty, enum pwm_polarity);
   int rk_isp_close_light(uint32_t pwm);
   int rk_isp_set_light_brightness(int cam_id, int value);  // value → duty
   int rk_isp_enable_ircut(bool on);                        // IR-cut фильтр
   ```

3. **В rv1126b версии rkipc `rk_isp_set_light_strength` ЗАКОММЕНТИРОВАНА** (isp.c:607-610):
   ```c
   int rk_isp_set_light_brightness(int cam_id, int value) {
       // pwm = 3;
       // period = 10000;
       // duty = 5000;
       // ret = rk_isp_set_light_strength(pwm, period, duty, PWM_POLARITY_NORMAL);
       rk_param_set_int(entry, value);  // только сохраняет в ini
   }
   ```
   **На RV1126B PWM управление IR LED отключено в rkipc** — только сохраняется
   настройка `light_brightness` в ini-файл. Возможно, на этой плате IR LED
   управляется иначе (или не управляется программно вообще).

4. **ircut GPIO** — в rv1126b ini **НЕТ** `ircut_open_gpio`/`ircut_close_gpio`
   (в отличие от rv1126/rv1106). На RV1126B ircut может управляться через
   rk_aiq (CPSL), а не через GPIO напрямую.

**Вывод для порта:**
- На RV1126B **IR LED встроен в камерный модуль BFC105-DUAL-L**
- Управление IR LED через **аппаратный PWM** (`rk_pwm_*` API) —
  это правильно (не софтверный ШИМ как в old мёртвом коде)
- В rkipc для rv1126b PWM-управление **отключено** — нужно проверить
  на реальной плате, есть ли `/sys/class/pwm/` и какой PWM-канал
  подключён к IR LED
- **Альтернатива**: через rk_aiq CPSL API (`rk_aiq_uapi2_sysctl_setCpsLtCfg`)
  как в old ispll.cpp (но там тоже не вызывалось)

**Рекомендация для порта:**
1. **Проверить на плате** наличие `/sys/class/pwm/pwmchip*/` и какой канал
   подключён к IR LED (через DTS или экспериментально)
2. Если PWM есть — использовать `rk_pwm_*` API напрямую (50 строк):
   ```c
   rk_pwm_init(pwm_channel, period_ns, duty_ns, PWM_POLARITY_NORMAL);
   rk_pwm_set_enable(pwm_channel, true);
   // регулировка яркости: rk_pwm_set_duty(pwm_channel, duty_ns);
   ```
3. Если PWM нет — проверить GPIO IR LED через sysfs
   (`/sys/class/gpio/gpioN/value`)
4. **Не встраивать в percomedia** — вынести в отдельный модуль или
   использовать rkipc/ rkaiq_server
5. Привязать к `led_ir_level` (0-8) из Qt UI через WebSocket →
   отдельный процесс → PWM/GPIO

**Главное:** старый мёртвый код ШИМ — **не портировать**.
На новой плате IR LED управляется через **аппаратный PWM** (если есть),
а не софтверным дёрганьем GPIO по таймеру.

### 11.7 Приведёт ли PWM к мерцанию? (критично для потока кадров)

**Вопрос:** «PWM приведёт к морганиям? Для потока кадров критично, чтобы
подсветка была равномерна.»

**Короткий ответ: НЕТ, аппаратный PWM на высокой частоте НЕ даёт мерцания.
Старый софтверный ШИМ — ДА, привёл бы к мерцанию (ещё один аргумент против портирования).**

#### Проблема мерцания PWM + камера

Известная проблема в IPC (IP cameras): PWM LED + камера = **полосы/стробоскопический эффект**,
если частота PWM сопоставима с частотой кадров или экспозицией.

**Механизм:**
- Камера захватывает кадр за время экспозиции (shutter time)
- Если за это время PWM проходит **целое число периодов** — засветка равномерна
- Если **не целое** — часть кадра засвечена больше, часть меньше → **полосы**
- Если PWM медленнее экспозиции — видны явные мерцания

#### Сравнение: старый софтверный ШИМ vs аппаратный PWM

| Параметр | Старый софтверный ШИМ (мёртвый код) | Аппаратный PWM (rk_pwm / VI_LIGHT) |
|----------|--------------------------------------|-------------------------------------|
| **Период** | 20 тиков × 10мс = **200 мс** | 10000 нс = **10 мкс** (rv1126b) |
| **Частота** | **5 Гц** (200 мс период) | **100 кГц** (10 мкс период) |
| **Периодов за экспозицию 1/30 сек** | 33 мс / 200 мс = **0.165** (доли периода!) | 33 мс / 10 мкс = **3300** периодов |
| **Равномерность засветки** | ❌ **КРИТИЧНО НЕРАВНОМЕРНО** — камера видит полосы | ✅ **Равномерно** — 3300 периодов усредняются |
| **Видимое мерцание** | ❌ **Да** (5 Гц — видно глазом и камерой) | ✅ **Нет** (100 кГц — выше видимого диапазона) |
| **Синхронизация с кадрами** | ❌ Нет (таймер 10мс не связан с кадрами) | ✅ Аппаратная (через VI_LIGHT_CTL_PARAM_S) |

**Вывод:** старый софтверный ШИМ на 5 Гц — **катастрофически плох** для камеры:
- 5 Гц << 30 fps (частота кадров)
- За время экспозиции проходит **доля периода** PWM → каждый кадр имеет
  разную засветку → **сильные полосы и мерцание**
- Это **никогда не работало** (код мёртвый), и слава богу — иначе изображение
  было бы испорчено

**Аппаратный PWM на 100 кГц** — **не даёт мерцания**:
- 100 кГц >> 30 fps (в 3333 раз выше)
- За время экспозиции 1/30 сек проходит **3300 периодов** PWM → полная
  равномерная засветка
- Выше видимого мерцания (>100 Гц для глаза, у нас 100 кГц — в 1000 раз выше)

#### Лучший вариант: VI_LIGHT_CTL_PARAM_S (аппаратная синхронизация с VI)

В RKMPI есть **встроенная структура для управления подсветкой на уровне VI**
(`Rockchip_Developer_Guide_MPI_CN.md`, раздел 6.57):

```c
typedef enum _rkVILightType {
    LIGHT_TYPE_PWM,   // PWM-управляемая подсветка
    LIGHT_TYPE_GPIO,  // GPIO-управляемая подсветка
} VI_LIGHT_TYPE_E;

typedef struct rkVILightParam {
    RK_U8  light_type;    // LIGHT_TYPE_PWM или LIGHT_TYPE_GPIO
    RK_U8  light_enable;  // включить/выключить
    RK_U64 duty_cycle;    // PWM duty cycle (скважность → яркость)
    RK_U64 period;        // PWM period
    RK_U32 polarity;      // полярность
} VI_LIGHT_CTL_PARAM_S;
```

**Преимущества VI_LIGHT_CTL_PARAM_S:**
1. **Аппаратная синхронизация с VI** — PWM управляется на уровне видеовхода,
   потенциально синхронизировано с захватом кадров в SoC
2. **Не требует отдельного процесса** — встроено в RKMPI pipeline
3. **Регулировка яркости через duty_cycle** — `led_ir_level` (0-8) → duty
4. **Высокая частота** — аппаратный PWM в SoC, не софтверный

**Это лучший вариант для percomedia порта** — управление подсветкой
встроено в видеопайплайн, синхронизировано с кадрами аппаратно.

#### Рекомендация: использовать VI_LIGHT_CTL_PARAM_S

```c
// В isp_vi.cpp при настройке VI канала:
VI_LIGHT_CTL_PARAM_S lightParam = {
    .light_type = LIGHT_TYPE_PWM,
    .light_enable = 1,
    .duty_cycle = led_ir_level * 1000,  // 0-8 → 0-8000 (регулировка яркости)
    .period = 10000,                    // 10 мкс = 100 кГц (равномерно для камеры)
    .polarity = 0
};
RK_MPI_VI_SetChnLightParam(pipe_id, ch, &lightParam);  // если есть такой API
```

**Если VI_LIGHT_CTL_PARAM_S недоступен** (нет API в RKMPI):
- Использовать `rk_pwm_*` через sysfs (100 кГц, без синхронизации с VI,
  но 3300 периодов за экспозицию — достаточно равномерно)
- Проверить на плате `/sys/class/pwm/pwmchip*/` и какой канал к IR LED

#### Итог по мерцанию

| Вариант | Мерцание для камеры | Равномерность | Рекомендация |
|---------|---------------------|---------------|--------------|
| Старый софтверный ШИМ (5 Гц) | ❌ Сильное | ❌ Полосы | **НЕ портировать** (мёртвый код) |
| Аппаратный PWM sysfs (100 кГц) | ✅ Нет | ✅ Равномерно | Допустимо |
| VI_LIGHT_CTL_PARAM_S (RKMPI) | ✅ Нет | ✅ Идеально | **Лучший вариант** |
| GPIO on/off (без PWM) | ✅ Нет | ✅ Равномерно | Если не нужна регулировка яркости |

**Главное:**
1. PWM на 100 кГц **НЕ даёт мерцания** — 3300 периодов за экспозицию
2. Старый софтверный ШИМ на 5 Гц **ДАЛ бы мерцание** — это ещё один аргумент
   против портирования мёртвого кода
3. Для равномерности: частота PWM >> 1/время_экспозиции.
   100 кГц >> 30 Гц (в 3333 раз) — более чем достаточно
4. **Лучший вариант** — `VI_LIGHT_CTL_PARAM_S` через RKMPI
   (аппаратная синхронизация с VI)
5. Если регулировка яркости не нужна — простой GPIO on/off
   (вообще без PWM, всегда равномерно)

### 11.8 Реальное поведение IR LED на old плате (наблюдение пользователя)

**Наблюдение:** «вижу на old что регулировка яркости IR фонарика работает!
мы не знаем кто её регулирует но! важный момент! она включается только
при наличии движения в кадре! я вижу импульсы глазом примерно 5 раз в секунду»

**Анализ кода — кто управляет IR LED на old плате:**

1. **percomedia (библиотека) — НЕ управляет** (мёртвый код, см. 11.1)
2. **s30gui (Qt UI) — НЕ управляет физически**:
   - `led_ir_level` (0-8) только сохраняется в JSON, отправляется на сервер
   - `apply_display_backlight` применяет ТОЛЬКО `licon_light` (экран)
   - Нет кода записи в `/sys/class/gpio/` или `/sys/class/pwm/`
3. **Управляет сервер PERCo-S30** (внешний процесс, не в репозитории):
   - GUI отправляет события motion на сервер:
     ```c
     // mainwin.cpp:230 — при НАЧАЛЕ движения
     JsonService::Instance()->onTransmit(
         "{\"event\":\"face\",\"face\":{\"name\":\"on_motion\"}}");
     // mainwin.cpp:234 — при ОКОНЧАНИИ движения (через 10 сек)
     JsonService::Instance()->onTransmit(
         "{\"event\":\"face\",\"face\":{\"name\":\"off_motion\"}}");
     ```
   - Сервер получает `on_motion`/`off_motion` и включает/выключает IR LED
   - Сервер также получает `led_ir_level` (0-8) для регулировки яркости

**MotionTrigger — логика срабатывания** (`motion_trigger.h`):
```c
qint64 motion_timeout = 10000; // 10 секунд — motion активен 10 сек после движения
float motion_limit = 0.005;    // порог движения (0.5% периметра bbox)
// tmotion->setInterval(500)    // проверка timeout каждые 500 мс (2 Гц)
// tscan->setInterval(100)     // сканирование NN каждые 100 мс (10 Гц)
```
- `motion_changed` эмитится **только при переходе** (false→true, true→false)
- НЕ каждый кадр, а 2 события: начало/конец движения
- Значит **5 Гц импульсов — это НЕ от motion_changed**

**Гипотеза: 5 Гц = strobe-подсветка синхронно с кадрами NN**

Наиболее вероятное объяснение «5 импульсов в секунду при движении»:
- Сервер PERCo-S30 включает IR LED в **strobe-режиме** — вспышки
  синхронно с захватом кадров для распознавания
- Реальная частота NN сканирования ~5-10 fps (не 10 Гц точно, т.к.
  обработка может занимать >100мс)
- IR LED вспыхивает на момент экспозиции кадра → каждый кадр идеально
  засвечен, но между кадрами LED выключен (экономия энергии)

**Почему именно strobe (а не постоянный PWM):**
1. **Экономия энергии** — LED включён только ~10% времени (при 5 fps)
2. **Не слепит пользователя** — постоянный IR раздражает (хоть и не виден
   напрямую, но виден красный glow), strobe менее заметен
3. **Идеальная засветка кадра** — вспышка синхронна с shutter, кадр
   получает равномерную засветку (нет полос PWM)
4. **Синхронизация с motion** — LED включается только когда нужно
   распознать лицо (по motion), не работает впустую

**Это объясняет всё наблюдаемое поведение:**
- «5 раз в секунду» = частота кадров NN (~5 fps, реальная)
- «только при движении» = strobe активируется по `on_motion` событию
- «регулировка яркости работает» = `led_ir_level` (0-8) задаёт
  **скважность или силу вспышки** на сервере
- «вижу импульсы глазом» = 5 Гц ниже частоты слияния глазом (~50-60 Гц),
  поэтому видны как мерцание

### 11.9 Что это значит для порта на RV1126B

**Два варианта реализации IR LED на новой плате:**

#### Вариант A: Постоянный PWM (рекомендация из 11.7)
- PWM на 100 кГц, постоянная засветка
- Плюсы: равномерно для камеры, не видимо глазом
- Минусы: больше энергопотребление, постоянный red glow

#### Вариант B: Strobe синхронно с кадрами (как на old плате) — РЕКОМЕНДУЕТСЯ
- IR LED вспыхивает синхронно с захватом кадра для распознавания
- Активируется только при motion (как на old)
- Частота вспышек = частота NN сканирования (~5-10 Гц)
- Плюсы: экономия энергии, не слепит, идеальная засветка каждого кадра
- Минусы: видимо глазом как мерцание (но это норма для IPC)

**Реализация strobe на RV1126B:**

1. **Через VI_LIGHT_CTL_PARAM_S** (если поддерживается):
   - Настроить PWM с коротким duty (вспышка) и длинным period
   - Синхронизировать с VI захватом (аппаратно)
   - Включать/выключать по motion

2. **Через callback перед захватом кадра** (софтверный strobe):
   ```c
   // В callback перед ROCKIVA_PushFrame:
   void on_before_nn_frame() {
       if (motion_active) {
           rk_pwm_set_enable(ir_led_pwm, true);   // вспышка
           // ... захват кадра ...
           // после получения кадра:
           rk_pwm_set_enable(ir_led_pwm, false);  // выкл
       }
   }
   ```
   Но это даёт дрожание тайминга (софтверная синхронизация).

3. **Через GPIO + V4L2 VBLANK синхронизацию** (продвинутый):
   - Использовать V4L2 события vsync для точной синхронизации
   - Включать LED перед экспозицией, выключать после

**Рекомендация для порта:**
1. **Сохранить strobe-режим** (как на old) — это умное решение
2. Привязать к MotionTrigger (уже есть в percomedia) — `motion_changed`
   → включить/выключить strobe
3. Реализовать через **VI_LIGHT_CTL_PARAM_S** (аппаратная синхронизация)
   или через callback перед `ROCKIVA_PushFrame`
4. Регулировку яркости через `led_ir_level` (0-8) → duty_cycle вспышки
5. **Не портировать** старый софтверный ШИМ (5 Гц, мёртвый код) —
   он не реализует strobe, это просто незаконченный эксперимент

**Главное:** old система использует **strobe-подсветку** (вспышки синхронно
с кадрами NN, только при motion) — это **умнее**, чем постоянный PWM.
Для порта сохранить этот подход, реализовать через аппаратный API
(VI_LIGHT_CTL_PARAM_S или callback перед ROCKIVA_PushFrame).

### 11.10 Архитектурное решение: зона ответственности подсветки

**Проблема old архитектуры:**
Управление IR LED вынесено в **сервер PERCo-S30 (СКУД-модуль)** —
внешний процесс, не имеющий отношения к видеопайплайну. Это
**архитектурное уродство** по нескольким причинам:

1. **Нарушение зоны ответственности:** подсветка камеры — это
   аппаратный аспект видеозахвата, логически относится к видеопайплайну
   (percomedia/ISP), а не к СКУД (контроль доступа, пользователи, замки)

2. **Разрыв пайплайна:** videopipeline (percomedia) захватывает кадры,
   но не контролирует их засветку. Сервер СКУД включает LED, но не
   знает когда именно происходит захват кадра → нет точной
   синхронизации strobe с shutter (только приблизительная)

3. **Лишний round-trip:** motion_detected (percomedia) → callback (GUI)
   → WebSocket → сервер СКУД → GPIO/PWM. Цепочка из 4 звеньев вместо 1.
   Каждое звено добавляет задержку и точку отказа.

4. **Скрытая зависимость:** percomedia зависит от сервера СКУД для
   работы камеры в темноте, но это нигде не декларировано в коде
   percomedia. Мёртвый код ШИМ (11.1) — попытка автора вынести
   подсветку в percomedia, но он её не закончил.

5. **Невозможность автономии:** percomedia не может работать без
   сервера СКУД (нет подсветки → нет распознавания в темноте).
   Это связывает два независимых продукта.

**Почему так получилось (гипотеза):**
- Сервер PERCo-S30 — старый продукт, существовал до percomedia
- IR LED исторически управлялся из СКУД (там же замки, реле, GPIO)
- Когда добавили face recognition (percomedia), подсветку не
  перенесли в новый модуль — оставили в СКУД «как работало»
- Мёртвый код ШИМ в userclass.h — незаконченная попытка автора
  percomedia перенести подсветку в свой модуль

**Решение для порта на RV1126B:**

**Подсветка переносится в percomedia (видеопайплайн)** — туда, где
ей место архитектурно. Сервер СКУД больше не управляет IR LED.

| Аспект | Old (PERCo-S30) | Порт (percomedia) |
|--------|-----------------|-------------------|
| Кто управляет IR LED | Сервер СКУД (внешний) | percomedia (видеопайплайн) |
| Синхронизация strobe | Нет (приблизительная) | Точная (в callback перед PushFrame) |
| Задержка motion→LED | 4 звена (round-trip) | 1 звено (прямо в пайплайне) |
| Зависимость | percomedia → СКУД → GPIO | percomedia → GPIO/PWM напрямую |
| Автономия | Не работает без СКУД | Работает independently |
| `led_ir_level` (0-8) | Через WebSocket на сервер | Прямо в percomedia config |

**Конкретное размещение в порте:**
- IR LED управление — в `isp_vi.cpp` (или отдельный `isp_light.cpp`)
- MotionTrigger → `motion_changed` → включить/выключить strobe
  (прямо в percomedia, без round-trip через GUI/сервер)
- `led_ir_level` (0-8) — в `percomedia_config.json`, применяется
  напрямую к duty_cycle PWM
- GUI ползунок «Яркость IR подсветки» — опционально, через callback
  в percomedia (не через сервер СКУД)

**Что НЕ портировать из old (архитектурное уродство):**
- ❌ Отправку `on_motion`/`off_motion` на сервер СКУД для управления LED
- ❌ Отправку `led_ir_level` на сервер СКУД
- ❌ Зависимость percomedia от сервера СКУД для подсветки
- ❌ Мёртвый код ШИМ в userclass.h (незаконченный эксперимент)

**Что портировать (с переносом зоны ответственности):**
- ✅ Strobe-режим (вспышки синхронно с кадрами NN) — умное решение
- ✅ Активация по motion (MotionTrigger уже в percomedia)
- ✅ Регулировку яркости через `led_ir_level` (0-8) → duty_cycle
- ✅ Реализация через VI_LIGHT_CTL_PARAM_S или callback перед PushFrame

**Итог:** old архитектура вынесла подсветку в СКУД-модуль — это
архитектурное уродство, создающее лишние зависимости и разрыв
пайплайна. В порте подсветка возвращается в percomedia (видеопайплайн),
где ей место. Strobe-режим сохраняется (он умный), но реализуется
прямо в пайплайне с точной синхронизацией, без round-trip через СКУД.

---

## 12. Приостановка дальнейшего плана

**Статус:** дальнейшая детализация плана портирования **приостановлена**
до получения полного доступа к исходному коду сервера PERCo-S30.

**Причина:** текущий анализ основан на коде `s30guiproj` (GUI + percomedia
библиотека). Сервер PERCo-S30 (СКУД-модуль) — закрытый бинарник,
его исходный код недоступен. Ряд критичных аспектов невозможно
доработать без доступа к серверу:

**Что требует доступа к исходному коду S30:**

1. **IR LED strobe-реализация на old плате** (см. 11.8):
   - Подтвердить гипотезу, что 5 Гц = strobe синхронно с кадрами NN
   - Понять точный механизм: PWM duty, period, синхронизация с shutter
   - Это нужно для корректного переноса strobe в percomedia (11.10)

2. **Протокол WebSocket GUI ↔ сервер СКУД:**
   - Полный список команд (`set:settings`, `event:face`, `get:settings`)
   - Какие настройки сервер применяет сам, какие форвардит обратно
   - Формат ответов (для замены сервера или прямого режима)

3. **`led_ir_level` / `led_white_level` применение:**
   - Как сервер переводит 0-8 в физическую яркость LED
   - Через PWM duty? GPIO? cpsl? (см. 11.6)
   - Нужно для корректной замены в percomedia

4. **IR-cut фильтр управление:**
   - Сервер управляет ircut? Через GPIO или rk_aiq CPSL?
   - На RV1126B ircut GPIO нет в ini (см. 11.6, пункт 4)

5. **Night-to-day режим:**
   - Кто переключает день/ночь (ircut + LED)?
   - Сервер или rkaiq_server автоматически?

**Что можно продолжать без доступа к S30:**

- ✅ Базовый видеопайплайн (VI → ROCKIVA → VO/VENC) — не зависит от СКУД
- ✅ ROCKIVA face recognition — самодостаточен
- ✅ Users/UserList (база пользователей) — в percomedia
- ✅ GUI (Qt) — в s30gui
- ✅ Базовый IR LED on/off (GPIO/PWM) — без strobe-тонкостей
- ✅ Дисплей backlight (`/sys/class/backlight/`) — уже в s30gui

**Что приостановлено:**

- ⏸ Точная strobe-реализация IR LED (до подтверждения механизма из S3 0)
- ⏸ Перенос `led_ir_level`/`led_white_level` (до понимания формата S3 0)
- ⏸ IR-cut фильтр управление (до понимания, кто им управляет)
- ⏸ Night-to-day логика (до понимания распределения ответственности)
- ⏸ Полный протокол GUI↔СКУД (до доступа к S3 0)

**Действие при получении доступа к S3 0:**
1. Изучить strobe-реализацию IR LED → подтвердить/опровергнуть гипотезу 11.8
2. Изучить применение `led_ir_level` → корректно перенести в percomedia
3. Изучить ircut управление → перенести в percomedia (или оставить rkaiq)
4. Изучить night-to-day → решить, кто управляет (percomedia vs rkaiq_server)
5. Доработать раздел 11 с точными механизмами вместо гипотез
6. Продолжить детализацию плана портирования

---

## 13. Итог: что уходит благодаря IVA

| Уходит | Причина |
|--------|---------|
| **6 вызовов rockx_* в check_face** | ROCKIVA делает quality+pose внутри |
| **3 вызова rockx_* в nn_face** | ROCKIVA делает align+recognize внутри |
| **rockx_object_track** | Трекер встроен в ROCKIVA |
| **rockx_face_liveness** | Встроен (faceLivenessEnable) |
| **2 канала RGA (RGA1/RGA2)** | ROCKIVA сама вращает (transformMode) |
| **ImageBuf для NN** | PushFrame напрямую из VI |
| **Таймер onTRecognizeTask** | Callback вместо опроса |
| **Isp_LL (rk_aiq)** | rkaiq_server как сервис |
| **~1200 строк кода** | Упрощение в 2 раза |

**Главный выигрыш:** ROCKIVA покрывает весь face pipeline (detect→quality→
landmark→pose→align→recognize→liveness→track) в одном `FACE_Init` + `PushFrame`.
Ручная логика из nnclass.cpp (575 строк) заменяется на ~200 строк
(init + callback). RGA для нейросети не нужен — ROCKIVA сама вращает кадр.
