# RockIva V1.23.1 для RV1126B — полная поставка

## Источник
`C:\Users\pvv\Downloads\Filez\WebTool\ROCKIVA_RV1126B\ROCKIVA_RV1126B_LINUX_V1.23.1_20260707\`

Версия на плате: `ROCK-X SDK 2.0.0-A1 | build: db69a43 2025-05-21` (librockiva.so 666KB)
Новая версия: V1.23.1 от 2026-07-07 (librockiva.so 3.0MB) — гораздо больше функционала.

Платформа: aarch64 (64-bit), Linux 6.1.118, RV1126B-buildroot.

### Liveness: 2D vs 3D (dualcam) — анализ librockiva.so

**Модель `face_liveness_2d.data` содержит ДВА пути** (из strings librockiva.so):

1. **RGB путь**:
   - `rockx_face_liveness_detect` (rockx_api.cpp)
   - вывод: `liveness result fake_score=%f real_score=%f`

2. **IR путь** (IR = инфракрасная камера, не аббревиатура):
   - `run_liveness_detect_ir` — анализ кадра с IR-камеры
   - вывод: `liveness ir %d fake=%f real=%f`
   - подаётся через `ROCKIVA_PushFrame2(rgb, ir, depth)` (dualcam)
   - если IR-лицо не найдено: `face %d link ir face null`

3. **Финальный score** (face_liveness.cpp):
   - `face %d liveness: score=%.2f`
   - `faceLivenessEnable` включает модуль

### НЕ ДОКАЗАНО (требует теста на плате)

- Работает ли liveness **только по RGB** (без IR-камеры)?
  - Вариант A: RGB-путь самостоятельный → работает, IR-путь опционально усиливает
  - Вариант B: liveness требует IR-кадр → без IR-камеры возвращает UNKNOWN
- Что именно анализирует RGB-путь (цвет? текстуру? частоты?) — модель закрытая

В demo:
- `face_recog_dualcam_demo.c`: `cameraType=DUAL` + `faceLivenessEnable=1` + `PushFrame2(rgb,ir)`
- `iva_app_face.c` (single-cam): `faceLivenessEnable` НЕ включён
- `iva_face.json` (single-cam): `faceLivenessEnable` отсутствует

**Тест на плате:**
1. Загрузить `face_liveness_2d.data` + новую `librockiva.so`
2. Демка: `faceLivenessEnable=1`, single-cam, `ROCKIVA_PushFrame(rgb)`
3. Смотреть логи:
   - `face %d liveness: score=0.85` → RGB-путь работает
   - `face %d link ir face null` + `liveness=UNKNOWN` → требуется IR-камера

**У нас на плате**: только RGB-сенсоры (sc200ai, sc450ai). IR-камеры нет.
Если liveness требует IR — anti-spoofing не сработает, нужно либо
вторая IR-камера, либо отказаться от liveness.

---

## 1. Библиотеки (`librockiva/rockiva-rv1126b-Linux/`)

| Файл | lib (armhf 32-bit) | lib64 (aarch64 64-bit) |
|------|--------------------|------------------------|
| `librockiva.so` | 3.0MB | 3.0MB ← **наша** |
| `librknnrt.so` | 4.9MB | 7.7MB |

На плате сейчас в `/usr/lib/` и `/usr/lib64/` — одинаковые старые 666KB файлы (V2.0.0-A1).
Нужно заменить на новую lib64 (3.0MB).

---

## 2. Заголовки (10 файлов, vs 6 в SDK)

| Файл | Назначение |
|------|------------|
| `rockiva_common.h` | Базовые типы, `ROCKIVA_Init/PushFrame/Release`, модели детекции |
| `rockiva_ba_api.h` | Behavior Analysis: regional invasion, tripwire, area in/out |
| `rockiva_det_api.h` | Простая детекция объектов (без правил) |
| `rockiva_face_api.h` | Распознавание лиц: атрибуты, качество, liveness, features |
| `rockiva_image.h` | Утилиты: Crop, Convert, Clone, SyncMem |
| `rockiva_one_api.h` | "One-shot" API (один вызов для одной картинки) |
| **`rockiva_hand_api.h`** | Жесты (новый) |
| **`rockiva_highap_api.h`** | Высокое падение (новый) |
| **`rockiva_object_api.h`** | Объекты/ReID (новый) |
| **`rockiva_plate_api.h`** | Номера автомобилей (новый) |
| **`rockiva_pose_api.h`** | Позы/падения (новый) |
| **`rockiva_reid_api.h`** | Re-identification человека (новый) |
| **`rockiva_ts_api.h`** | Статистика客流 (новый) |

В SDK `external/iva/librockiva/rockiva-rv1126b-Linux/include/` — только 6 базовых хедеров
(без hand/highap/object/plate/pose/reid/ts).

---

## 3. Модели (`models/rockiva_data_rv1126b/`, 18 файлов)

### Детекция объектов
| Файл | Размер | Тип | Классы |
|------|--------|------|--------|
| `object_detection_v3_cls8_640x384.data` | 5.2MB | CLS8 640x384 | person, face, vehicle, plate, non-vehicle, head |
| `object_detection_v3_cls8_896x512.data` | 5.2MB | CLS8 896x512 | (то же, выше разрешение) |
| `object_detection_v3_pfp_640x384.data` | 3.5MB | PFP 640x384 | person, face, pet |
| `object_detection_v3_pfp_896x512.data` | 3.5MB | PFP 896x512 | (то же, выше разрешение) |
| `object_detection_v4_hand.data` | 5.2MB | v4 hand | руки/жесты |
| `object_detection_pfp_for_recog.data` | 803KB | PFP for recog | детекция для face import |

### Face (распознавание лиц)
| Файл | Размер | Назначение |
|------|--------|------------|
| `face_detection_v2.data` | 2.3MB | Детекция лиц |
| `face_landmark5.data` | 501KB | 5 ключевых точек |
| `face_quality_v2.data` | 341KB | Оценка качества |
| `face_quality_mask.data` | 202KB | Качество + маска |
| `face_liveness_2d.data` | 2.4MB | Liveness 2D (анализ одной RGB картинки: текстура, спектр, Moiré) |
| **`face_recognition.data`** | **9.5MB** | **Feature extraction (главная!)** |
| `face_attribute_v3.data` | — | Атрибуты (пол/возраст/эмоция/...) |

### Carplate (номера)
| Файл | Размер | Назначение |
|------|--------|------------|
| `carplate_detection_v3.data` | 2.4MB | Детекция номеров |
| `carplate_attribute.data` | 1.2MB | Атрибуты (цвет/тип) |
| `carplate_recognition_v3.data` | 6.0MB | Распознавание символов |

### Vehicle
| Файл | Размер | Назначение |
|------|--------|------------|
| `vehicle_attribute.data` | 902KB | Атрибуты автомобиля |

### Hand
| Файл | Размер | Назначение |
|------|--------|------------|
| `hand_gesture.data` | 1.1MB | Распознавание жестов |

### На плате сейчас
Только 3 модели в `/usr/lib/`:
- `iva_object_detection_pfp.data` (32B — заглушка?)
- `iva_object_detection_v3_cls8.data` (5.2MB)
- `iva_object_detection_v3_pfp.data` (3.5MB)

**Face-моделей на плате НЕТ.** Нужно загрузить.

---

## 4. Демо (полный исходник в `demo/rockiva_demo/`)

| Файл | Назначение |
|------|------------|
| `rockiva_demo.c` | Детекция на картинках |
| `rockiva_video_demo.cpp` | Видео/RTSP |
| `rockiva_camera_demo.cpp` | Камера + DRM display |
| `rockiva_ipc_demo.c` | IPC стиль |
| `rockiva_multi_channel_demo.c` | Многоканальный |
| `rockiva_one_demo.c` | One-shot API |
| **`face_recog_import_demo.c`** | **Регистрация лиц в базу** |
| **`face_recog_picture_demo.c`** | **Распознавание с поиском** |
| **`face_recog_search_demo.c`** | **Поиск feature в базе** |
| **`face_recog_dualcam_demo.c`** | **Двойная камера (RGB+IR для liveness)** |
| `person_retrieval_import_search_demo.c` | ReID человека |

### Модули (`demo/rockiva_demo/modules/`)
- `iva_app_det.c` — детекция
- `iva_app_ba.c` — behavior analysis
- `iva_app_face.c` — face recognition (главный для контроля доступа)
- `iva_app_plate.c` — номера
- `iva_app_ts.c` — статистика
- `iva_app_object.c` — ReID
- `iva_app_pose.c` — позы/падения
- `iva_app_hand.c` — жесты
- `iva_app_highap.c` — высокое падение

---

## 5. Утилиты (`demo/rockiva_demo/utils/`)

| Файл | Назначение |
|------|------------|
| `face_db.c/h` | SQLite база лиц (id, info, feature blob) |
| `object_db.c/h` | База объектов (ReID) |
| `image_buffer.c/h` | Менеджер буферов (CPU/DMA) |
| `image_drawing.c/h` | Рисование (rect, text) |
| `image_utils.c/h` | Конвертация, JPEG, SaveImage, ReadImage |
| `common_utils.c/h` | ReadDataFile, GetTimeStampMS, CreateIvaAppDir |
| `truetype_utils.c/h` | Шрифты TTF |
| `draw_call.c/h` | Векторные команды рисования |
| `libyuv_utils.cpp/h` | YUV конвертация |

### 3rdparty (`demo/rockiva_demo/3rdparty/`)
- `jpeg_turbo/` — libturbojpeg (JPEG кодек)
- `libdrm/` — DRM заголовки
- `libexif/` — EXIF
- `librga/` — RGA
- `libyuv/` — YUV утилиты
- `mpp/` — MPP
- `sqlite3/` — SQLite (для face_db)
- `stb_image/` — STB image
- `zlmediakit/` — streaming

---

## 6. Лицензирование — выводы (по данным ChatGPT + документация)

### Главное
- **License передаётся в глобальный `ROCKIVA_Init`** (поле `RockIvaInitParam.license`),
  ДО инициализации любого модуля (face/det/ba). То есть лицензируется **весь SDK целиком**,
  не отдельные модули.
- **Официального прайса нет.** Условия выдаются по запросу через:
  1. Поставщика платы/SoM (имеет FAE-канал Rockchip)
  2. Rockchip Business Consulting / Technical Consulting
  3. `service@rock-chips.com`
  4. Тел: `+86-591-83991906`
- **Trial публично не задокументирован.** Но RKAUTH технически поддерживает
  тестовые квоты (видели `1/5` для другого модуля) — нужно просить у FAE.

### Что означает `face 152/200`
- `152/200` = **использовано уникальных активаций / предоставлено активаций**
- НЕ количество распознаваний, НЕ запусков программы
- Повторная активация того же устройства не списывает count
- У владельца аккаунта — пакет на 200 устройств

### Что работает без license
- Документация **не гарантирует** даже базовый detect без license
- То что `rkipc` на плате работает с BA-детекцией без видимого `key.lic`:
  - Возможно license встроен в `rkipc` / vendor storage / OEM-сборка
  - Возможно BA использует облегчённую ветку
  - Возможно базовый detect разрешён контрактом, а face_recognition — нет
- **Это не доказывает**, что `ROCKIVA_FACE_Init` с feature extraction тоже разрешён

### План проверки на плате
1. Найти откуда `rkipc` берёт license:
   ```sh
   PID=$(pidof rkipc)
   cat /proc/$PID/maps | grep -E 'rockiva|rknn'
   strings /usr/bin/rkipc | grep -iE 'license|rkauth|key\.lic'
   grep -RIlE 'Rockchip License|Activate Code' /userdata /oem /vendor /etc 2>/dev/null
   strace -f -e trace=openat,read /usr/bin/rkipc 2>&1 | grep -iE 'lic|key|auth|vendor'
   ```
2. Тестовый запуск:
   ```c
   RockIvaRetCode ret = ROCKIVA_Init(...);  // license = NULL
   printf("ROCKIVA_Init = %d\n", ret);
   ret = ROCKIVA_FACE_Init(...);
   printf("ROCKIVA_FACE_Init = %d\n", ret);
   ```
   - Если `ROCKIVA_Init = 0` с пустой license → сборка OEM/pre-authorized
   - Если `ROCKIVA_FACE_Init = -4` (LICENSE_ERROR) → детекция разрешена, face закрыт
   - Если callback status `10` (ROCKIVA_LICENSE_ERROR) → то же самое

### РЕЗУЛЬТАТ ТЕСТА на плате (2026-07-29)

**Проверка rkipc:**
- `rkipc` использует только `ROCKIVA_Init` + `ROCKIVA_BA_Init` (только BA-детекция)
- В строках rkipc НЕТ `license`, `key.lic`, `rkauth`
- В открытых файлах rkipc: `/dev/kmpp_objs`, DMA heap, `/tmp/aiq0.lock` — **никакого license файла**
- `key.lic` отсутствует на плате: `/data/`, `/userdata/`, `/etc/`, `/oem/` — нет нигде
- OTP (eFuse) есть: `/sys/bus/nvmem/devices/rockchip-otp0/nvmem` = `52 56 11 26 42 bd ff ff...`
- **Вывод: rkipc работает вообще без license**

**Тест со старой lib (на плате, 666KB, V2.0.0-A1):**
```
ROCKIVA_Init(license=NULL)           = SUCCESS(0)  ← работает без license!
ROCKIVA_FACE_Init(faceRecogEnable=1) = SUCCESS(0)  ← формально OK
ROCKIVA_FACE_FeatureCompare(...)     = UNSUPPORTED(-5)  ← НЕ РЕАБОТАЕТ
ROCKIVA_FACE_SearchFeature(...)      = UNSUPPORTED(-5)  ← НЕ РАБОТАЕТ
```
- Модели из V1.23.1 несовместимы со старой lib: `parseRKNN: invalid RKNN_MAGIC!`
- Старая lib экспортирует face-функции, но **не реализует их** (UNSUPPORTED)
- SDK external/iva для RV1126B содержит только `iva_object_detection_*.data`
  (нет `face_recognition.data`, `face_detection_v2.data`, и т.д.)

**Тест с новой lib (V1.23.1, 3.3MB, 2026-07-07):**
```
ROCKIVA_Init(license=NULL) = FAIL(-1)
  auth fail -99
  Error: no authorization!
  RockIVA SDK (version: 1.9.5 | auth: iva)
