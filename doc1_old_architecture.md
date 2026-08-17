# Документ 1: Текущая архитектура percomedia (old, RV1109/1126)

## 1. Обзор проекта

`s30guiproj` — терминал распознавания лиц (face recognition access terminal).

**Два компонента:**
- `percomedia/` — **библиотека** (libpercomedia.so): видеозахват + нейросеть + распознавание
- `s30gui/` — **GUI** (Qt приложение): UI, сцены, WebSocket сервер, вызовы percomedia

**Платформа:** RV1109/1126 (arm 32-bit, buildroot), экран **800x1280 portrait**,
две камеры (RGB + IR) по 1920x1080.

**Запуск:** `S99-5-s30gui` → `screen -S s30gui` → `s30gui` (цикл с restart при падении).

---

## 2. Слои архитектуры

```
┌─────────────────────────────────────────────────────────┐
│  s30gui (Qt приложение)                                 │
│  ├─ AIService — обёртка над PercoMedia (callbacks→Qt)   │
│  ├─ MainWin / scenes / WebSocket / touch                │
│  └─ perco.cpp — бизнес-логика проходов                  │
└────────────────────┬────────────────────────────────────┘
                     │ PercoMedia API (cb.h: callbacks + getters)
┌────────────────────┴────────────────────────────────────┐
│  percomedia (libpercomedia.so) — Qt-библиотека          │
│  ├─ ThreadController : QObject — 2 QThread, QTimer'ы   │
│  ├─ ImageSignalProcessing : QObject — VI/RGA/VO/VENC   │
│  ├─ FaceRecogn (NNC) — rockx face pipeline              │
│  ├─ Users/UserList — база отслеживаемых лиц             │
│  ├─ ImageBuf — синхронный обмен кадрами ISP→NNC         │
│  ├─ MotionTrigger — детектор движения по трекингу       │
│  └─ Isp_LL — rk_aiq (если нет ispserver)                │
│  Зависимость: QtCore (QObject/QThread/QTimer/QMutex),  │
│              QtGui (QImage/QRect/QSize для обмена с GUI) │
└─────────────────────────────────────────────────────────┘
```

### 2.1 Qt-зависимость percomedia (ВАЖНО для порта!)

percomedia — **не standalone C-библиотека**, а Qt-библиотека (`CONFIG += QT` в percomedia.pro).
Она встроена в Qt event loop хост-приложения и использует Qt как инфраструктуру:

