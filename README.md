# QDiag Band Lock

Android-приложение для **включения/выключения конкретных LTE/NR5G бэндов** и
**привязки (lock) к конкретной LTE-соте** по PCI + EARFCN на устройствах
с модемом Qualcomm + root.

Работает напрямую через `/dev/diag`, отправляя `QMI_NAS` запросы поверх
DIAG-обёртки — так же, как это делают QXDM / Network Signal Guru /
SnapdragonBandLock. Shizuku здесь не подходит: `/dev/diag` недоступен
shell-пользователю, нужен UID=0.

## Требования

- Android 8.0+ (minSdk 26)
- **Модем Qualcomm** (Snapdragon). MediaTek/Exynos не поддерживаются.
- **Root** (Magisk / KernelSU). Приложение использует
  [libsu](https://github.com/topjohnwu/libsu) для запуска своего рутового
  сервиса в отдельном UID=0 процессе (`:root`).
- SELinux: на большинстве современных прошивок `/dev/diag` читается только
  доменами `diag` / `kernel`. Magisk-правило
  [magisk-diag-se](https://github.com/P1sec/QCSuper/issues/70) либо
  `magiskpolicy --live "allow appdomain diag_device chr_file *"` может
  потребоваться.

## Архитектура

```
┌────────────────────────── UI process ────────────────────────────┐
│  MainActivity + Jetpack Compose  (com.qdiag.bandlock)            │
│            │                                                     │
│            └─ MainViewModel ── RootClient (libsu)                │
│                                    │                             │
│                                    │ bindService (AIDL)          │
│                                    ▼                             │
└──────────────────────── :root process (uid=0) ───────────────────┘
│  DiagRootService (libsu.ipc.RootService)                         │
│        │                                                         │
│        └─ DiagNative (JNI)                                       │
│                │                                                 │
│                └─ libqdiag_native.so                             │
│                      ├─ diag.c   (open /dev/diag, SWITCH_LOGGING)│
│                      ├─ hdlc.c   (CRC-16/X-25 + byte stuffing)   │
│                      ├─ qmi_nas.c (QMI request builders)         │
│                      └─ jni_bridge.c                             │
└──────────────────────────────────────────────────────────────────┘
         │
         ▼
   /dev/diag  ──►  модем (Qualcomm MSM / MDM / SDX)
```

## Возможности

- **Список текущих сот** (LTE + NR5G): PCI, EARFCN/NRARFCN, бэнд, RSRP/RSRQ,
  CellID, TAC, MCC/MNC.
- **Band preference** — чекбоксы для 30+ LTE бэндов и 20+ NR бэндов.
  Отправляется `QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE_REQ` (0x0033) с TLV
  `0x11` (legacy 64-bit), `0x1C` (LTE extended 128-bit), `0x24`/`0x25`
  (NR5G SA / NSA, 128-bit).
- **Cell lock** по PCI + EARFCN — отправляется Qualcomm-специфичный
  NAS-опкод (см. ниже).
- **Reset** — снимает все ограничения.
- **Log** — последние ответы модема в HEX.

## Сборка

```bash
# Нужен JDK 17, Android SDK (platform-34, build-tools 34.0.0), NDK 26.1.10909125
./gradlew :app:assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

Путь к SDK/NDK настраивается в `local.properties`:

```
sdk.dir=/path/to/Android/Sdk
```

## Установка и запуск

1. `adb install app-debug.apk`
2. При первом запуске приложение попросит разрешение на локацию (нужно для
   `TelephonyManager.allCellInfo`) и попытается получить root через libsu —
   в Magisk/KernelSU разрешите.
3. Нажмите **Connect root service**, затем **Open /dev/diag**. Если
   открытие проваливается с `permission denied`, см. раздел SELinux ниже.
4. Отметьте нужные бэнды → **Apply**. Модем перерегистрируется.
5. Для cell lock: введите PCI и EARFCN → **Lock cell**.

## SELinux / доступ к /dev/diag

Даже под root SELinux обычно не даёт приложению открыть `/dev/diag`. На
Magisk достаточно:

```
su
magiskpolicy --live "allow untrusted_app diag_device chr_file { read write open ioctl }"
magiskpolicy --live "allow untrusted_app device dir search"
chmod 666 /dev/diag
```

Для постоянного эффекта положите правило в Magisk-модуль в `sepolicy.rule`.

## Per-baseband caveats

Опкоды **Qualcomm cell-lock** (`QMI_NAS_SET_LTE_PCI_LOCK = 0x4567` и
`QMI_NAS_CLEAR_LTE_PCI_LOCK = 0x4568` в `app/src/main/cpp/qmi_nas.h`)
наблюдались в QXDM-трейсах на SDX5x / SDX65. Для MDM9x07 (старые
Snapdragon 6xx/8xx до 2018 года) и некоторых IoT-модемов опкод отличается
— поправьте константы в `qmi_nas.h` и пересоберите.

TLV-layout самого `SET_SYSTEM_SELECTION_PREFERENCE` стабилен и
совпадает с документацией libqmi.

## Отладка

- Логи нативного слоя: `adb logcat -s qdiag-native:V qdiag-jni:V`
- Логи рутового сервиса libsu: `adb logcat | grep "RootService\|libsu"`
- Дамп ответа модема: кнопка `Open /dev/diag` → нажмите любую команду →
  смотрите карточку **Log**.

## Что НЕ делает это приложение

- Не умеет MediaTek / Exynos (нужен другой транспорт — engineering mode /
  `at+egmr`).
- Не подменяет IMEI, не управляет SIM-apps.
- Не работает без root — это архитектурное ограничение: Android не
  экспортирует Radio HAL в user space.

## Лицензия

MIT.