```
- Новая lib **требует license для всего** — даже ROCKIVA_Init падает
- Без license: `rockx_check_auth` → `auth fail -99`

### ИТОГ

| Компонент | Старая lib (666KB) | Новая lib V1.23.1 (3.3MB) |
|-----------|-------------------|---------------------------|
| BA-детекция (person/face detect) | ✅ без license | ❌ требует license (detModel=PFP) |
| ROCKIVA_Init (detModel=NONE) | ✅ без license | ✅ без license (с пустым key.lic) |
| Face detection (внутри FACE_Init) | ❌ UNSUPPORTED | ✅ без license (с пустым key.lic) |
| Face recognition (feature extraction) | ❌ UNSUPPORTED | ❌ **LICENSE_ERROR(-4)** |
| FeatureCompare (1:1) | ❌ UNSUPPORTED | ❌ требует license |
| SearchFeature (1:N) | ❌ UNSUPPORTED | ❌ требует license |
| License | не требуется | **обязательна для face_recognize** |

**Тест с пустым key.lic (0 байт) — НЕ триал:**
- `ROCKIVA_Init(detModel=NONE, license=пустой)` = SUCCESS
- `ROCKIVA_FACE_Init(faceRecognizeEnable=1)`:
  - ObjectDetector (face detection) — **OK** (Init end)
  - ObjectTracker — **OK**
  - `GetFaceRecognizerInstance` → `auth fail -99` → **LICENSE_ERROR(-4)**
- Auth check срабатывает **именно на feature extraction** (GetFaceRecognizerInstance)
- Пустой key.lic = просто пустой файл, не триал

**Выводы:**
1. **Бесплатно (без license) работает:**
   - BA-детекция (через старую lib 666KB)
   - Face detection (через новую lib V1.23.1 с пустым key.lic, detModel=NONE)
2. **Face recognition (feature extraction) НЕ работает без license:**
   - Старая lib: функции возвращают UNSUPPORTED (не реализованы)
   - Новая lib: `GetFaceRecognizerInstance` → auth fail -99 → LICENSE_ERROR(-4)
3. **Trial не найден** — пустой key.lic не активирует trial
4. **Для face recognition нужна:**
   - Новая lib V1.23.1 (3.3MB)
   - License от Rockchip (через sales/FAE) — **обязательна**
   - Face-модели (face_recognition.data и др.)

### ТРЮК WARMUP — бесплатная PFP детекция (для разработки, НЕ для production)

**Обнаружен баг в librockiva V1.23.1:** после неудачного auth check в VIDEO mode,
lib не сбрасывает внутренний флаг "auth checked", и последующий PICTURE mode
пропускает проверку.

**Использование (только для dev/test, пока ждём license):**
```c
/* 1. Warmup: VIDEO PFP → FAIL (auth check, но флаг остаётся) */
RockIvaHandle h1 = NULL;
ROCKIVA_Init(&h1, ROCKIVA_MODE_VIDEO, &pfp_params, NULL);  // = -1 (FAIL)
// h1 не валиден, не Release

