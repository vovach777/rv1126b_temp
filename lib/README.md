# lib/ — библиотеки для линковки (НЕ в git)

Сюда положите `librga.so` (и опционально `librockit.so`) — они нужны для линковки
на хост-машине при кросс-компиляции.

## Откуда взять

### librga.so (arm64) — ЕСТЬ в SDK!

`librga.so` (arm64) нашёлся в SDK в неочевидном месте — внутри `rknpu2`:

```
external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64/
├── librga.so   (196 KB, AARCH64)
└── librga.a    (349 KB, статическая)
```

`build.sh` и `CMakeLists.txt` автоматически находят его там. Можно ничего не класть в `lib/`!

**Альтернативы (если нужно):**
```bash
# С платы (может быть новее):
scp root@<board-ip>:/usr/lib/librga.so lib/

# Собрать из исходников:
BUILD_RGA_FROM_SOURCE=1 SDK_PATH=/path/to/sdk ./build.sh
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