**QtCore (основная зависимость):**
- `QObject` — базовый класс для `ThreadController`, `ImageSignalProcessing`, `FaceRecogn`, `Users`, `ImageBuf`, `MotionTrigger`, `DbgLogger`
- `QThread` — два рабочих потока (`threadISP`, `threadNNC`)
- `QTimer` — период сканирования (100ms), детектор движения (500ms), чтение конфига
- `QMutex` — потокобезопасный обмен (`ImageBuf::clientReentry/track_module`, `LockedBuf::mux`, `Users::mux/ir_track_mux/rgb_track_mux`, `MotionTrigger::mux`)
- `Qt::QueuedConnection` — асинхронное межпоточное взаимодействие ISP↔NNC↔GUI через очередь событий Qt (15 connect'ов)
- `QQueue` — **только в #include, НЕ используется** (мёртвый include в 4 файлах)

**QtGui (вспомогательная, для обмена с GUI):**
- `QImage` — кадры лиц для callbacks в AIService (`face()` → `QImage`, `userclass.h:194` — QImage из photo)
- `QRect` / `QSize` — координаты прямоугольников лиц, зоны детекции (`setFaceCatchRect(QRect)`, `face_rect_def`)

**QDebug** — логирование (84 упоминания `qDebug()`, плюс операторы `QDebug operator<<` в dbg.cpp для rockx типов)

**Почему так сделано:** percomedia проектировалась как модуль Qt-приложения s30gui,
а не как независимая библиотека. Qt даёт готовую инфраструктуру (потоки, таймеры,
сигналы) — не нужно писать свой event loop. Обмен с GUI идёт через Qt-типы
(QImage/QRect) напрямую, без конверсии.

### 2.2 Как разорвать зависимость от Qt (реалистично)

**Цель:** сделать percomedia standalone C/C++ библиотекой, не требующей Qt.

**Сложность:** средняя. Qt используется как инфраструктура, не как UI.
Замена — механическая, но объёмная.

**Шаг 1: QtCore → POSIX/C++17 (потоки, таймеры, синхронизация)**
- `QThread` → `std::thread` (C++17) или `pthread` (POSIX)
- `QTimer` → `timer_create` (POSIX) или `std::thread` + `sleep_for` цикл
- `QMutex` → `std::mutex` (C++17) или `pthread_mutex_t`
- `QQueue` → `std::queue` + `std::mutex`
- `QObject` + сигналы/слоты → callback-функции (уже есть в cb.h!) или
  `std::function` / `std::atomic` флаги

**Шаг 2: QtGui → plain C-типы (обмен с GUI)**
- `QImage` → `struct frame { uint8_t* data; int w, h, fmt; }` или `rkmedia MEDIA_BUFFER`
- `QRect` → `struct rect { int x, y, w, h; }` (уже есть `t_rect` в cb.h!)
- `QSize` → `struct { int w, h; }`
- В API percomedia.h заменить `QImage face()` → `frame face()`,
  `setFaceCatchRect(QRect)` → `setFaceCatchRect(t_rect)`

**Шаг 3: QDebug → printf/log**
- `qDebug() << ...` → `printf(...)` или собственный логгер

**Шаг 4: Сборка**
- Убрать `CONFIG += QT` из percomedia.pro
- Или заменить .pro на CMake/Makefile без Qt
- Убрать `#include <Q*>` из всех заголовков

**Реалистичность:**
- Шаги 1-3 — механическая замена, ~1-2 дня работы
- Главный риск: `Qt::QueuedConnection` — межпоточные сигналы нужно заменить на
  thread-safe очередь + condition_variable (или просто mutex+callback)
- `ImageBuf` уже использует `QMutex` — замена на `std::mutex` тривиальна
- `t_rect` и `t_userdata` в cb.h уже C-типы — API почти готов к standalone

**Альтернатива (минимальная):** оставить Qt в percomedia, но вынести UI в
отдельный процесс. Тогда percomedia+QtCore — один процесс (headless), UI — другой.
Qt в percomedia остаётся только как QtCore (без QtGui), что уже легче.

---

## 3. API библиотеки percomedia (percomedia.h, cb.h)

**Callbacks (вызывает percomedia → получает s30gui):**
```c
void set_personid_cb(onPersonId cb);   // распознан/проверен человек
void set_recheck_cb(onPersonId cb);     // перепроверка (повторный захват)
void set_lost_cb(onLost cb);            // потерян текущий пользователь
void set_wdt_timeout_cb(onWDT cb);      // watchdog timeout
void set_motion_cb(onMotion cb);        // изменилось состояние движения
```

**Getters (вызывает s30gui):**
```c
QImage face();                         // текущий кадр лица (RGB)
std::vector<float> getVector();        // feature vector текущего (512 float)
t_rect getRect();                      // bounding box текущего
int getId();                           // track_id текущего
t_userdata getUserData();              // всё сразу: id, acc, photo, rect, feature
bool setAcc(int id, std::string acc);  // привязать аккаунт
bool setPass(int id, PassState en);    // отметить результат прохода
```

**Управление:**
```c
void setNNRun(bool st);                // старт/стоп сканирования
void resetCurrentUser();               // сбросить текущего
void setFaceCatchRect(QRect r);        // зона детекции лиц
void setFaceCatch(bool st);            // вкл/выкл зону
void motion_fake_trig();               // тестовый триггер движения
```

**Типы (cb.h):**
```c
typedef struct { int left, top, right, bottom; } t_rect;
typedef struct {
    int track_id;
    std::string acct;        // привязанный аккаунт
    std::string photo;       // base64 JPEG
    t_rect r;
    std::vector<float> face; // 512-float feature
} t_userdata;
```

---

## 4. ThreadController — главный класс (threadcontroller.cpp)

**Конструктор:**
1. Создаёт синглтоны: `ImageSignalProcessing`, `FaceRecogn`, `ImageBuf`, `Users`, `MotionTrigger`, `DbgLogger`
2. Разносит их по двум QThread: `threadISP`, `threadNNC`
3. Соединяет сигналы через `Qt::QueuedConnection` (межпоточные очереди)
4. Запускает оба потока, шлёт `init_isp` и `init_nn`
5. Настраивает таймеры:
   - `tscan` (100ms) → `FaceRecogn::onTRecognizeTask` — период сканирования
   - `tmotion` (500ms) → `MotionTrigger::check_motion_timeout`

**Состояния пользователя (percomedia_config.h):**
```c
enum UserState { CHECK, CHECK_ERR, NN, OK };
enum PassState { PASS_INIT, PASS_ERR, PASS_OK, PASS_WAIT };
```

**Автомат состояний (Users::tracklist + FaceRecogn::onTRecognizeTask):**
```
CHECK → (check_face OK) → NN → (nn_face OK) → OK
CHECK → (check_face FAIL) → CHECK_ERR → (retry) → CHECK
OK → (recheck) → OK
OK → (lost) → emit user_lost
```

---

## 5. ImageSignalProcessing (ISP) — видеопайплайн

**API:** старый `rkmedia` (RK_MPI_*) + `librga` (im2d) + `rk_aiq`.

### 5.1 Каналы VI (isp_vi.cpp)

Две камеры, каждый — отдельный VI канал:
```c
VI0 = {RK_ID_VI, dev=0, chn=0}  // RGB камера, "rkispp_scale0" /dev/video31
VI1 = {RK_ID_VI, dev=1, chn=1}  // IR камера,  "rkispp_scale0"
```
Вход: NV12 1920x1080, 2 буфера, DMA.

### 5.2 RGA — 4 канала (isp_rga.cpp)

**Главное:** RGA делает crop+scale+rotate+format-convert за один проход.

| Канал | Назначение | Вход | Выход | Rotation (runtime) |
|-------|-----------|------|-------|----------|
| RGA0 | VO (дисплей) | ARGB8888 800x1280 | RGB888 800x1280 | 0° (фиксировано) |
| RGA1 | NN RGB | NV12 1920x1080 | BGR888 720x1280 | **90°** (rga_nn_angle) |
| RGA2 | NN IR | NV12 1920x1080 | BGR888 720x1280 | **90°** (rga_nn_angle) |
| RGA3 | VO (видео) | NV12 1920x1080 crop | ARGB8888 800x1280 | **270°** (rga_vo_angle) |

**Углы настраиваются из конфига (percomedia_config.h → isp::ISPConfig):**
```c
rga_vo_angle = 270;  // угол для дисплея (RGA3)
rga_nn_angle = 90;   // угол для нейросети (RGA1, RGA2)
```
Значения по умолчанию в `ISPConfig()` (percomedia_config.h:63-64).
В `rga_init()` углы применяются к `outs[RGA3]` и `outs[RGA1/RGA2]` (isp_rga.cpp:162-165).

### 5.3 Bind-схема (ispclass.cpp proc_init)

**Полная последовательность proc_init() (ispclass.cpp:82-103):**
```c
void ImageSignalProcessing::proc_init() {
  1. if (ispserver не запущен) → new Isp_LL()     // ISP (rk_aiq)
  2. media_api_init()                              // RK_MPI_SYS_Init()
  3. vi_init()                                     // VI0 + VI1 каналы
  4. rga_init()                                    // RGA0-RGA3 каналы
  5. vo_init()                                     // VO0 канал (DRM)
  6. if (display_ch==0) bind_to_rga(VI0, RGA3)     // RGB на дисплей
     else            bind_to_rga(VI1, RGA3)        // IR на дисплей
  7. bind_to_vo(RGA0)                              // RGA0 → VO0
  8. bind_to_rga(VI0, RGA1)                        // RGB → NN
  9. bind_to_rga(VI1, RGA2)                        // IR → NN
 10. rga_cb_reg()                                  // ← регистрация callback'ов!
}
```

**Важно: `vi_cb_reg()` определён (isp_vi.cpp:89), но НИКОГДА НЕ ВЫЗЫВАЕТСЯ — мёртвый код.**
Callback'и регистрируются только через `rga_cb_reg()`.

**Поток выполнения:** `proc_init()` вызывается через сигнал `init_isp`
из `ThreadController` (threadcontroller.cpp:34,56), подключённый через
`Qt::QueuedConnection` к `ImageSignalProcessing::proc_init`.
`ImageSignalProcessing` живёт в `threadISP` (QThread). Значит **proc_init
выполняется в threadISP**.

```
VI0 (RGB) ─┬─→ RGA3 ─→ callback (display_packet_callback)
           │              → draw_rects + imcomposite
           │              → RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA, 0, mb)  ← ручная отправка, НЕ bind!
           │              → RGA0 (по каналу 0) ─→ VO0 ─→ /dev/dri/card0 (дисплей)
           │
           └─→ RGA1 ─→ callback (video_packet_callback_rgb) → ImageBuf.RGB

VI1 (IR)  ─┬─→ RGA3 (если display_ch==1, переключение камер!)
           └─→ RGA2 ─→ callback (video_packet_callback_ir)  → ImageBuf.IR
```

**Переключение камер на дисплей (display_ch):**
```c
if (iconf.display_ch == 0)
    bind_to_rga(VI0, RGA3);  // RGB на дисплей
else
    bind_to_rga(VI1, RGA3);  // IR на дисплей
```
`display_ch` из `/home/percomedia.json` (isp::ISPConfig, default=0).

**Bind'ы (RK_MPI_SYS_Bind) — только эти:**
- VI0 или VI1 → RGA3 (видео на дисплей, поворот rga_vo_angle) — **один из двух**, не оба!
- RGA0 → VO0 (на DRM overlay plane)
- VI0 → RGA1 (RGB для нейросети, поворот rga_nn_angle)
- VI1 → RGA2 (IR для нейросети, поворот rga_nn_angle)

**НЕ bind (ручная отправка):**
- RGA3 → RGA0: `display_packet_callback` принимает кадр с RGA3,
  рисует прямоугольники, вызывает `RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA, 0, mb)`
  — кадр попадает на RGA0 (канал 0), оттуда по bind → VO0 → дисплей.

### 5.4 Callbacks RGA (isp_rga.cpp rga_cb_reg)

**Регистрация (isp_rga.cpp:207-236):**
```c
callbacks[1] = video_packet_callback_rgb;  // RGA1 → ImageBuf.RGB(mb)
callbacks[2] = video_packet_callback_ir;    // RGA2 → ImageBuf.IR(mb)
callbacks[3] = display_packet_callback;     // RGA3 → draw_rects + imcomposite → VO

foreach (ch in callbacks) {
    RK_MPI_SYS_StartRecvFrame(RGA, ch, {s32RecvPicNum=0});  // стоп
    RK_MPI_SYS_RegisterOutCb(&chan, callbacks[ch]);         // регистрация
    RK_MPI_SYS_StartRecvFrame(RGA, ch, {s32RecvPicNum=-1}); // пуск бесконечно
}
```

**В каких потоках выполняются callback'и:**

Callback'и — **НЕ Qt slots**, это C-функции, зарегистрированные через
`RK_MPI_SYS_RegisterOutCb`. Вызываются **напрямую из mpp-потоков** rkmedia
(синхронный вызов, НЕ через Qt очередь).

Из комментария в isp_rga.cpp:204:
> «Треды в которых оно вертится предоставляет mpp, и они разные для камер»

Из комментария в ispclass.cpp:247:
> «ВАРНИНГ: Зависание в каллбэке приведет к зависанию в соответствующем потоке rkmedia!
>  Подтверждение ворнинга: при использовании QBlockingQueue проводами испускаемых
>  тут сигналов - стопается картинка на дисплее.»

**Потоки callback'ов (mpp, НЕ Qt):**
- `video_packet_callback_rgb` (RGA1) — поток mpp для RGB камеры
- `video_packet_callback_ir` (RGA2) — поток mpp для IR камеры (другой поток!)
- `display_packet_callback` (RGA3) — поток mpp для дисплея

**Межпоточное общение — два механизма:**

1. **RGA callback → ImageBuf** — прямой вызов C-функции в mpp-потоке,
   запись в `LockedBuf` под мьютексом (`track_module.tryLock(10)`).
   Никакой Qt очереди — синхронно в потоке mpp.

2. **Qt signals/slots с `Qt::QueuedConnection`** — событие кладётся в event loop
   потока-приёмника, слот выполнится когда поток дойдёт до него:
   - `init_isp` → `proc_init` (ThreadController → threadISP)
   - `tscan timeout` → `onTRecognizeTask` (ThreadController → threadNNC)
   - `Users::user_change` → `on_face` (mpp-поток → главный поток ThreadController)
   - `Users::user_lost` → `on_lost` (mpp-поток → главный поток)
   - `MotionTrigger::motion_changed` → `on_motion` (mpp-поток → главный поток)
   - `ImageSignalProcessing::wdt` → `DbgLogger::dbg` (mpp → главный поток)
   - `ImageBuf::wdt` → `DbgLogger::dbg` (mpp → главный поток)

   **Потоки объектов (moveToThread):**
   - `ImageSignalProcessing` → `threadISP`
   - `FaceRecogn` → `threadNNC`
   - `Users`, `ImageBuf`, `MotionTrigger`, `DbgLogger` — **главный поток** ThreadController
     (не moveToThread, живут в потоке где созданы)

   Callback'и RGA **эмитят Qt-сигналы** (`emit Instance()->wdt(...)`,
   `emit user_change(...)`, `emit motion_changed(...)`) из mpp-потоков —
   эти сигналы идут через `QueuedConnection` в потоки приёмников
   (главный поток для DbgLogger/Users/MotionTrigger).
   Но сама работа callback'а (track, ImageBuf, draw_rects) — в mpp-потоке.

### 5.5 Рисование прямоугольников (ispclass.cpp draw_rects + display_packet_callback)

**Оверлей-буфер `rectangle_mb`** (ARGB8888 800x1280) — отдельный буфер, создаётся в конструкторе.

**display_packet_callback(mb) — каждый кадр с RGA3:**
```c
1. src_mb = wrapbuffer_fd(mb, RGBA8888)            // видео
2. rect_mb = wrapbuffer_fd(rectangle_mb, RGBA8888)  // оверлей
3. draw_rects(rect_mb):
   - imfill(rect_mb, full, 0x00000000)              // очистка
   - для каждого user в UserList:
       imfill(rect_mb, rect_up,    color)           // 4 линии бокса
       imfill(rect_mb, rect_buttom,color)
       imfill(rect_mb, rect_left,  color)
       imfill(rect_mb, rect_right, color)
4. imcomposite(src_mb, rect_mb, src_mb, IM_ALPHA_BLEND_DST_OVER)  // смешивание
5. imsync()
6. RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA, 0, mb)    // → RGA0 → VO → дисплей (НЕ bind!)
```

**Цвета по состоянию (draw_rects):**
- `CHECK` — жёлтый `#FFFF00`
- `CHECK_ERR` — оранжевый `#FF8000`
- `NN` — небесный `#87CEEB`
- `PASS_INIT` — синий `#001B51`
- `PASS_ERR` — красный `#AD2828`
- `PASS_OK` — зелёный `#1E7514`
- `PASS_WAIT` — голубой `#ADD8E6`
- Текущий пользователь — линия 9px, остальные — 5px