/* 2. Рабочий вызов: PICTURE PFP → SUCCESS (auth пропущен) */
RockIvaHandle h2 = NULL;
ROCKIVA_Init(&h2, ROCKIVA_MODE_PICTURE, &pfp_params, NULL);  // = 0 (SUCCESS)
// h2 валиден, можно использовать
```

**Что работает с warmup:**
- `ROCKIVA_Init(PICTURE, PFP)` = SUCCESS
- `ROCKIVA_DETECT_Init` = SUCCESS

**Что НЕ работает даже с warmup:**
- `ROCKIVA_PushFrame` → `rockx_object_detect_ipc error -3`
  (auth check срабатывает на runtime inference, не только на Init)
- `ROCKIVA_FACE_Init(faceRecognizeEnable=1)` → LICENSE_ERROR(-4)
- Face recognition (feature extraction) — требует настоящую license

**Вывод: warmup трюк НЕ работает для реальной детекции через новую lib.**
Auth check в новой lib стоит на двух уровнях:
1. Init (обходится warmup) — для VIDEO mode
2. Runtime inference `rockx_object_detect_ipc` — НЕ обходится

### РАБОЧИЙ БЕСПЛАТНЫЙ ПУТЬ — старая lib 666KB + ROCKIVA_BA_Init

**Старая lib (666KB, /usr/lib/librockiva.so) — без auth check, работает бесплатно:**

```c
/* 1. ROCKIVA_Init(VIDEO, PFP) — без auth, без warmup */
RockIvaHandle handle = NULL;
RockIvaInitParam params;
memset(&params, 0, sizeof(params));
snprintf(params.modelPath, 128, "/usr/lib/");  /* модели: iva_object_detection_pfp.data */
params.detModel = ROCKIVA_DET_MODEL_PFP;
params.imageInfo.width = 1920;
params.imageInfo.height = 1080;
params.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
ROCKIVA_Init(&handle, ROCKIVA_MODE_VIDEO, &params, NULL);  // = 0 (SUCCESS)

/* 2. ROCKIVA_BA_Init — BA (Behaviour Analysis) детекция */
RockIvaBaTaskParams baParams;
memset(&baParams, 0, sizeof(baParams));
baParams.baRules.areaInBreakRule[0].ruleEnable = 1;
baParams.baRules.areaInBreakRule[0].objType =
    ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON) |
    ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_FACE) |
    ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PET);
baParams.aiConfig.detectResultMode = 1;  /*上报所有检测目标 */
ROCKIVA_BA_Init(handle, &baParams, callback);  // = 0 (SUCCESS)

/* 3. PushFrame — работает с CPU malloc (без DMA!) */
RockIvaImage image = {.dataAddr = malloc(...), .info.format = NV12, ...};
ROCKIVA_PushFrame(handle, &image, NULL);  // = 0, callback с результатами
```

**Результат теста:**
- `ROCKIVA_Init` = 0 (SUCCESS, без auth)
- `ROCKIVA_BA_Init` = 0 (SUCCESS)
- `ROCKIVA_PushFrame` = 0 (SUCCESS)
- BA callback: status=0 (SUCCESS), objNum=0 (серый кадр — нет объектов)
- **Pipeline работает!** Callbacks приходят с SUCCESS

**Модели на плате (/usr/lib/):**
- `iva_object_detection_pfp.data` — Person, Face, Pet
- `iva_object_detection_v3_cls8.data` — CLS8
- `iva_object_detection_v3_pfp.data` — PFP v3

**Это то что использует rkipc** — BA детекция person/face/pet бесплатно.

### Архитектура оригинала s30guiproj (старая плата)

**Путь данных (pipeline):**
```
Cam0 (IR, GC2093, 1920x1080 landscape)
Cam1 (RGB, GC2093, 1920x1080 landscape)
        │
        ▼
   RK_MEDIA VI (старый API rkmedia, НЕ RKMPI!)
        │
        ├──→ RGA3 (rotate 270°, NV12→ARGB8888, crop) ──→ RGA0 (RGB888) ──→ VO (DRM overlay) ──→ DSI дисплей 800x1280 portrait
        │
        ├──→ RGA1 (rotate 90°, NV12→BGR888, 720x1280) ──→ callback ──→ ImageBuf::RGB ──→ nnclass (rockx face)
        │
        └──→ RGA2 (rotate 90°, NV12→BGR888, 720x1280) ──→ callback ──→ ImageBuf::IR  ──→ nnclass (rockx face)
```

**Ключевые компоненты:**
- `rkmedia/` — старый RKMEDIA API (`RK_MPI_VO_CreateChn`, `RK_MPI_RGA_CreateChn`, `RK_MPI_SYS_Bind`)
- `rockx/` — старая RockX библиотека (face detect/recognize, БЕЗ license)
- `drmconf.cpp` — прямой DRM вывод (overlay plane на /dev/dri/card0)
- `isp_rga.cpp` — 4 канала RGA с ротацией:
  - RGA0: ARGB8888→RGB888, rotation=0 (для VO)
  - RGA1: NV12→BGR888, rotation=rga_nn_angle (90°) — RGB для нейронки
  - RGA2: NV12→BGR888, rotation=rga_nn_angle (90°) — IR для нейронки
  - RGA3: NV12→ARGB8888, rotation=rga_vo_angle (270°) — для дисплея
- `nnclass.cpp` — face recognition через rockx (rockx_face_recognize, БЕЗ license)
- `ispclass.cpp` — оверлей с боксами через im2d (imfill, imcomposite)

**Ротация:**
- Камеры landscape 1920x1080, дисплей portrait 800x1280
- RGA3 крутит 270° для дисплея (VO)
- RGA1/RGA2 крутят 90° для нейронки (NN)
- Углы настраиваются в percomedia.json: rga_vo_angle=270, rga_nn_angle=90
- **Ротация через RGA, НЕ через VPSS/VO** (VPSS rotation на RV1126B не работает)

### Ротация на RV1126B — РЕШЕНО через librga im2d

**Проблема:** `RK_MPI_VPSS_SetGrpRotation` на RV1126B возвращает OK, но НЕ крутит.
Причина: `online not support rotate` (VPSS online mode не поддерживает ротацию).

**Решение:** Использовать `improcess()` из librga im2d API напрямую.

**Тест rga_test.c подтвердил:**
```
RGA version: RGA_2_PRO
Max input:  8192x8192  (1920x1080 влезает с запасом!)
Max output: 8192x8192

