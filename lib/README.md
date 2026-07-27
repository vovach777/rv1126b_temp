# lib/ — библиотеки для линковки (НЕ в git)

Сюда положите `librga.so` и (опционально) `librockit.so` — они нужны для линковки
на хост-машине при кросс-компиляции. Эти файлы **не в SDK** — только на плате.

## Откуда взять

```bash
# librga.so — с платы (нет в SDK)
scp root@<board-ip>:/usr/lib/librga.so lib/

# librockit.so — можно из SDK (external/rockit/lib/arm64/rv1126b/linux/)
# или с платы
cp /path/to/sdk/external/rockit/lib/arm64/rv1126b/linux/librockit.so lib/
# или
scp root@<board-ip>:/usr/lib/librockit.so lib/
```

## Почему не в git

Это бинарники для aarch64 — у каждого пользователя свои версии.
В SDK есть `librockit.so`, а `librga.so` только на плате.