**Важно:** координаты прямоугольников приходят из нейросети (rockx детектит на
повёрнутом кадре 720x1280 portrait), а рисование идёт в 800x1280 portrait.
Масштаб 720→800 по X, 1280→1280 по Y — учитывается в draw_rects через `VO_DISP_W/H`.

### 5.6 VENC — JPEG энкодер (isp_enc.cpp) — МЁРТВЫЙ КОД!

**ВНИМАНИЕ: `venc_init()` и `bind_to_venc()` НИКОГДА НЕ ВЫЗЫВАЮТСЯ!**
В `proc_init()` (ispclass.cpp:82-103) нет вызова `venc_init()`.
Код определения ENC0/ENC1, колбэков, bind'ов — существует, но не используется.

Два канала для снимков (определено, но НЕ вызывается):
```c
ENC0 = {RK_ID_VENC, chn=0}  // RGB JPEG 1920x1080, 1 fps
ENC1 = {RK_ID_VENC, chn=1}  // IR  JPEG 1920x1080, 10 fps
```
Колбэки `video_packet_callback_rgb/ir` в VENC зарегистрированы, но
bind VI→VENC не происходит — **JPEG файлы `/tmp/rgb.jpeg` и `/tmp/ir.jpeg`
не создаются**.

**Вывод:** VENC — мёртвый код. Фото для распознавания берётся из
ImageBuf (rockx_image_t из RGA1/RGA2 callback), а не из JPEG.

### 5.7 Isp_LL — rk_aiq (ispll.h)

Если `ispserver` не запущен, percomedia сам поднимает ISP через `rk_aiq_*` API:
- `SAMPLE_COMM_ISP_Init` — инициализация ISP (iq файлы из `/etc/iqfiles/`)
- Настройки: exposure, white balance, brightness, contrast, HDR, mirror, crop
- **Управление IR LED через CPSL** (см. 5.8) — НО функции определены и НЕ вызываются

### 5.8 IR LED / подсветка — МЁРТВЫЙ КОД (ВАЖНО для порта!)

**В коде есть ДВА механизма управления IR подсветкой, ОБА ЗАКОММЕНТИРОВАНЫ:**

#### 5.8.1 GPIO-импульсы (userclass.h, строки 455-488, 506-511) — ЗАКОММЕНТИРОВАНО

```c
/*  void onLedTick(){                          // таймер 10ms
        Instance()->ir_led_tick();
    }*/

/*  void ir_led_tick(){
      if(ir_led != nullptr){
          if(!motion_st)
              ir_led->value(0);                // выключить если нет движения
          else{
          cr ++;  cr %= period;                 // period=20
          ir_led->value((cr < ccr)?1:0);        // ccr=10 → ШИМ 50% на 10ms период
          }
      } else {
          cr = 0;
          ir_led = new pad(IR_PAD);             // класс pad НЕ определён в проекте!
          ir_led->direction(1);
          ir_led->value(0);
      }
  }*/
//QThread* t_led = new QThread();
//QTimer* ledtim;
//pad* ir_led = nullptr;
//int cr = 0;  int ccr = 10;  int period = 20;
```

**Состояние:**
- Весь блок в `/* ... */` — **закомментирован**
- Таймер `ledtim` в конструкторе `Users()` — **закомментирован** (строки 179-185)
- Класс `pad` — **НЕ определён** в проекте (нет `class pad` нигде)
- `IR_PAD` — **НЕ определён** (нет `#define IR_PAD`)
- **Этот код никогда не компилировался и не работал**

**Что пытался сделать автор:**
- Таймер 10ms дёргает GPIO IR LED по ШИМ (50% скважность, период 20 тиков = 200ms)
- Идея (невнятная): мигать IR фонариком синхронно с кадрами, чтобы сэкономить
  питание и не светить непрерывно
- Привязка к `motion_st` — светить только при движении
- **Зависимость от кадров НЕ реализована** — просто ШИМ с фиксированным периодом

#### 5.8.2 CPSL через rk_aiq (ispll.h/.cpp, строки 512-546) — ОПРЕДЕЛЕНО, НЕ ВЫЗЫВАЕТСЯ

```c
RK_S32 Isp_LL::SAMPLE_COMM_ISP_SET_OpenColorCloseLed(RK_S32 CamId) {
    g_cpsl_cfg[CamId].lght_src = RK_AIQ_CPSLS_IR;
    g_cpsl_cfg[CamId].mode = RK_AIQ_OP_MODE_MANUAL;
    g_cpsl_cfg[CamId].gray_on = RK_FALSE;
    g_cpsl_cfg[CamId].u.m.on = 0;               // выключить
    g_cpsl_cfg[CamId].u.m.strength_led = 0;
    g_cpsl_cfg[CamId].u.m.strength_ir = 0;
    ret = SAMPLE_COMM_ISP_SET_CPSL_CFG(CamId, &g_cpsl_cfg[CamId]);
}

RK_S32 Isp_LL::SAMPLE_COMM_ISP_SET_GrayOpenLed(RK_S32 CamId, RK_U8 u8Strength) {
    g_cpsl_cfg[CamId].mode = RK_AIQ_OP_MODE_MANUAL;
    g_cpsl_cfg[CamId].lght_src = RK_AIQ_CPSLS_IR;
    g_cpsl_cfg[CamId].gray_on = RK_TRUE;
    g_cpsl_cfg[CamId].u.m.on = 1;               // включить
    g_cpsl_cfg[CamId].u.m.strength_led = u8Strength / 5 + 3;
    g_cpsl_cfg[CamId].u.m.strength_ir = u8Strength / 5 + 3;
    ret = SAMPLE_COMM_ISP_SET_CPSL_CFG(CamId, &g_cpsl_cfg[CamId]);
}
```

**Состояние:**
- Функции **определены** в ispll.cpp и объявлены в ispll.h
- **НИКТО ИХ НЕ ВЫЗЫВАЕТ** — `grep` по всему `s30guiproj` находит только определения
- Это API rk_aiq для управления IR/LED подсветкой через ISP (CPSL = Color/light Source)
- `GrayOpenLed(strength)` — включить IR с заданной силой
- `OpenColorCloseLed()` — выключить IR, перейти в цветной режим

**Вывод:** управление IR подсветкой в old percomedia **полностью отсутствует в runtime**.
IR фонарь (если есть аппаратно) управляется отдельным процессом/драйвером вне percomedia,
либо не управляется вообще (всегда вкл/выкл аппаратно).

---

## 6. FaceRecogn (NNC) — нейросетевой pipeline (nnclass.cpp)

**API:** старая `librockx` (rockx_*).

### 6.1 Модули rockx (proc_init)

```c
ModMap = {
    {ROCKX_MODULE_FACE_LANDMARK_5,   NULL},  // 5 точек лица
    {ROCKX_MODULE_FACE_LANDMARK_106, NULL},  // 106 точек
    {ROCKX_MODULE_OBJECT_TRACK,     NULL},  // трекинг объектов
    {ROCKX_MODULE_FACE_DETECTION,    NULL},  // детекция лица
    {ROCKX_MODULE_FACE_RECOGNIZE,    NULL},  // распознавание (feature vector)
    {ROCKX_MODULE_FACE_LIVENESS,     NULL},  // 2D liveness
};
// Модели: /usr/lib/*.data (ROCKX_DATA_PATH)
```

### 6.2 Полный pipeline распознавания (check_face + nn_face)

```
1. rockx_face_detect(FACE_DETECTION)        → bounding box лица
2. Фильтр SCORE: score > facescore_min (0.9)
3. Фильтр SIZE:  area > min_size_px2 (20000) + в зоне face_rect
4. rockx_face_quality(LANDMARK_5, RGB)       → качество (угол/blur/яркость)
   rockx_face_quality(LANDMARK_5, IR)        → то же для IR
   Фильтр: оба PASS
5. rockx_face_landmark(LANDMARK_5, RGB)      → 5 точек
6. rockx_face_pose(landmarks)                → pitch/yaw/roll
   Фильтр: pitch ±50°, yaw ±20°, roll ±30°
7. rockx_image_equalize_hist(RGB)            → гистограммная эквализация
8. rockx_face_align(LANDMARK_5, RGB, box)    → выравненное лицо 112x112
9. rockx_face_recognize(FACE_RECOGNIZE, aligned) → 512-float feature vector
10. (опц.) rockx_face_liveness(FACE_LIVENESS, aligned) → liveness score
```

**Параметры фильтров (percomedia_config.h nn::t_nn):**
```c
facescore_min  = 0.9f;
livescore_min  = 0.04f;
min_size_px2   = 20000;
max_pitch = 50.0;  min_pitch = -50.0;
max_yaw   = 20.0;  min_yaw   = -20.0;
max_roll  = 30.0;  min_roll  = -30.0;
rgb_blur_max = 0.3;  ir_blur_max = 0.3;
photo_en = true;  photo_w = 300;  photo_h = 300;  photo_margin = 1.5;
```

### 6.3 Трекинг (track)

**Важно: `track()` вызывается НЕ только в таймере, но и в ImageBuf callbacks!**

В `ImageBuf::RGB(mb)` и `ImageBuf::IR(mb)` (imagebuf.cpp:42,70):
```c
FaceRecogn::Instance()->track(b, rgb_arr);  // вызывается на каждый кадр!
```

Сам `track()` (nnclass.cpp:520-526):
```c
rockx_face_detect(FACE_DETECTION, pic, &obj, nullptr);
rockx_object_track(OBJECT_TRACK, w, h, 10, &obj, obj_track);
```
Результат — `rockx_object_array_t` с id, box, score для каждого отслеживаемого лица.

Также в callbacks вызывается `Users::tracklist()` — обновление списка
пользователей происходит **на каждый кадр** (imagebuf.cpp:48,76),
а не только в таймере сканирования.

### 6.4 Таймер сканирования (onTRecognizeTask)

Таймер `tscan` (scan_period_ms=100) запускается через `setScanEnable(true)`
(вызывается из `PercoMedia::setNNRun(true)`).