rotate 90°:  1920x1080 → 1080x1920  ✅ SUCCESS
rotate 270°: 1920x1080 → 1080x1920  ✅ SUCCESS
scale:       1920x1080 → 800x1280   ✅ SUCCESS
scale+rot90: 1920x1080 → 720x1280   ✅ SUCCESS
crop+scale+rot90: 800x1080 → 800x1280 ✅ SUCCESS
```

**Код для порта (замена старому RK_MPI_RGA_CreateChn):**
```c
#include "im2d.h"
#include "RgaApi.h"

rga_buffer_t src = wrapbuffer_fd(src_fd, 1920, 1080, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_fd(dst_fd, 800, 1280, RK_FORMAT_YCbCr_420_SP);
im_rect srect = {0, 0, 1920, 1080};
im_rect drect = {0, 0, 800, 1280};
im_rect prect = {0};
int usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;  /* или ROT_270 */
IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, usage);
/* st == IM_STATUS_SUCCESS (1) = OK */
```

**Эквивалент старых 4 каналов RGA:**
| Старый канал | u16Rotaion | Новый эквивалент |
|--------------|-----------|------------------|
| RGA0 (VO) | 0 | improcess(..., IM_SYNC) |
| RGA1 (NN RGB) | 90 | improcess(..., ROT_90 \| IM_SYNC) |
| RGA2 (NN IR) | 90 | improcess(..., ROT_90 \| IM_SYNC) |
| RGA3 (display) | 270 | improcess(..., ROT_270 \| IM_SYNC) |

**Важно:** RGA im2d — НЕ MPI-модуль, нельзя `RK_MPI_SYS_Bind(RGA→...)`.
Нужно вручную: VPSS GetFrame → improcess → результат в свой буфер.

### VPSS размер кадра — НЕ проблема

**Тест vpss_size_test.c: 15/15 OK**

| Размер | VProcDev | Результат |
|--------|----------|-----------|
| 640x480 | RGA / VPSS | ✅ OK |
| 1280x720 | RGA / VPSS | ✅ OK |
| 1280x720 → 800x1280 | RGA / VPSS | ✅ OK |
| **1920x1080** | **RGA / VPSS** | **✅ OK** |
| 1920x1080 → 800x1280 | RGA / VPSS | ✅ OK |
| 1920x1080 → 720x1280 | RGA | ✅ OK |
| 1920x1080 → 1080x1920 (rotate size) | RGA | ✅ OK |
| 2688x1520 → 1920x1080 (max cam) | RGA / VPSS | ✅ OK |
| 4096x2160 → 1920x1080 | RGA | ✅ OK |

**VPSS u32MaxW/u32MaxH: [64, 16384]** — переваривает любой размер.
**VProcDev RGA и VPSS оба работают.**

### VPSS Rotation — РАБОТАЕТ в offline режиме!

**Тест vpss_rotate_matrix.c: 12/16 OK**

**Ключевое открытие:** `RK_MPI_VPSS_SetGrpRotation` работает в **offline режиме**
(ручной `RK_MPI_VPSS_SendFrame`), но НЕ в **online режиме** (VI→VPSS bind).

**Online режим:** `online not support rotate` — rotation тихо игнорируется.
**Offline режим:** rotation РАБОТАЕТ, но с ограничениями по размеру выхода.

| Тест | Результат |
|------|-----------|
| 512x512 rot90 → 512x512 | ✅ WORKS |
| 640x480 rot90 → 640x480 | ✅ WORKS |
| 1280x720 rot90 → 1280x720 | ✅ WORKS |
| 1920x1080 rot90 → 1920x1080 | ✅ WORKS |
| 640x480 rot90 → 480x640 (swap) | ✅ WORKS |
| 1280x720 rot90 → 720x1280 (swap) | ✅ WORKS |
| **1920x1080 rot90 → 800x1280** | **✅ WORKS (для дисплея!)** |
| **1920x1080 rot90 → 720x1280** | **✅ WORKS (для нейронки!)** |
| 1280x720 rot90 → 800x1280 | ✅ WORKS |
| 1920x1080 rot90 → 1080x1920 (full swap) | ❌ TIMEOUT (h=1920 too big) |
| 1920x1080 rot270 → any | ❌ TIMEOUT |
| 1920x1080 rot180 → any | ❌ TIMEOUT |

**Ограничения VPSS rotation (offline):**
- rot90 работает если выходная высота ≤ 1280
- rot270 и rot180 НЕ работают для 1920x1080
- Выходной размер может быть любым (scale + rotate в одном проходе)

**Для порта percomedia:**
- Дисплей: 1920x1080 → rot90 → 800x1280 ✅ (в оригинале rot270, но rot90 = то же)
- Нейронка: 1920x1080 → rot90 → 720x1280 ✅
- Если нужен rot270 — использовать RGA im2d (`improcess` с `ROT_270`)

**Архитектура порта (два варианта):**

**Вариант A: VPSS offline rotation**
```
VI → VPSS1 (bind, online, БЕЗ rot) → GetFrame 800x1280
  → VPSS2 (offline, rot90) → SendFrame → GetFrame → VO
```

**Вариант B: VPSS + RGA im2d**
```
VI → VPSS (bind, online, БЕЗ rot) → GetFrame 800x1280
  → RGA improcess(rot90/270) → свой буфер → VO
```

**Вариант B проще** — не нужен второй VPSS. RGA im2d доказано работает (rga_test.c).

### VPSS НЕ НУЖЕН — rkipc не использует его для IVA!

**Доказательство из rkipc rv1126b_ipc (наш чип):**

```c
// video.c строка 1410-1447: поток rkipc_get_vi_2_send
while (g_video_run_) {
    // БЕЗ FEC — кадр напрямую из VI:
    ret = RK_MPI_VI_GetChnFrame(pipe_id_, g_vi_for_npu_ivs_id, &stViFrame, 1000);
    // С FEC — через VPSS (только для беспроводной передачи!):
    // ret = RK_MPI_VPSS_GetChnFrame(0, 2, &stViFrame, 1000);

    // Дальше — просто PushFrame в IVA с dmabuf fd:
    rkipc_rockiva_write_nv12_frame_by_fd(
        stViFrame.stVFrame.u32Width,
        stViFrame.stVFrame.u32Height,
        loopCount,
        RK_MPI_MB_Handle2Fd(stViFrame.stVFrame.pMbBlk));

    RK_MPI_VI_ReleaseChnFrame(pipe_id_, g_vi_for_npu_ivs_id, &stViFrame);
}
```

**rkipc создаёт отдельные VI каналы** (video.c строка 783-808):
- `g_vi_for_npu_ivs_id = 4` — для нейронки (960x540, маленький!)
- `g_vi_for_vo_chn_id` — для дисплея (1920x1080)
- `g_vi_for_venc_1_id` — для энкодера (2560x1440)

**VI сам масштабирует** через ISP (каждый канал — свой размер из одного сенсора).
**VPSS используется ТОЛЬКО при enable_fec** (Forward Error Correction — беспроводная передача).

### Финальная архитектура порта percomedia — БЕЗ VPSS

**ROCKIVA сама вращает кадр для нейронки!** Через `transformMode` в `RockIvaImageInfo`.
RGA нужен только для дисплея (VO не умеет rotate).

```
Cam (GC2093 1920x1080 landscape)
    │
    ▼
   VI (3 канала из одного ISP, VI сам делает scale)
    │
    ├── VI ch_NN (960x540 NV12 landscape) → GetChnFrame
    │       → ROCKIVA_PushFrame(dataFd, transformMode=ROTATE_90)
    │       ↑ IVA САМА вращает перед детекцией!
    │       → callback: координаты объектов в повёрнутой системе
    │
    ├── VI ch_VO (1920x1080 NV12 landscape) → GetChnFrame
    │       → RGA improcess(rot90, NV12→BGRA8888, 800x1280)
    │       → RK_MPI_VO_SendFrame  [дисплей portrait]
    │
    └── VI ch_ENC (1920x1080 NV12) → bind → VENC [H264/JPEG запись]
