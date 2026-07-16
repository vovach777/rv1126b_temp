# Dual Camera Display Patch (RV1106/RV1103B)

## Содержимое

- `dual_camera_patch.diff` — патч для модификации существующих файлов
- `rkadk_dual_disp_test.c` — новый test app (untracked, применять отдельно)
- `apply_patch.sh` — скрипт применения патча

## Изменённые файлы (в патче)

1. `app/rkadk/src/display/rkadk_disp.c` — поддержка нескольких камер (массив handle по CamId)
2. `app/rkadk/inicfg/rv1106_1103/rkadk_defsetting_sensor_1.ini` — vo_chn=1, x=320
3. `app/rkadk/inicfg/rv1106_1103/rkadk_setting_sensor_1.ini` — vo_chn=1, x=320
4. `app/rkadk/examples/CMakeLists.txt` — цель сборки rkadk_dual_disp_test

## Новый файл (вне патча)

5. `app/rkadk/examples/rkadk_dual_disp_test.c` — test app для двух камер 256xN

## Применение

```bash
# 1. Применить патч к существующим файлам
cd /path/to/sdk
git apply dual_camera_patch.diff

# 2. Скопировать новый test app
cp rkadk_dual_disp_test.c app/rkadk/examples/

# 3. Собрать
cd build
cmake .. && make rkadk_dual_disp_test
```

## Запуск

```bash
# По умолчанию: 256px ширина, высота пропорциональна, два окна рядом
rkadk_dual_disp_test -a /etc/iqfiles -p /data/rkadk

# Свои параметры: -W ширина -H высота(0=пропорц) -s offset -g gap
rkadk_dual_disp_test -W 256 -H 0 -g 8 -s 0
```

## Архитектура

```
Cam 0 (цветная) → VI → VPSS → VO(layer 0, chn 0) → окно (0, 0, 256, N)
Cam 1 (серая)   → VI → VPSS → VO(layer 0, chn 1) → окно (264, 0, 256, N)
```

## Важно

- Sensor 1 по умолчанию `used_isp = FALSE`. Если вторая матрица — ISP-сенсор,
  поставь `used_isp = TRUE` в `rkadk_setting_sensor_1.ini` и проверь `device_name`.
- Для чёрно-белой матрицы без IR-фильтра могут понадобиться отдельные IQ-файлы.