**Важно: `tscan->start()` закомментирован в конструкторе** (threadcontroller.cpp:67)
и запускается только при `setScanEnable(true)`.

```c
void onTRecognizeTask() {
    int id = Users::Instance()->get_user();  // выбрать кандидата
    if (id < 0) return;
    if (ImageBuf::lockPic(ir, rgb)) {        // захват пары IR+RGB (без проверки isSync!)
        user u = user_list.user_by_id(id);
        if (st == CHECK || st == CHECK_ERR)
            check_face(rgb, ir, id);          // шаг 1-6 (фильтры)
        if (st == NN || st == OK)
            nn_face(rgb, id, out_feature);    // шаг 7-9 (feature)
        ImageBuf::unlockPic();
    }
}
```

**Архитектурный недостаток: QueuedConnection для onTRecognizeTask избыточен.**

`tscan` (100ms) → `Qt::QueuedConnection` → `onTRecognizeTask` в threadNNC.
Если recognize длится дольше 100ms — тики копятся в очереди threadNNC.
Когда очередь доходит — каждый тик берёт один и тот же latest кадр (lockPic
берёт последний, не тот что при тике) и делает ту же работу повторно.

**Правильнее:** распознавание не требует очереди событий — нужен актуальный
кадр. Лучше:
- Прямой вызов из callback'а (как `track()` — синхронно в mpp-потоке)
- Или condvar/флаг в callback'е, поток NN забирает когда готов — без очереди тиков

QueuedConnection здесь работает как "межпоточный вызов", но копит лишние
события. Для медленного recognize это не страшно (лишние тики отбрасываются
по `get_user()`), но архитектурно избыточно.

### 6.5 Фото для GUI (nn_face, опционально)

```c
if (fcfg.photo_en) {
    rockx_image_convert_with_crop(&in_rgb, &in_r, &photo, NONE);
    user_list.set_photo(id, &photo);  // → base64 JPEG в user.b64png_photo
}
```

---

## 7. Users / UserList — база отслеживаемых лиц (userclass.h/.cpp)

### 7.1 Структура user

```c
struct user {
    int id = -1;                       // track_id из rockx_object_track
    rockx_rect_t rect;                 // bounding box (в координатах NN 720x1280)
    UserState ustate = CHECK;           // CHECK/CHECK_ERR/NN/OK
    PassState pstate = PASS_INIT;       // PASS_INIT/ERR/OK/WAIT
    int chstate = 0;                   // битовые флаги ошибок (CheckState)
    qint64 timestamp;                  // время последнего обновления
    std::vector<float> list;            // 512-float feature vector
    std::string acct;                   // привязанный аккаунт (от GUI)
    std::string b64png_photo;           // base64 JPEG фото
};
```

### 7.2 UserList — потокобезопасный список

QMutex + QList<user>, сортировка по площади бокса (большие лица приоритетнее).
Методы: append_or_update, remove, set_user_state, set_feature, set_photo,
set_acc, set_pass, get_stat_users(по состоянию).

### 7.3 Users — логика выбора текущего

**tracklist()** — обновляет список по результатам трекинга:
1. Добавляет новые лица (score > 0.9, площадь > 5000, не у края кадра)
2. Удаляет пропавшие/ушедшие за границу
3. Сортирует по площади
4. Выбирает `current_user_id` — самый большой OK-пользователь (антидребезг: не
   менять, если разница площадей < 3000)
5. Шлёт `user_change` / `user_lost` сигналы

**get_user()** — выбирает кандидата для обработки:
1. NN (готов к распознаванию)
2. CHECK (новый, нужна проверка)
3. CHECK_ERR (ошибка, retry через 500ms)
4. OK (recheck через 5000ms)

### 7.4 MotionTrigger — детектор движения

Сравнивает смещение bounding box'ов между кадрами (трекинг).
Если суммарное смещение > 0.005 периметра → `stamp_motion()`.
Таймаут 10 сек → `motion_changed(false)`.
Используется для включения IR LED и активации сканирования.

---

## 8. ImageBuf — синхронный обмен кадрами ISP→NNC (imagebuf.h)

ISP шлёт кадры в callback'ах RGA1/RGA2:
```c
video_packet_callback_rgb(mb) → ImageBuf::RGB(mb)
video_packet_callback_ir(mb)  → ImageBuf::IR(mb)
```

**Важно: в callback'ах происходит track() и tracklist() — НЕ только в таймере!**

`ImageBuf::RGB(mb)` (imagebuf.cpp:59-88):
1. `Users::rgb_obj_array()` — получить массив объектов
2. `Users::maxscore(rgb_arr)` — максимальный score
3. `MotionTrigger::rect_filter(rgb_arr)` — фильтр движения
4. `track_module.tryLock(10)` — мьютекс трекинга (10ms таймаут)
5. `FaceRecogn::track(b, rgb_arr)` — **трекинг** (rockx_face_detect + rockx_object_track)
6. Если `m_score > 0.98` → `bRGB.get_buf(b)` — сохранить кадр (высокий порог!)
7. `Users::tracklist()` — **обновление списка пользователей**
8. `track_module.unlock()`

`ImageBuf::IR(mb)` (imagebuf.cpp:33-57):
1. `Users::ir_obj_array()` — получить массив объектов
2. `Users::maxscore(ir_arr)` — максимальный score
3. `track_module.tryLock(10)` — мьютекс трекинга
4. `FaceRecogn::track(b, ir_arr)` — **трекинг**
5. Если `m_score > 0.85` → `bIR.get_buf(b)` — сохранить кадр (порог ниже RGB!)
6. `Users::tracklist()` — **обновление списка пользователей**
7. `track_module.unlock()`

**Пороги сохранения кадра:** RGB `m_score > 0.98`, IR `m_score > 0.85`.
Разные пороги — RGB строже (нужно качественное фото), IR слабее (достаточно детекции).

NNC забирает пару синхронно:
```c
if (ImageBuf::lockPic(ir, rgb)) {  // ждёт пока оба буфера готовы
    // ... обработка ...
    ImageBuf::unlockPic();
}
```

Реализация: два `Lockedbuf` (bIR, bRGB) с мьютексами. Если кадр не забрали —
новый перезаписывает старый (drop).

**`isSync()` — ОТКЛЮЧЁН!** Функция определена (imagebuf.cpp:24-31), проверяет
что оба кадра свежие (разница PTS < FR_DROP_MS=100ms), но вызовы
**закомментированы** в `ImageBuf::RGB()` и `ImageBuf::IR()`:
```c
//if (!isSync())
    bRGB.get_buf(b);   // сохраняет БЕЗ проверки синхронности
```
Кадры сохраняются в буфер **независимо** от синхронности RGB↔IR.
Синхронность не проверяется — `lockPic` просто берёт последние доступные.

---

## 9. Конфигурация (percomedia_config.h)

Три структуры, загружаются из `/home/percomedia.json`:

**isp::t_isp** — настройки видео:
```c
display_ch = 0;        // 0=RGB на дисплей, 1=IR
ir_auto = true;        // автоэкспозиция IR
rgb_auto = true;
rga_vo_angle = 270;    // угол поворота дисплея
rga_nn_angle = 90;     // угол поворота для нейросети
hdr_mode = false;
manual_shutter_ir = 3;
manual_shutter_rgb = 8;
```

**nn::t_nn** — настройки распознавания (см. 6.2).

**thrctl::ThrCtrlConfig** — тайминги:
```c
scan_period_ms = 100;   // период сканирования (10 fps)
frame_drop_ms = 100;     // окно синхронности IR+RGB
```

---

## 10. GUI (s30gui) — Qt приложение

### 10.1 Точка входа (main.cpp)

```c
QApplication a(argc, argv);
qputenv("QT_QPA_PLATFORM", "linuxfb:rotation=180:size=800x1280");
AIService::Instance();         // создаёт PercoMedia, регистрирует callbacks
AIService::enStream(true);      // запускает сканирование
JsonService::Instance();        // WebSocket сервер
MainWin mwin; mwin.show();
a.exec();
```

### 10.2 AIService — мост percomedia → Qt (aiservice.h)

Обёртка над `PercoMedia*`:
- Создаёт `libPM = new PercoMedia()` в конструкторе
- Регистрирует static callbacks → emit Qt signals
- `on_face(t_userdata)` → `emit onFace(UserData)` (с конверсией типов)
- Геттеры: `getPhoto()`, `getUserData()`, `getId()`, `getVector()`, `getRect()`
- `setVideoFrame(bool)` — вкл/выкл сканирование (через `setNNRun`)
- `setFaceCatch(bool)` — зона детекции

### 10.3 Связь с внешним миром

**JsonService** — WebSocket сервер (QWsServer), принимает команды от сервера
PERCo-S30 (проходы, аккаунты, настройки). Отправляет события (распознан,
потерян, движение).

**MainWin** — главное окно со сценами (QGraphicsView):
- `svideo` — видео + прямоугольники (через AIService::getRect)
- `sadduser`, `sedituser` — добавление/редактирование пользователей
- `seventlist` — список событий
- `sdevice` — настройки устройства
- `passbar`, `toppanel` — UI элементы

### 10.3.1 Жёсткая зависимость от сервера PERCo-S30 (критично!)

**Проект s30guiproj — тонкий клиент к серверу PERCo-S30.** GUI
**запускается** без S30, **видео идёт на экран** без S30, но
**распознавание лиц работает только когда S30 подключён**.

**Ключевая находка — двойное условие для распознавания** (aiservice.h):

```c
// aiservice.h:160-178 — setVideoFrame
static void setVideoFrame(bool is_video){
    is_videoframe = is_video;
    if(is_videoframe && is_s30connected) {   // ДВА условия!
        libPM->setNNRun(true);                // распознавание ON
    } else {
        libPM->setNNRun(false);               // распознавание OFF
    }
}

// aiservice.h:199-216 — onS30Connected
void onS30Connected(bool is_connected){
    is_s30connected = is_connected;
    if(is_videoframe && is_s30connected) {   // ДВА условия!
        libPM->setNNRun(true);                // распознавание ON
    } else {
        libPM->setNNRun(false);               // распознавание OFF
    }
}
```