```

**Доказательство из rkipc rv1126b_ipc (rockiva.c строка 220-233):**
```c
globalParams.imageInfo.width = 960;   // landscape, как есть из VI
globalParams.imageInfo.height = 540;
globalParams.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
// IVA сама покрутит:
if (rotation == 90)
    globalParams.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_ROTATE_90;
else if (rotation == 270)
    globalParams.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_ROTATE_270;

ROCKIVA_Init(&rkba_handle, ROCKIVA_MODE_VIDEO, &globalParams, NULL);
```

**RockIvaImageTransform (rockiva_common.h):**
```c
ROCKIVA_IMAGE_TRANSFORM_NONE      = 0x00,
ROCKIVA_IMAGE_TRANSFORM_FLIP_H    = 0x01,
ROCKIVA_IMAGE_TRANSFORM_FLIP_V    = 0x02,
ROCKIVA_IMAGE_TRANSFORM_ROTATE_90 = 0x04,  // CW 90°
ROCKIVA_IMAGE_TRANSFORM_ROTATE_180= 0x03,
ROCKIVA_IMAGE_TRANSFORM_ROTATE_270= 0x07,  // CW 270°
```

**Сравнение со старой платой (s30guiproj):**

| | Старая (rockx) | Новая (ROCKIVA) |
|---|---|---|
| Ротация для NN | RGA rot90 (4 канала RGA) | **ROCKIVA сама** через `transformMode` |
| Ротация для дисплея | RGA rot270 | RGA rot90 (только для VO) |
| Кадр в NN | BGR888 720x1280 повёрнутый | NV12 960x540 landscape (IVA крутит) |
| Формат в NN | BGR888 (rockx требовал) | NV12 (ROCKIVA принимает) |

**Преимущества:**
- НЕТ VPSS — проще, меньше кода
- НЕТ RGA для нейронки — ROCKIVA сама вращает
- VI сам масштабирует (через ISP) — бесплатно
- RGA im2d нужен ТОЛЬКО для дисплея (VO)
- IVA принимает NV12 dmabuf fd напрямую (zero-copy)
- Соответствует архитектуре rkipc (эталонный пример от Rockchip)

**Что нужно для порта:**
1. `RK_MPI_VI_SetChnAttr` + `RK_MPI_VI_EnableChn` — создать VI каналы
2. `RK_MPI_VI_GetChnFrame` — забрать кадр (dmabuf fd)
3. `ROCKIVA_Init` с `transformMode=ROTATE_90` — IVA будет сама вращать
4. `ROCKIVA_PushFrame(dataFd)` — в нейронку (NV12, landscape)
5. `improcess()` — RGA rotate+scale+format ТОЛЬКО для дисплея
6. `RK_MPI_VO_SendFrame` — на дисплей

### Маппинг моделей old rockx → new ROCKIVA

**Архитектура разная:**
- **Old rockx:** каждый модуль — отдельный `rockx_create(ROCKX_MODULE_*)` + отдельная модель
- **New ROCKIVA:** один `ROCKIVA_FACE_Init` с флагами — загружает все модели сразу

**Таблица маппинга модулей:**

| Old rockx (s30guiproj) | New ROCKIVA | Модель | Размер | License |
|---|---|---|---|---|
| `ROCKX_MODULE_FACE_DETECTION` | `ROCKIVA_FACE_Init` (faceCaptureEnable=1) | `face_detection_v2.data` | 2.3MB | **нужна** |
| `ROCKX_MODULE_FACE_LANDMARK_5` | `ROCKIVA_FACE_Init` (faceLandmarkEnable=1) | `face_landmark5.data` | 501KB | **нужна** |
| `ROCKX_MODULE_FACE_LANDMARK_106` | `ROCKIVA_FACE_Init` (faceLandmarkEnable=2) | (внутри librockiva) | — | **нужна** |
| `ROCKX_MODULE_FACE_RECOGNIZE` | `ROCKIVA_FACE_FeatureCompare` + `SearchFeature` | `face_recognition.data` | 9.5MB | **нужна** |
| `ROCKX_MODULE_FACE_LIVENESS` | `ROCKIVA_FACE_Init` (faceLivenessEnable=1) | `face_quality_v2.data` | 341KB | **нужна** |
| `ROCKX_MODULE_OBJECT_TRACK` | `ROCKIVA_FACE_Init` (внутри tracker) | (внутри librockiva) | — | **нужна** |
| — | `ROCKIVA_BA_Init` (PFP/CLS8) | `iva_object_detection_v3_pfp.data` | 3.5MB | **БЕСПЛАТНО** |

**Модели на плате:**
```
/usr/lib/iva_object_detection_v3_pfp.data   (3.5MB) — BA детекция person/face/pet (БЕСПЛАТНО)
/usr/lib/iva_object_detection_v3_cls8.data  (5.2MB) — BA детекция + vehicle/plate (БЕСПЛАТНО)
/tmp/iva_test/models/face_detection_v2.data (2.3MB) — face detection (нужна license)
/tmp/iva_test/models/face_landmark5.data    (501KB) — 5 точек лица (нужна license)
/tmp/iva_test/models/face_recognition.data  (9.5MB) — распознавание (нужна license)
/tmp/iva_test/models/face_quality_v2.data   (341KB) — качество + liveness (нужна license)
```

**Две версии librockiva.so:**
- `/usr/lib/librockiva.so` (666KB) — только BA (детекция), **БЕЗ license**
- `/tmp/iva_test/lib/librockiva.so` (3.3MB) — полный FACE (det+landmark+recogn+liveness), **нужна license**

**Бесплатно (без license):**
- `ROCKIVA_BA_Init` — детекция person/face/pet (bounding box только)
- Модели: `iva_object_detection_v3_pfp.data` / `iva_object_detection_v3_cls8.data`

**Требуют license:**
- `ROCKIVA_FACE_Init` — полный face pipeline (det + landmark + recognize + liveness)
- `ROCKIVA_FACE_FeatureCompare` / `SearchFeature` — распознавание 1:1 и 1:N
- Модели: `face_detection_v2.data`, `face_landmark5.data`, `face_recognition.data`, `face_quality_v2.data`

### 5 точек лица (face_landmark_5) — НЕ упрощённый поиск

**5 точек = базовая геометрия лица:**
- 2 глаза (left, right)
- 2 уголка рта (left, right)
- 1 нос (center)

**Используются для 3 вещей (в old rockx — вручную, в new ROCKIVA — автоматически):**

1. **Оценка качества** (`rockx_face_quality`) — по 5 точкам определяются:
   - Угол лица (pitch/yaw/roll) — не сильно ли повёрнут
   - Размытие (blur)
   - Яркость
   - Score уверенности

2. **Угол поворота** (`rockx_face_pose`) — проверка что человек смотрит прямо:
   - pitch ±50°, yaw ±20°, roll ±30° (из percomedia_config.h)

3. **Выравнивание лица** (`rockx_face_align`) — САМОЕ ВАЖНОЕ:
   - По 5 точкам лицо поворачивается/масштабируется/центрируется в стандартную позу 112x112
   - Только после align → `rockx_face_recognize` извлекает 512-float feature vector

**Полный pipeline распознавания в old rockx (nnclass.cpp):**
```
1. rockx_face_detect          → bounding box лица
2. rockx_face_quality (5pt)   → качество (угол/blur/яркость) ← фильтр
3. rockx_face_landmark (5pt)  → 5 точек
4. rockx_face_pose (5pt)      → pitch/yaw/roll ← фильтр (смотрит прямо?)
5. rockx_face_align (5pt)     → выравненное лицо 112x112
6. rockx_face_recognize       → 512-float feature vector
7. rockx_face_feature_compare → сравнение с базой (1:N)
```

**106 точек** — более детальные (контур лица, брови, глаза), для доп. анализа.
Для базового распознавания **5 точек достаточно**.

### Архитектура: old rockx (ручной) vs new ROCKIVA (автоматический)

**Old rockx (s30guiproj nnclass.cpp 22KB):**
- Каждый шаг ВРУЧНУЮ: detect → quality → landmark → pose → align → recognize
- 6 отдельных функций, ~500 строк кода
- Feature vector: 512 float
- Сравнение вручную: `rockx_face_feature_compare(feat1, feat2, &score)`

**New ROCKIVA:**
- **Один вызов** `ROCKIVA_FACE_Init` с флагами + `PushFrame` → callback с результатом
- IVA сама делает detect → quality → landmark → align → recognize внутри
- Feature vector: до 4096 float (`ROCKIVA_FACE_FEATURE_SIZE_MAX`)
- Сравнение: `ROCKIVA_FACE_FeatureCompare` (1:1) или `SearchFeature` (1:N по библиотеке)

```c
// New ROCKIVA — весь pipeline в одном init:
RockIvaFaceTaskParams params = {0};
params.faceTaskType.faceCaptureEnable   = 1;  // детекция + трекинг
params.faceTaskType.faceRecognizeEnable = 1;  // распознавание
params.faceTaskType.faceLandmarkEnable  = 1;  // 5 точек (quality + align)
params.faceTaskType.faceLivenessEnable  = 1;  // liveness
// faceLandmarkEnable = 2 → 5 + 106 точек

