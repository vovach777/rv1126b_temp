# lib/ — библиотеки для линковки (НЕ в git)

Сюда положите `librga.so` (и опционально `librockit.so`) — они нужны для линковки
на хост-машине при кросс-компиляции.

## Откуда взять

### librga.so (arm64) — НЕТ в SDK

В SDK (`external/camera_engine_rkaiq/rkisp_demo/demo/libs/`) есть только **arm32** версия.
Для RV1126B (arm64) нужно:

**Вариант 1 — с платы (быстро):**
```bash
scp root@<board-ip>:/usr/lib/librga.so lib/
```

**Вариант 2 — собрать из исходников (если нет платы):**
```bash
# build.sh сделает это автоматически если BUILD_RGA_FROM_SOURCE=1
BUILD_RGA_FROM_SOURCE=1 SDK_PATH=/path/to/sdk ./build.sh

# или вручную:
cd $SDK_PATH/external/linux-rga
mkdir build && cd build
cmake -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc ..
make -j4
cp librga.so /path/to/rv1126b_temp/lib/
```

### librockit.so — в SDK

```bash
# Из SDK:
cp $SDK_PATH/external/rockit/lib/arm64/rv1126b/linux/librockit.so lib/
# Или с платы:
scp root@<board-ip>:/usr/lib/librockit.so lib/
```

## Почему не в git

Это бинарники для aarch64 — у каждого пользователя свои версии.
В SDK есть `librockit.so`, а `librga.so` (arm64) только на плате или собирается из `external/linux-rga/`.