**Распознавание (`setNNRun`) включается ТОЛЬКО когда ОБА условия истинны:**
- `is_videoframe == true` — текущая сцена показывает видео (SVideo, SClock)
- `is_s30connected == true` — S30 подключён к WebSocket

**Инициализация:**
- `is_s30connected = false` (aiservice.cpp:3) — стартует с false
- `is_videoframe = false` (aiservice.cpp:4) — стартует с false
- На ПК (`#ifndef ROCKCHIP`): `is_connected()` возвращает true (jsonservice.h:21),
  `ConnectChanged(true)` эмитится через 1 сек (jsonservice.h:84) — S30 не нужен

**Цепочка управления распознаванием от S30:**

1. **S30 подключается** → `JsonService::processNewConnection` (jsonservice.h:41-52)
   → `client = clientSocket` → `emit ConnectChanged(true)`
2. **AIService** подключён к `ConnectChanged` (aiservice.h:240)
   → `onS30Connected(true)` → `is_s30connected = true`
3. Если `is_videoframe` тоже true → `setNNRun(true)` → распознавание ON
4. **S30 отключается** → `socketDisconnected` (jsonservice.h:53-56)
   → `client = nullptr` → `emit ConnectChanged(false)`
5. → `onS30Connected(false)` → `is_s30connected = false` → `setNNRun(false)`
   → распознавание OFF

**Что делает `setNNRun` в percomedia** (threadcontroller.cpp:158-169):
```c
void ThreadController::setScanEnable(bool st){
    if(st){
        tscan->start();                          // таймер сканирования NN
        ImageSignalProcessing::Instance()->onMute(false);  // b_mut=false
    } else {
        tscan->stop();                            // остановка сканирования NN
        ImageSignalProcessing::Instance()->onMute(true);   // b_mut=true
    }
}
```

**`onMute` (b_mut) управляет передачей кадров в NN, НЕ видео на экран** (ispclass.cpp):
```c
// ispclass.cpp:220-242 — display_packet_callback (видео на экран)
void ImageSignalProcessing::display_packet_callback(MEDIA_BUFFER mb) {
    if(!b_mut){
        draw_rects(rect_mb);                      // рисуем прямоугольники лиц
        imcomposite(src_mb, rect_mb, src_mb, ...); // alpha blend
    }
    RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA,0,mb);   // ВИДЕО ИДЁТ ВСЕГДА!
    RK_MPI_MB_ReleaseBuffer(mb);
}

// ispclass.cpp:251-258 — video_packet_callback_rgb (кадры в NN)
void ImageSignalProcessing::video_packet_callback_rgb(MEDIA_BUFFER mb) {
    if(!b_mut){
        ImageBuf::Instance()->RGB(mb);           // кадр в NN pipeline
    }
    RK_MPI_MB_ReleaseBuffer(mb);                 // буфер освобождается ВСЕГДА
}

// ispclass.cpp:260-267 — video_packet_callback_ir (то же для IR)
```

**ИТОГО по `b_mut` (mute):**
- `b_mut == true` (S30 отключён): видео **ИДЁТ на экран** (VO работает),
  но кадры **НЕ идут в NN** (ImageBuf не вызывается), прямоугольники **НЕ рисуются**
- `b_mut == false` (S30 подключён): видео идёт на экран **И** кадры идут в NN,
  прямоугольники рисуются → **распознавание работает**

**Особый случай — `enStream` в main.cpp:**
- `main.cpp:100` — `AIService::enStream(true)` → `setNNRun(true)` напрямую
  **БЕЗ проверки is_s30connected** (aiservice.h:148-152)
- Это включает NN при запуске, но **первая же видео-сцена** (SVideo) вызовет
  `setView` → `setVideoFrame(true)` → если S30 не подключён → `setNNRun(false)`
- В итоге NN выключится когда SVideo покажется без S30

**SVideo — главная видео-сцена** (svideo.h:109-128):
```c
SVideo() : Fragment(){
    is_videoframe = true;                         // это видео-сцена
    if(JsonService::Instance()->is_connected()){
        settings_src.req_conf();                  // настройки от S30
    } else {
        connect(JsonService::Instance(),&JsonService::ConnectChanged,
                this,[this](bool st){
            if(st) settings_src.req_conf();       // ждём S30 для настроек
        });
    }
}
```
- `is_videoframe = true` — SVideo помечает себя как видео-сцена
- `req_conf()` запрашивается только когда S30 подключён (или ждёт подключения)
- При показе SVideo → `Fragment::setView` (fragment.h:215-222) → `setVideoFrame(true)`
  → проверка `is_videoframe && is_s30connected`

**Полная картина работы без S30:**

| Что работает без S30 | Что НЕ работает без S30 |
|-----------------------|--------------------------|
| ✅ GUI запускается (main.cpp) | ❌ Распознавание лиц (setNNRun=false) |
| ✅ Видео на экране (VO работает) | ❌ Прямоугольники лиц (b_mut=true) |
| ✅ Видеозахват (VI работает) | ❌ Настройки устройства (req_conf ждёт S30) |
| ✅ DRM overlay (видео в дисплей) | ❌ Проходы (некому отправить identification) |
| ✅ Дисплей backlight (sysfs) | ❌ Замок (exdev на S30) |
| ✅ Локальные видеонастройки | ❌ Подсветка IR LED (on_motion на S30) |
| | ❌ Журнал событий (eventlist на S30) |

**Доказательства из кода (каждая строка проверена):**

1. **GUI запускается без S30, видео идёт, распознавание выключено:**
   - `main.cpp:99-105` — запуск без блокирующего ожидания S30
   - `aiservice.h:165,203` — `setNNRun` только при `is_videoframe && is_s30connected`
   - `ispclass.cpp:229,238` — видео на экран даже при `b_mut=true`
   - `ispclass.cpp:253,262` — кадры в NN только при `b_mut=false`
   - **Доказано:** без S30 видео идёт, распознавание выключено

2. **Конфигурация приходит от S30, не из локального файла:**
   - `onJsonAnsw` (devicesrc.h:122-140) — `d.setJson(jo_settings)` (строка 135)
     заполняет `DeviceData` из ответа S30
   - `DeviceData::setJson(QJsonObject)` (perco.h:565) заполняет:
     `led_ir_level`, `led_white_level`, `licon_light`,
     `severity_face_identify`, `sleep_time`, `scan_time`, `device_mode`,
     `fl15_*` параметры — **все от сервера S30**
   - `svideo.h:120-126` — `req_conf()` только когда S30 подключён
   - Локальный `percomedia.json` (percomedia_config.h:17,
     `#define CONFIG_FILE "/home/percomedia.json"`) — только видеонастройки
   - **Доказано:** бизнес-настройки — от S30, видеонастройки — локально

3. **Бизнес-логика проходов — на S30:**
   - `svideo.h:568` — отправка распознанного лица на S30:
     ```c
     QString("{\"event\":\"face\",\"face\":{\"name\":\"identification\", %1}}")
         .arg(b64face_template);
     ```
   - `sfaceremotecatch.h:183` — то же: `{"event":"face","face":{"name":"identification",...}}`
   - Сервер S30 решает: открыть замок? записать проход? заблокировать?
   - GUI только отображает результат, не принимает решений
   - **Доказано:** без S30 — нет проходов (некуда отправить identification)

4. **Пользователи (accounts) — на S30:**
   - `sadduser.h:644` — отправка на S30:
     ```c
     QString("{\"set\":\"uid\",\"uid\":{\"acct\":\"%1\",\"value\":\"%2\"}}")
     ```
   - `sedituser.h:634` — то же: `{"set":"uid","uid":{"acct":...,"value":...}}`
   - `sadduser.h:600` — запрос от S30: `{"get":"uid","uid":{"user_acct":...}}`
   - Сервер S30 хранит базу аккаунтов, привязку face_id → account
   - percomedia хранит только feature vectors в памяти:
     - `user` struct (userclass.h:29): `id, rect, list (feature), acct, b64png_photo`
     - `UserList` (userclass.h:55) — `QList<user> usrlist` в памяти
     - **Нет save/load на диск** (grep: только `img.save(&buffer,"JPG")` для
       base64 в память, не на диск; `QFile` не используется в userclass.h)
   - **Доказано:** аккаунты — на S30, percomedia — только feature vectors в памяти

5. **IR LED подсветка — на S30** (см. док 2, раздел 11.8):
   - `mainwin.cpp:230` — `{"event":"face","face":{"name":"on_motion"}}` → S30
   - `mainwin.cpp:234` — `{"event":"face","face":{"name":"off_motion"}}` → S30
   - `sscreen.h:87` — `led_ir_level` (0-8) → `send(jo)` → S30
   - **Доказано:** без S30 — нет подсветки (некому включить IR LED)

6. **Шаблоны настроек — на S30:**
   - `write_template` (devicesrc.h:33-66) читает `://fl15_template.json`
     локально (строка 36-38), но **отправляет на S30** для применения:
     ```c
     JsonService::Instance()->onTransmit(
         QString("{\"set\":\"settings\",\"settings\":{\"template\":\"%1\"}}").arg(key));
     ```
   - Через `onSeriesTick` (devicesrc.h:96-108) отправляет каждую команду
     из серии на S30
   - **Доказано:** шаблон применяется на S30, не локально

7. **Замок/реле (exdev) — на S30:**
   - `exdevsrc.h:22` — `{"set":"exdev","exdev":...}` → S30
   - `exdevsrc.h:29` — `{"get":"exdev","exdev":{}}` → запрос от S30
   - `fl15_template.json` — `"server_addr":"127.0.0.1","server_port":"3002"`
     (reader подключается к S30 на порт 3002)
   - **Доказано:** замок управляется через S30, не напрямую из GUI