ROCKIVA_FACE_Init(handle, &params, &callback);
// Дальше просто PushFrame — callback получаем результат
```

**Качество и угол** настраиваются через `RockIvaFaceQualityConfig` (пороги blur/angle/size).
IVA сама отфильтрует — не нужно вручную проверять pitch/yaw/roll.

**Для порта:** большая часть nnclass.cpp (quality/pose/align) НЕ НУЖНА —
ROCKIVA делает это сама. Нужно только:
1. `ROCKIVA_FACE_Init` с флагами + `RockIvaFaceQualityConfig` (пороги)
2. `ROCKIVA_PushFrame` — отправить кадр
3. В callback получить результат (face box + feature + ID)
4. `ROCKIVA_FACE_FeatureCompare` / `SearchFeature` — сравнение с базой

### Рисование прямоугольников — RGA im2d (перенос 1:1)

**Old (s30guiproj ispclass.cpp):**
```c
// 1. Очистка оверлей-буфера
imfill(rect_mb, full_rect, 0x00000000);

// 2. Рисование 4 линий бокса для каждого user
imfill(rect_mb, rect_up,    color);  // верхняя линия
imfill(rect_mb, rect_buttom,color);  // нижняя линия
imfill(rect_mb, rect_left,  color);  // левая линия
imfill(rect_mb, rect_right, color);  // правая линия

// 3. Альфа-смешивание видео + оверлей
imcomposite(src_mb, rect_mb, src_mb, IM_ALPHA_BLEND_DST_OVER);
imsync();
```

**Эти функции — librga im2d, тот же API на новой плате!** Переносятся 1:1.
Доказано `rga_test.c` — `imfill` и `imcomposite` работают на RGA2_PRO.

**Цвета по состоянию пользователя (из draw_rects):**
- `CHECK` — жёлтый `#FFFF00`
- `CHECK_ERR` — оранжевый `#FF8000`
- `NN` — небесный `#87CEEB`
- `PASS_INIT` — синий `#001B51`
- `PASS_ERR` — красный `#AD2828`
- `PASS_OK` — зелёный `#1E7514`
- `PASS_WAIT` — голубой `#ADD8E6`
- Текущий пользователь — линия 9px, остальные — 5px

**Важно:** координаты прямоугольников приходят из нейронки.
- Old: rockx детектит на повёрнутом кадре (RGA rot90) → координаты в portrait
- New: ROCKIVA сама вращает (transformMode) → координаты в portrait (после rot90)
- Оба варианта дают координаты в portrait 800x1280 — рисование 1:1

**Связь с Qt:**
- `s30gui` — Qt приложение (QGraphicsView)
- Видео идёт через DRM overlay plane (в обход Qt)
- Qt рисует GUI на другом plane через Weston compositor
- `PercoMedia::updateGuiOverlay(QImage)` — смешивает результаты NN с Qt графикой
- `draw_rects()` — рисует боксы лиц через RGA imfill поверх видео

**Дисплей:**
- DSI 720x1280 (portrait), 56Hz
- DRM: connector=95 (DSI-1), crtc=72, plane=58/73
- VO_CHN_ATTR_S.pcDevNode = "/dev/dri/card0" (старый API)
- plane_type = VO_PLANE_OVERLAY (только оверлей умеет скалить)

### Запрашивать у Rockchip/поставщика
- Модуль: `face`
- Платформа: RV1126B, Linux aarch64
- Device ID: `1126:39eb501a677cf16f` (получен через `rkdevice_info`)
- Нужно: account на сервере Rockchip ИЛИ activate code для `rkauth_tool_bin`

**rkauth_tool_bin** (в SDK: `rkauth/Linux/aarch64/`):
```
./rkauth_tool_bin -u <user> -p <passwd> -d 1126:39eb501a677cf16f -m face -o key.lic
```
или через activate code:
```
./rkauth_tool_bin -t 1 -u <activate_code> -d 1126:39eb501a677cf16f -m face -o key.lic
```

**После получения key.lic** — положить в `/data/key.lic` и использовать примеры из SDK.

### Примеры в новом SDK (demo/rockiva_demo/)

| Файл | Описание |
|------|----------|
| `face_recog_picture_demo.c` | Распознавание по картинкам (NORMAL mode) |
| `face_recog_import_demo.c` | Импорт face library (IMPORT mode) |
| `face_recog_search_demo.c` | Поиск по базе |
| `face_recog_dualcam_demo.c` | Dual camera (RGB+IR) |
| `person_retrieval_import_search_demo.c` | Person Re-ID (заголовок есть, функции нет в lib) |
| `rockiva_ipc_demo.c` | Полный IPC demo |
| `rockiva_camera_demo.cpp` | С камеры |
| `rockiva_video_demo.cpp` | Из видео файла |