8. **События (журнал проходов) — на S30:**
   - `eventlistsrc.h:289` — `{"get":"event_count"}` → S30
   - `eventlistsrc.h:487` — `{"get":"event","event":{"done":true,...}}` → S30
   - **Доказано:** журнал хранится на S30, GUI его запрашивает

**Что НЕ подтверждено (гипотезы, требуют доступа к S30):**

- ❓ **IR-cut фильтр управление:** в s30gui нет упоминаний `ircut`/`ir_cut`
  (grep: 0 совпадений). Кто управляет ircut — S30, rkaiq, или никто —
  **неизвестно**. В old percomedia (ispll.cpp) есть `rk_aiq_uapi_sysctl_setCpsLtCfg`
  но она не вызывается. См. док 2, раздел 11.6, пункт 4.

**Что S30 контролирует (доказано):**

| Функция | Где | Доказательство | Без S30 |
|---------|-----|----------------|---------|
| Распознавание лиц | aiservice.h:165,203 | setNNRun=false | ❌ Выключено |
| Настройки устройства | S30 → JSON → GUI | devicesrc.h:88,135 | Дефолтные/пустые |
| Бизнес-логика проходов | S30 (решение) | svideo.h:568 | Нет проходов |
| База аккаунтов | S30 (хранение) | sadduser.h:644 | Нет управления |
| Замок/реле (exdev) | S30 → GPIO | exdevsrc.h:22 | Не открывается |
| IR LED подсветка | S30 → GPIO/PWM | mainwin.cpp:230 | Нет подсветки |
| Шаблоны устройства | S30 (применение) | devicesrc.h:55 | Не применяются |
| Журнал событий | S30 (хранение) | eventlistsrc.h:487 | Нет журнала |

**Что s30guiproj делает сам (без S30, доказано):**

| Функция | Где | Работает без S30? |
|---------|-----|---------------------|
| GUI запуск | main.cpp | ✅ Да (не блокируется) |
| Видеозахват (VI) | percomedia | ✅ Да |
| Видео на экран (VO) | percomedia → DRM | ✅ Да (b_mut не влияет на VO) |
| Локальные видеонастройки | percomedia.json | ✅ Да |
| Дисплей backlight | s30gui (sysfs) | ✅ Да |
| Face detection/recognition | percomedia (rockx) | ❌ Нет (setNNRun=false) |
| Трекинг лиц | percomedia | ❌ Нет (кадры не идут в NN) |
| Прямоугольники лиц | percomedia (RGA) | ❌ Нет (b_mut=true) |
| UI сцены | s30gui (Qt) | ⚠️ Запускаются, но без данных от S30 |

**Вывод:** s30guiproj — это **тонкий клиент** к серверу PERCo-S30.
GUI **запускается** без S30, **видео идёт на экран** без S30 (VO работает
независимо от `b_mut`), но **распознавание лиц работает только когда S30
подключён** (двойное условие `is_videoframe && is_s30connected` в
`setVideoFrame` и `onS30Connected`). Без S30 терминал показывает видео,
но не распознаёт лица — бесполезен как устройство контроля доступа.

**Это критично для порта на RV1126B:**
- Если S30 недоступен/не портируется — нужно **заменить** его функции
  (настройки, проходы, аккаунты, подсветка, замок, журнал) на локальные
- **Убрать двойное условие `is_s30connected`** — распознавание не должно
  зависеть от СКУД-сервера (это часть архитектурного уродства, см. док 2,
  раздел 11.10 — подсветка и распознавание должны быть в percomedia)
- percomedia (видеопайплайн + распознавание) — **переносится как есть**
  (с заменой rockx → ROCKIVA), это автономная часть
- Бизнес-логика S30 — **требует отдельного решения** (см. док 2, раздел 12)

2. **Конфигурация приходит от S30, не из локального файла:**
   - `onJsonAnsw` (devicesrc.h:122-140) — `d.setJson(jo_settings)` (строка 135)
     заполняет `DeviceData` из ответа S30
   - `DeviceData::setJson(QJsonObject)` (perco.h:565) заполняет:
     `led_ir_level`, `led_white_level`, `licon_light`,
     `severity_face_identify`, `sleep_time`, `scan_time`, `device_mode`,
     `fl15_*` параметры — **все от сервера S30**
   - Локальный `percomedia.json` (percomedia_config.h:17,
     `#define CONFIG_FILE "/home/percomedia.json"`) — только видеонастройки
     (resolution, scan_period_ms, face_rect), не бизнес-логика
   - **Доказано:** бизнес-настройки — от S30, видеонастройки — локально

3. **Бизнес-логика проходов — на S30:**
   - `svideo.h:568` — отправка распознанного лица на S30:
     ```c
     QString("{\"event\":\"face\",\"face\":{\"name\":\"identification\", %1}}")
         .arg(b64face_template);
     ```
   - `sfaceremotecatch.h:183` — то же: `{"event":"face","face":{"name":"identification",...}}`
   - Сервер S30 решает: открыть замок? записать проход? заблокировать?
   - GUI только отображает результат, не принимает решений
   - **Доказано:** без S30 — нет проходов (некуда отправить identification)

4. **Пользователи (accounts) — на S30:**
   - `sadduser.h:644` — отправка на S30:
     ```c
     QString("{\"set\":\"uid\",\"uid\":{\"acct\":\"%1\",\"value\":\"%2\"}}")
     ```
   - `sedituser.h:634` — то же: `{"set":"uid","uid":{"acct":...,"value":...}}`
   - `sadduser.h:600` — запрос от S30: `{"get":"uid","uid":{"user_acct":...}}`
   - Сервер S30 хранит базу аккаунтов, привязку face_id → account
   - percomedia хранит только feature vectors в памяти:
     - `user` struct (userclass.h:29): `id, rect, list (feature), acct, b64png_photo`
     - `UserList` (userclass.h:55) — `QList<user> usrlist` в памяти
     - **Нет save/load на диск** (grep: только `img.save(&buffer,"JPG")` для
       base64 в память, не на диск; `QFile` не используется в userclass.h)
   - **Доказано:** аккаунты — на S30, percomedia — только feature vectors в памяти

5. **IR LED подсветка — на S30** (см. док 2, раздел 11.8):
   - `mainwin.cpp:230` — `{"event":"face","face":{"name":"on_motion"}}` → S30
   - `mainwin.cpp:234` — `{"event":"face","face":{"name":"off_motion"}}` → S30
   - `sscreen.h:87` — `led_ir_level` (0-8) → `send(jo)` → S30
   - **Доказано:** без S30 — нет подсветки (некому включить IR LED)

6. **Шаблоны настроек — на S30:**
   - `write_template` (devicesrc.h:33-66) читает `://fl15_template.json`
     локально (строка 36-38), но **отправляет на S30** для применения:
     ```c
     JsonService::Instance()->onTransmit(
         QString("{\"set\":\"settings\",\"settings\":{\"template\":\"%1\"}}").arg(key));
     ```
   - Через `onSeriesTick` (devicesrc.h:96-108) отправляет каждую команду
     из серии на S30
   - **Доказано:** шаблон применяется на S30, не локально

7. **Замок/реле (exdev) — на S30:**
   - `exdevsrc.h:22` — `{"set":"exdev","exdev":...}` → S30
   - `exdevsrc.h:29` — `{"get":"exdev","exdev":{}}` → запрос от S30
   - `fl15_template.json` — `"server_addr":"127.0.0.1","server_port":"3002"`
     (reader подключается к S30 на порт 3002)
   - **Доказано:** замок управляется через S30, не напрямую из GUI

8. **События (журнал проходов) — на S30:**
   - `eventlistsrc.h:289` — `{"get":"event_count"}` → S30
   - `eventlistsrc.h:487` — `{"get":"event","event":{"done":true,...}}` → S30
   - **Доказано:** журнал хранится на S30, GUI его запрашивает

**Что НЕ подтверждено (гипотезы, требуют доступа к S30):**

- ❓ **IR-cut фильтр управление:** в s30gui нет упоминаний `ircut`/`ir_cut`
  (grep: 0 совпадений). Кто управляет ircut — S30, rkaiq, или никто —
  **неизвестно**. В old percomedia (ispll.cpp) есть `rk_aiq_uapi_sysctl_setCpsLtCfg`
  но она не вызывается. См. док 2, раздел 11.6, пункт 4.

**Что S30 контролирует (доказано):**

| Функция | Где | Доказательство | Без S30 |
|---------|-----|----------------|---------|
| Настройки устройства | S30 → JSON → GUI | devicesrc.h:88,135 | Дефолтные/пустые |
| Бизнес-логика проходов | S30 (решение) | svideo.h:568 | Нет проходов |
| База аккаунтов | S30 (хранение) | sadduser.h:644 | Нет управления |
| Замок/реле (exdev) | S30 → GPIO | exdevsrc.h:22 | Не открывается |
| IR LED подсветка | S30 → GPIO/PWM | mainwin.cpp:230 | Нет подсветки |
| Шаблоны устройства | S30 (применение) | devicesrc.h:55 | Не применяются |
| Журнал событий | S30 (хранение) | eventlistsrc.h:487 | Нет журнала |

**Что s30guiproj делает сам (без S30, доказано):**

| Функция | Где | Работает без S30? |
|---------|-----|---------------------|
| Видеозахват (VI/RGA/VO) | percomedia | ✅ Да |
| Face detection/recognition | percomedia (rockx) | ✅ Да (но без подсветки ночью) |
| Трекинг лиц | percomedia | ✅ Да |
| Motion detection | percomedia | ✅ Да |
| Отображение видео | percomedia → DRM | ✅ Да |
| Локальные видеонастройки | percomedia.json | ✅ Да |
| Дисплей backlight | s30gui (sysfs) | ✅ Да |
| GUI запуск | main.cpp | ✅ Да (не блокируется) |
| UI сцены | s30gui (Qt) | ⚠️ Запускаются, но без данных от S30 |

**Вывод:** s30guiproj — это **тонкий клиент** к серверу PERCo-S30.
GUI **запускается** без S30 (main.cpp не ждёт подключения), видеопайплайн
(percomedia) автономен, но **бизнес-логика, настройки, подсветка, проходы,
замок, журнал — всё на S30**. Без S30 терминал бесполезен как устройство
контроля доступа (хотя видео и распознавание работают).

**Это критично для порта на RV1126B:**
- Если S30 недоступен/не портируется — нужно **заменить** его функции
  (настройки, проходы, аккаунты, подсветка, замок, журнал) на локальные
- percomedia (видеопайплайн + распознавание) — **переносится как есть**
  (с заменой rockx → ROCKIVA), это автономная часть
- Бизнес-логика S30 — **требует отдельного решения** (см. док 2, раздел 12)

### 10.4 Отображение видео

Видео идёт через **DRM overlay plane** (в обход Qt) — VO0 пишет напрямую в
`/dev/dri/card0`. Qt рисует UI на primary plane, видео — на overlay plane
с `u16Zpos = 0`. Прямоугольники лиц рисуются RGA в сам кадр перед отправкой в VO.

---

## 11. Зависимости (percomedia.pro)

```makefile
LIBS += -leasymedia -lrkaiq -lrkisp_api -lrockx -lrknn_api
        -ldrm -lrga -lpthread -lsqlite3 -lasound
        -lrockchip_mpp -lturbojpeg -lm
```

**Сторонние библиотеки:**
- `easymedia` (rkmedia API) — VI/RGA/VO/VENC
- `rkaiq` — ISP (экспозиция, баланс белого, HDR)
- `rockx` — нейросеть (face detect/landmark/recognize/liveness/track)
- `rknn_api` — runtime нейросети
- `rga` (librga + im2d) — 2D ускоритель
- `drm` — видеовыход
- `sqlite3` — база пользователей (в GUI)
- `turbojpeg` — JPEG кодек

### 11.1 Источники заголовков и библиотек для кросс-сборки (ВАЖНО!)

**Проблема:** на плате `librkaiq.so` / `librga.so` собраны из SDK `rv1126_0318`
(проект `facial_gate`, версия rkaiq **v1.0**, 18 марта 2021). Стандартные SDK
от 6 марта 2021 (`rv1126_rv1109_linux_release_20210306`,
`rv1126_rv1109_linux_ai_camera_release_20210306`) содержат **другие** версии
rkaiq (новее/старее) — структуры `Uapi_ExpSwAttr_s`, `Uapi_ExpQueryInfo_s`
и сигнатуры `imfill`/`imsync` **несовместимы** с .so на плате.

**Решение:** заголовки rkaiq и linux-rga взяты из SDK, совпадающего с .so на плате.

**.so библиотеки** — копируются с платы в `K:\cross_sysroot\usr\lib\`:
```
scp old:/usr/lib/librkaiq.so        K:\cross_sysroot\usr\lib\
scp old:/usr/lib/librga.so.2        K:\cross_sysroot\usr\lib\
scp old:/usr/lib/libeasymedia.so.1  K:\cross_sysroot\usr\lib\
scp old:/usr/lib/librockx.so        K:\cross_sysroot\usr\lib\
scp old:/usr/lib/librknn_api.so     K:\cross_sysroot\usr\lib\
scp old:/usr/lib/librkisp_api.so    K:\cross_sysroot\usr\lib\
scp old:/usr/lib/libdrm.so.2        K:\cross_sysroot\usr\lib\
scp old:/usr/lib/libpercomedia.so.1.0.0  K:\cross_sysroot\usr\lib\  # эталон
```

**Заголовки rkaiq** (v1.0, direct API — совместимы с .so на плате):
```
Источник (HTTP):  http://172.17.0.129:8000/rv1126/external/camera_engine_rkaiq/include/
Назначение:       K:\cross_sysroot\usr\include\rkaiq\

Структура:
  uAPI/    — rk_aiq_user_api_*.h (imgproc.h, ae.h, sysctl.h, awb.h, ...)
  common/  — rk_aiq_types.h, rk_aiq_comm.h, rk_aiq.h
  algos/   — rk_aiq_uapi_ae_int_types.h (Uapi_ExpSwAttr_s с stAdvanced,
              Uapi_ExpQueryInfo_s с CurExpInfo/MeanLuma/HdrMeanLuma)
  xcore/base/ — xcam_common.h, xcam_defs.h

Ключевые признаки совместимости:
  - Uapi_ExpSwAttr_s содержит: enable, stAdvanced, stIris, stAntiFlicker
  - Uapi_ExpQueryInfo_s содержит: CurExpInfo (RKAiqAecExpInfo_t), MeanLuma, HdrMeanLuma
  - rk_aiq_user_api_imgproc.h — direct API (не IPC!), тянет rk_aiq_user_api_*.h
```

**Заголовки linux-rga** (im2d с макросами imfill→imfill_t, imsync(void)):
```
Источник (HTTP):  http://172.17.0.129:8000/rv1126/external/linux-rga/
Назначение:       K:\cross_sysroot\usr\include\rga\