**Ключевые моменты из примеров:**
- License: `/data/key.lic` → `commonParams.license.memAddr`
- Models: `/data/rockiva_data/`
- DMA buffers: `ALLOC_IMAGE_BUFFER_TYPE = 1`
- Face library: `ROCKIVA_FACE_FeatureLibraryControl(face_lib, INSERT/SEARCH, ...)`
- SQLite для face database: `utils/face_db.h`

### Модули в librockiva V1.23.1

| Модуль | Функции в lib | License |
|--------|---------------|---------|
| BA | ✅ | не нужна (старая lib) |
| DETECT | ✅ | нужна (новая lib) |
| FACE | ✅ | нужна |
| HAND | ✅ | ? |
| HIGHAP | ✅ | ? |
| OBJECT | ✅ | ? |
| ONE | ✅ | ? |
| PLATE | ✅ | нужна |
| POSE | ✅ | ? |
| TS | ✅ | ? |
| REID | ❌ нет в lib (только заголовок) | — |
- Функции: feature extraction, 1:1 compare, 1:N local feature-library search
- Количество production-устройств
- Development/trial quota
- Срок действия license
- Привязка к chip ID / eFuse / device info
- Возможность переноса license при браке платы

---

## 7. Готовое решение на старой плате (10.1.29.246, RV1126/RV1109 armhf)

### Архитектура
```
s30gui (Qt GUI, "S30 face recognition gui", 2.9MB)
  └── libpercomedia.so.1 (565KB) ← наш K:\rockchip_test\percomedia\
       ├── ispclass (VI/VPSS/VO) — isp_vi.cpp, isp_vpss.cpp, isp_vo.cpp
       └── nnclass (FaceRecogn) → librockx.so (10MB) → librknn_runtime.so
```

### Ключевые файлы на плате
- `/usr/bin/s30gui` — главное приложение (Qt, 2.9MB, 2024-05-08)
- `/usr/lib/libpercomedia.so.1` — наша библиотека (565KB, 2024-06-18)
- `/usr/lib/librockx.so` — RockX SDK (10MB, 2023-01-09)
- `/usr/lib/librknn_runtime.so` — RKNN runtime (3MB)
- `/usr/lib/librockface.so` — RockFace обёртка (10MB, 2023-04-03) — НЕ используется!
- `/usr/lib/face_*.data` — модели:
  - `face_detection_v3_fast.data` (419KB)
  - `face_landmark5.data` (476KB)
  - `face_landmarks106.data` (618KB)
  - `face_liveness_2d.data` (2.1MB)
  - `face_recognition.data` (33MB)
  - `face_mask_recognition.data` (75MB)
  - `face_mask_classify.data` (189KB)
  - `face_mask_landmarks.data` (3.9MB)
  - `face_attribute.data` (290KB)
- `/oem/key.lic` — **ПУСТОЙ (0 байт)!**
- `/oem/face_data.db` — SQLite база лиц (пустая, 0 записей)
- `/oem/ir.yuv` — IR калибровочный кадр (1.3MB)
- `/oem/sysconfig.db` — конфигурация (315KB)
- `/home/percomedia.json` — конфиг ispclass + nnclass
- `/home/hnsw_db.json` — HNSW база лиц (481KB, старая)
- `/home/s30/` — рабочая директория s30gui (lua скрипты, www, ssl.pem)
- `/userdata/photoset/` — фото для распознавания (пусто)

### Конфиг percomedia.json
```json
{
    "ispclass": {
        "dbg_en": true,
        "display_ch": 1,
        "hdr_mode": true,
        "ir_auto": true,
        "manual_shutter_ir": 1,
        "manual_shutter_rgb": 8,
        "rga_nn_angle": 90,
        "rga_vo_angle": 270,
        "rgb_auto": true
    },
    "nnclass": {
        "dbg_en": 4,
        "facescore_min": 0.6,
        "framediff_max": 0.5,
        "ir_blur_max": 0.4,
        "livescore_min": 0.3,
        "max_pitch": 30.0,
        "max_rect_diff": 0.9,
        "max_roll": 30.0,
        "max_yaw": 30.0,
        "min_pitch": -30.0,
        "min_roll": -30.0,
        "min_size_px2": 5000,
        "min_yaw": -30.0,
        "rgb_blur_max": 0.3,
        "rgb_blur_min": 0.0,
        "rgb_imagequal": false
    }
}
```

### License — РАБОТАЕТ БЕЗ LICENSE!
- `/oem/key.lic` = **0 байт** (пустой файл)
- `ROCKX_LICENCE_KEY` env = **NOT SET**
- `rockx_set_licence` = **не экспортируется** librockx.so
- s30gui работает: face detect → landmark → align → quality → recognize
- В логах: `------Face ctor/dtor`, `{"event":"face","face":{"name":"on_motion"}}`
- librockx.so загружена, librockface.so НЕ загружена (libpercomedia использует rockx_* напрямую)

### API libpercomedia (класс FaceRecogn)
- `FaceRecogn::proc_init()` / `proc_deinit()` — инициализация
- `FaceRecogn::check_face(rockx_image_t&, rockx_image_t&, int)` — quality check
- `FaceRecogn::nn_face(rockx_image_t&, int, rockx_face_feature_t&)` → `rockx_face_recognize`
- `FaceRecogn::onTRecognizeTask()` — поток распознавания
- `FaceRecogn::onNNfaceRect(bool, QRect)` — callback
- `FaceRecogn::track(void*, rockx_object_array_t*)` — трекинг
- Использует: `rockx_face_detect`, `rockx_face_landmark`, `rockx_face_align`,
  `rockx_face_pose`, `rockx_face_quality`, `rockx_face_recognize`

### Запуск
- `/oem/RkLunch.sh` — автозапуск (QFacialGate закомментирован)
- `screen -S s30gui -d -m -c /home/s30/s30gui.rc` — s30gui в screen
- s30gui.rc: `screen 1 sh -c 'while true; do s30gui ; sleep 10 ; done'`
- s30.rc — отдельный screen для s30 (lua скрипты)

### Отличия от нашей платы (RV1126B aarch64)
| Параметр | Старая плата (10.1.29.246) | Наша плата (10.0.55.160) |
|----------|---------------------------|--------------------------|
| SoC | RV1126/RV1109 | RV1126B |
| Архитектура | ARMv7 armhf (32-bit) | aarch64 (64-bit) |
| Kernel | 4.19.111 (2024-03-11) | 5.10.x (2025-08-13) |
| Face SDK | RockX (librockx.so 10MB) | RockIva (librockiva.so 666KB) |
| License | **не нужна** (key.lic пустой) | нужна для face_recognition |
| Face recognition | ✅ работает | ❌ UNSUPPORTED в старой lib |
| Модели | face_*.data (33MB+75MB) | только object_detection |
| GUI | s30gui (Qt) | rkipc (без GUI) |

### Вывод для порта на нашу плату
1. **RockX (librockx.so) работает без license** на старой плате — это старый SDK
   который не требует авторизации
2. **RockIva (librockiva.so) требует license** на нашей плате — это новый SDK
3. **Для порта есть два пути:**
   - **Путь A:** Портировать librockx.so (armhf→aarch64) — если есть aarch64 сборка
     RockX SDK. Это позволит использовать готовый код percomedia без license.
   - **Путь B:** Использовать RockIva V1.23.1 с license — нужно получить license
     от Rockchip sales
4. **Путь A предпочтительнее** если:
   - RockX SDK доступен для aarch64
   - License не нужна (как на старой плате)
   - Код percomedia совместим с aarch64

### Утилиты (`rkauth/`)