Файлы:
  im2d_api/im2d.h → K:\cross_sysroot\usr\include\rga\im2d.h
  include/*.h      → K:\cross_sysroot\usr\include\rga\ (RgaApi.h, RockchipRga.h,
                    rga.h, drmrga.h, RgaUtils.h, RgaMutex.h, RgaSingleton.h,
                    GrallocOps.h, platform_gralloc4.h)

Ключевые признаки совместимости:
  - imfill — макрос (#define imfill(...)) вызывающий imfill_t(buf, rect, color, sync)
  - imsync(void) — БЕЗ аргументов (новая версия: imsync(int release_fence_fd))
  - librga.so на плате экспортирует imfill_t (C-символ), НЕ C++ imfill
```

**Заголовки rockx** (Rock-X SDK v1.1.0):
```
Источник (локально, из распакованного .repo):  M:\rockx_full\sdk\rockx-rk1808-Linux\include\
Назначение:                                     K:\cross_sysroot\usr\include\rockx\

Файлы:
  rockx.h, rockx_type.h
  modules/face.h, object_detection.h, object_track.h, pose.h, carplate.h
  utils/rockx_config_util.h, rockx_image_util.h, rockx_tensor_util.h
```

**Заголовки drm** (xf86drm.h, xf86drmMode.h):
```
Источник:  K:\sdk\external\camera_engine_rkaiq\rkisp_demo\demo\include\xf86drmMode.h
           (xf86drm.h — из libdrm SDK)
Назначение: K:\cross_sysroot\usr\include\drm\
```

**Заголовки rkmedia / easymedia** — уже в проекте, не нужны из SDK:
```
K:\s30guiproj\percomedia\rkmedia\   — rkmedia_api.h, rkmedia_*.h
K:\s30guiproj\percomedia\easymedia\ — rknn_user.h, rknn_utils.h, rga_filter.h
```

**Кросс-компилятор:** `D:\dev\SysGCC\raspberry\bin\arm-linux-gnueabihf-g++`
(arm-linux-gnueabihf-gcc/g++/readelf/strings/nm из SysGCC Raspberry)

**Qt (для линковки):** `K:\sdk\5.9.4\mingw53_32\` (Windows-сборка Qt 5.9.4,
используется qmake для генерации Makefile, .so Qt берутся с платы в sysroot)

**Sysroot:** `K:\cross_sysroot\` — содержит usr/lib (.so с платы) и
usr/include (заголовки из источников выше)

**Версии SDK — что НЕ подходит (несовместимые структуры rkaiq):**
- `rv1126_rv1109_linux_release_20210306` — rkaiq с Bypass/EcmFlickerSelect/NightMode,
  БЕЗ stAdvanced (Uapi_ExpSwAttr_s другого размера → segfault)
- `rv1126_rv1109_linux_ai_camera_release_20210306` — rkaiq uAPI2 (LinAeInfo/HdrAeInfo
  вместо CurExpInfo, imsync(int) вместо imsync(void))
- `RV1126-RV1109_Linux_SDK_v2.1.1_20210816` — rkaiq v1.0x77.2 (2023, слишком новая)
- `RV1126-RV1109_Linux_SDK/` (без версии в имени) — rkaiq v1.0x77.2 (2023)

**Версия SDK которая подходит:**
- `http://172.17.0.129:8000/rv1126/` — `repo` checkout манифеста
  `rv1126_rv1109_linux_release.xml`, синхронизированный **позже** архива
  `rv1126_rv1109_linux_release_20210306.tgz` (тот же манифест, но более свежие
  коммиты — соответствуют .so на плате от 18 марта 2021, проект `facial_gate`).
  **Отдельного архива для этого SDK нет** — это `repo init -m rv1126_rv1109_linux_release.xml` + `repo sync`.
  Идентификация по ревизиям в `.repo/manifest.xml`:
    - camera_engine_rkaiq: `c324f37490b4b34b2e6dd0e7bc7d9a0176fd53d2`
    - linux-rga:           `aff8f9c3352267ab3310faeea92066657f8a4786` (upstream=im2d)
    - rockx:               `3742f4c1888b21318a1dc6d7268779545ddfd549`
  Для воспроизведения: `repo init -u <manifests.git> -m rv1126_rv1109_linux_release.xml` + `repo sync`,
  либо просто скачать нужные подпапки по HTTP с `http://172.17.0.129:8000/rv1126/external/`.

---

## 12. Размеры и форматы (percomedia_config.h)

```c
// Камеры (ДВЕ камеры — обе физически подключены на old)
// Разные сенсоры, одинаковое разрешение 1920x1080
// Проверено на old: i2c-1 0x32+0x37 заняты драйверами, IQ files есть
//
// Соответствие rk_aiq CamId ↔ VI DevId (ispll.cpp:52,77):
//   rk_aiq CamId 0 = IR  = GC2053  (i2c 0x37, модуль YT-RV1109-2-V1, линза 40IR-2MP-F20)
//   rk_aiq CamId 1 = RGB = SC200AI (i2c 0x32, модуль C7234A-400,  линза 30IRC-2MP-F20)
// Обе front-facing, 2MP, F2.0
//
// IQ files на old (/etc/iqfiles/):
//   gc2053_YT-RV1109-2-V1_40IR-2MP-F20.xml
//   sc200ai1_C7234A-400_30IRC-2MP-F20.xml
CAM_RGB_W = 1920;  CAM_RGB_H = 1080;   // RGB = SC200AI (rk_aiq CamId 1, VI DevId 1)
CAM_IR_W  = 1920;  CAM_IR_H  = 1080;   // IR  = GC2053  (rk_aiq CamId 0, VI DevId 0)

// Нейросеть (повёрнутый кадр portrait)
NN_W = 720;  NN_H = 1280;              // = 360*2 × 640*2, после rot270
NN_VECTOR_LEN = 512;                   // feature vector

// Дисплей
VO_IN_W = 800;   VO_IN_H = 1280;
VO_DISP_W = 800; VO_DISP_H = 1280;      // portrait

// JPEG энкодер
ENC_JPEG_W = 1920;  ENC_JPEG_H = 1080;

// FPS
FPS_RGB_IN = 20;  FPS_RGB_OUT = 1;    // RGB JPEG 1 fps
FPS_IR_IN  = 20;  FPS_IR_OUT  = 10;    // IR  JPEG 10 fps
```

### 12.1 Масштабирование координат NN → дисплей (ВАЖНО!)

**Координаты bounding box'ов** хранятся в `user.rect` в системе **NN_W × NN_H = 720×1280**
(приходят из rockx_face_detect на повёрнутом кадре).

**Рисование боксов** (draw_rects) идёт в **VO_DISP_W × VO_DISP_H = 800×1280**.

**В коде НЕТ явного масштабирования** — координаты 720×1280 используются напрямую
в формулах с `VO_DISP_W`:
```c
int x = VO_DISP_W - (u.rect.right+h_gap) - 35;  // 800 - rect.right(≤720) - 35
int y = VO_DISP_H - (u.rect.bottom+v_gap);       // 1280 - rect.bottom(≤1280)
```

Это работает «достаточно хорошо» потому что:
- **По высоте:** NN_H=1280 = VO_DISP_H=1280 — точно совпадает
- **По ширине:** NN_W=720 vs VO_DISP_W=800 — разница 80px (10%), компенсируется
  `h_gap=30` и `-35` в формуле (визуально боксы слегка смещены, но приемлемо)

**Для порта:** при переходе на ROCKIVA (960×540 → поворот → 540×960) координаты
будут в системе 540×960, а дисплей 800×1280 — масштаб **явно нужен**:
- X: 540 → 800 (множитель 1.481)
- Y: 960 → 1280 (множитель 1.333)
Либо подать в ROCKIVA кадр 800×1280 (но это больше, медленнее), либо
масштабировать координаты в callback'е.

### 12.2 Двухкамерность — зафиксирована глубоко в архитектуре

**Две камеры (RGB + IR) — архитектурное решение для liveness:**
- RGB камера (VI0, dev=0) — цветное изображение для распознавания
- IR камера (VI1, dev=1) — инфракрасное для liveness (живой ли человек)

**Где зафиксировано:**

1. **VI каналы (isp_vi.cpp):**
   ```c
   VI0 = {RK_ID_VI, dev=0, chn=0}  // RGB
   VI1 = {RK_ID_VI, dev=1, chn=1}  // IR
   ```

2. **RGA каналы (isp_rga.cpp):**
   - RGA1 ← VI0 (RGB для NN, rot270)
   - RGA2 ← VI1 (IR для NN, rot270)

3. **ImageBuf (imagebuf.h/.cpp):** два буфера `bRGB` и `bIR`,
   `lockPic(ir, rgb)` — синхронный захват пары

4. **Users (userclass.h):**
   - `tracked_face_array_rgb` + `tracked_face_array_ir` — два массива
   - `rgb_obj(id)` / `ir_obj(id)` — доступ к объектам по id
   - `rgb_objprt` / `ir_objprt` — счётчики ссылок

5. **FaceRecogn::check_face (nnclass.cpp):**
   ```c
   rockx_face_quality(LANDMARK_5, &rgb, rgb_obj, &cfg, &Q_rgb);  // качество RGB
   rockx_face_quality(LANDMARK_5, &ir,  ir_obj,  &cfg, &Q_ir);   // качество IR
   bool qual = (Q_rgb.result == PASS) && (Q_ir.result == PASS);   // ОБА должны PASS
   ```

6. **Users::tracklist (userclass.cpp):**
   ```c
   if(tracked_face_array_rgb.count != tracked_face_array_ir.count) return; // рассинхрон
   bool good_score = (rgb_score > score_lim) && (ir_score > score_lim);     // ОБА score
   ```

7. **VENC (isp_enc.cpp):** два JPEG энкодера — **МЁРТВЫЙ КОД** (venc_init не вызывается)
   - ENC0 → `/tmp/rgb.jpeg` (1 fps) — НЕ создаются
   - ENC1 → `/tmp/ir.jpeg` (10 fps) — НЕ создаются

8. **DbgLogger (dbg.h):** `NN_RGB_CB`, `NN_IR_CB` — отдельные callback-метки

9. **Конфиг (percomedia_config.h):**
   ```c
   FPS_IR_IN = 20;  FPS_IR_OUT = 10;
   FPS_RGB_IN = 20; FPS_RGB_OUT = 1;
   ```

**Вывод:** двухкамерность пронизывает **весь код** — VI, RGA, ImageBuf, Users,
FaceRecogn, конфиг. VENC — мёртвый код (не влияет). Убрать IR камеру = переписать ~30% логики.

---

## 13. Потоки данных (итоговая схема)

```
┌─ RGB камера 1920x1080 NV12  ─┐
│                              │
│  VI0 ─┬─ RGA3(rot270,→ARGB) ─ callback(display_packet_callback)
│       │                      │   draw_rects(rect_mb): imfill × 4 линии на user
│       │                      │   imcomposite(video, rect_overlay, video)
│       │                      │   RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA,0,mb) ← ручная отправка!
│       │                      │      → RGA0(→RGB888, rot0) ─ bind ─ VO0 ─ DRM ─ дисплей 800x1280
│       │                      │
│       └─ RGA1(rot90,→BGR888 720x1280) ─ callback(video_packet_callback_rgb)
│                                              ↓
│                                       ImageBuf.RGB(mb):
│                                         track(b, rgb_arr)      ← трекинг на каждый кадр!
│                                         if m_score>0.98: save  ← порог сохранения
│                                         Users::tracklist()     ← обновление списка
│                                              ↓
│  VI1 ─┬─ RGA3 (если display_ch==1!) ─ ... (то же что VI0 выше)
│       │
│       └─ RGA2(rot90,→BGR888 720x1280) ─ callback(video_packet_callback_ir)
│                                              ↓
│                                       ImageBuf.IR(mb):
│                                         track(b, ir_arr)       ← трекинг на каждый кадр!
│                                         if m_score>0.85: save  ← порог ниже RGB
│                                         Users::tracklist()     ← обновление списка
│                                              ↓
│                              ImageBuf.lockPic(ir, rgb) — захват (БЕЗ проверки isSync!)
│                                              ↓
│  FaceRecogn::onTRecognizeTask (по таймеру 100ms, через setScanEnable):
│    1. Users::get_user() → id кандидата
│    2. check_face(rgb, ir, id):
│       rockx_face_detect → score/size фильтр
│       rockx_face_quality (RGB+IR) → quality фильтр
│       rockx_face_landmark(5) → rockx_face_pose → angle фильтр
│    3. nn_face(rgb, id, feature):
│       rockx_image_equalize_hist
│       rockx_face_align(5) → aligned 112x112
│       rockx_face_recognize → 512-float feature
│    4. user_list.set_feature(id, feature)
│    5. (tracklist уже вызван в callback'ах выше)
│                                              ↓
│  callbacks → AIService → Qt signals → MainWin → UI
│                                              ↓
│  JsonService → WebSocket → сервер PERCo-S30
```

**Ключевые отличия от прежней схемы:**
- RGA3→RGA0 — **НЕ bind**, а callback + `SendMediaBuffer` (ручная отправка)
- `track()` и `tracklist()` — в **ImageBuf callbacks**, не только в таймере
- `isSync()` — **отключён** (закомментирован)
- `display_ch` — переключение камеры на дисплей (VI0 или VI1 → RGA3)
- VENC — **мёртвый код** (не вызывается)
- Углы: RGA3=270° (rga_vo_angle), RGA1/RGA2=90° (rga_nn_angle)

---

## 14. Ключевые особенности архитектуры

1. **Две камеры (RGB + IR)** — синхронная обработка пары кадров для liveness
2. **RGA делает всё** — crop+scale+rotate+format за один проход, 4 канала
3. **rockx — ручной pipeline** — каждый шаг детекции/качества/выравнивания/распознавания вызывается отдельно
4. **Трекинг** — rockx_object_track даёт id, по нему связываем RGB+IR+feature
5. **Автомат состояний** — CHECK→NN→OK с retry и recheck
6. **Рисование в кадр** — прямоугольники через RGA imfill+imcomposite, поверх видео
7. **DRM overlay** — видео в обход Qt, напрямую в дисплей
8. **Qt сигналы** — межпоточное общение через QueuedConnection
9. **Конфиг в JSON** — `/home/percomedia.json`, горячая перезагрузка
10. **Watchdog** — DbgLogger считает таймауты callback'ов, шлёт wdt_timeout