| Файл | Назначение |
|------|------------|
| `rkauth_tool_bin` | Инструмент генерации лицензии |
| `rkdevice_info` | Получение device ID |
| `rkauth_verify_licence` | Проверка лицензии |
| `Rockchip_User_Guide_RKAUTH_CN.pdf` | Документация |

Платформы: Linux aarch64/armhf/armhf_uclibc/x86_64, Windows x86/x64.

**В demo коде license опциональна:**
```c
if (access(LICENSE_PATH, R_OK) == 0) {
    // загружаем license key
}
```
rkipc на плате работает БЕЗ лицензии (BA-детекция). Но face_recognition может
требовать лицензию — нужно проверить.

---

## 7. API контроля доступа (полный)

### Feature extraction
```c
// RockIvaFaceAnalyseInfo (rockiva_face_api.h:334)
struct RockIvaFaceAnalyseInfo {
    uint32_t featureSize;                     // размер feature vector
    char feature[ROCKIVA_FACE_FEATURE_SIZE_MAX]; // 4096 байт максимум
    RockIvaFaceAttribute faceAttr;            // атрибуты
};
```

### 1:1 сравнение
```c
RockIvaRetCode ROCKIVA_FACE_FeatureCompare(
    const void* feature1, const void* feature2, float* score);
// score: 0.0 - 1.0
```

### 1:N поиск по базе
```c
RockIvaRetCode ROCKIVA_FACE_SearchFeature(
    const char* libName,           // имя базы ("face")
    const void* featureData,       // feature vector
    int featureSize,
    uint32_t num,                  // сколько в базе
    int32_t topK,                  // топ-K ближайших
    RockIvaFaceSearchResults* results);
```

### Ведение базы
```c
RockIvaRetCode ROCKIVA_FACE_FeatureLibraryControl(
    const char* libName,
    RockIvaFaceLibraryAction action,  // INSERT/DELETE/UPDATE/RETRIEVAL/CLEAR
    RockIvaFaceIdInfo* faceIdInfo,
    uint32_t faceIdNum,
    const void* featureData,
    int featureSize);
```

### Порог распознавания
```c
#define FACE_RECOG_THRESHOLD 0.75  // из iva_app_face.c:16
```

---

## 8. Архитектура контроля доступа

### Регистрация (face_recog_import_demo)
```
JPG файлы (имя = faceId)
    │
    ▼
ROCKIVA_Init(ROCKIVA_MODE_PICTURE)
ROCKIVA_FACE_Init(ROCKIVA_FACE_MODE_IMPORT, faceRecognizeEnable=1)
    │
    ▼
ROCKIVA_PushFrame(image)  для каждого JPG
    │
    ▼  callback
rockiva_face_analyse_callback:
    if (qualityResult == ROCKIVA_FACE_QUALITY_OK):
        face.feature = faceAnalyseInfo.feature
        face.size = faceAnalyseInfo.featureSize
        insert_face(db, &face)  → SQLite face.db
```

### Распознавание (face_recog_picture_demo)
```
load_face_lib(face.db → ROCKIVA_FACE_FeatureLibraryControl INSERT)
    │
    ▼
ROCKIVA_Init(ROCKIVA_MODE_PICTURE)
ROCKIVA_FACE_Init(ROCKIVA_FACE_MODE_NORMAL, faceRecognizeEnable=1)
    │
    ▼
ROCKIVA_PushFrame(image)
    │
    ▼  callback
rockiva_face_analyse_callback:
    if (qualityResult == ROCKIVA_FACE_QUALITY_OK):
        ROCKIVA_FACE_SearchFeature("face", feature, size, 1, 5, &results)
        for i in results:
            printf("face_info=%s score=%f", ...)
        if (results.faceIdScore[0].score > 0.75):
            → ДОСТУП ОТКРЫТ
```

### Live камера (iva_app_face.c)
```
Камера → ROCKIVA_PushFrame (каждый кадр)
    │
    ▼  detCallback
FaceDetResultCallback:
    рисование bounding box + faceId (если уже распознан)
    опционально: ROCKIVA_FACE_SetAnalyseFace (ручной захват)
    │
    ▼  analyseCallback
FaceAnalyseResultCallback:
    ROCKIVA_FACE_SearchFeature → topK=5
    if (score > 0.75):
        обновить FaceRecord
        ROCKIVA_FACE_SetFilteredFace (не анализировать повторно)
```

---

## 9. Пайплайн интеграции с нашим VI → VO

```
Сенсор → VI (NV12, dmabuf_fd)
            │
            ▼
    ROCKIVA_PushFrame(handle, &image)
        image.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12
        image.dataFd = dmabuf_fd       ← zero-copy!
        image.dataAddr = virAddr
        image.info.transformMode = ROTATE_90  (если надо)
            │
            ▼  (асинхронно в NPU)
    RKNN inference:
        face_detection_v2 → face_landmark5 → face_quality_v2
        → face_recognition (feature extraction)
            │
            ▼  callback
    FaceAnalyseResultCallback:
        ROCKIVA_FACE_SearchFeature → face.db
        if (score > 0.75): ДОСТУП
            │
            ▼
    OSD: RK_MPI_RGN (bounding box + name + score)
            │
            ▼
    VO (дисплей)
            │
            ▼  release callback
    освобождаем VI кадр
```

---

## 10. Константы и пути (из iva_app_ctx.h)

```c
#define MODEL_DATA_PATH       "/data/rockiva_data"
#define LICENSE_PATH          "/data/key.lic"
#define IVA_RES_PATH          "/data/iva"
#define FACE_DATABASE_PATH    IVA_RES_PATH"/face.db"
#define REG_IMAGE_PATH        IVA_RES_PATH"/reg"
#define PERSON_DATABASE_PATH  IVA_RES_PATH"/person.db"
#define FONT_PATH             MODEL_DATA_PATH"/sy_7000_black.ttf"

#define ALLOC_IMAGE_BUFFER_TYPE 1   // 0:CPU 1:DMA
#define ALLOC_IMAGE_BUFFER_NUM  5
```

---

## 11. План работы

1. **Обновить librockiva.so** на плате (666KB → 3MB, V1.23.1, lib64)
2. **Загрузить face-модели** на плату в `/data/rockiva_data/`:
   - `face_recognition.data` (9.5MB) — обязательно
   - `face_detection_v2.data` (2.3MB) — обязательно
   - `face_landmark5.data` (501KB) — обязательно
   - `face_quality_v2.data` (341KB) — обязательно
   - `face_liveness_2d.data` (2.4MB) — опционально (liveness)
   - `face_attribute_v3.data` — опционально (атрибуты)
   - `object_detection_v3_cls8_640x384.data` (5.2MB) — детекция для face
3. **Создать /data/iva/** директорию для face.db
4. **Адаптировать demo** под наш pipeline:
   - Взять `face_recog_import_demo.c` для регистрации
   - Взять `face_recog_picture_demo.c` + `iva_app_face.c` для распознавания
   - Заменить V4L2Camera на наш VI (RK_MPI_VI)
   - Заменить DRM display на наш VO (RK_MPI_VO)
   - Использовать OSD (RK_MPI_RGN) для bounding boxes
5. **Протестировать**:
   - Регистрация 3-5 лиц (JPG → face.db)
   - Распознавание с камеры
   - Проверить порог 0.75
6. **Проверить лицензию** — нужен ли key.lic для face_recognition

---

## 12. Сборка

Компилятор: aarch64-linux-gnu-gcc (zig cross-compile в нашем build.sh).
Нужна lib64 версия librockiva.so (3.0MB).

CMake зависимости:
- librockiva.so
- librknnrt.so
- sqlite3 (для face_db)
- libturbojpeg (для SaveImage/ReadImage)
- librga (для ROCKIVA_IMAGE_Convert)
- stb_image (header-only)
