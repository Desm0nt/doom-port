# DOOM (Switch 1.1 update): гироприцеливание на Android — разбор и что проверить на устройстве

Статический разбор `work/exefs-update/main.raw` (capstone; адреса — `main+offset`) и для имён функций — символы базовой 1.0
(`work/exefs-base/main.symbols.json`: `idNXJoystick::ProcessSixAxisStates`, `idUsercmdGenLocal::JoystickMove`, `idPlayer::ToggleZoomWork`).

## 1. Какие вызовы nn::hid six-axis делает игра

Импортов six-axis ровно три (других — `IsSixAxisSensorAtRest`, fusion, `SetGyroscopeZeroDriftMode` — в бинарнике нет):

| Вызов | Где | Наш HLE (`dm/host/hle_hid.c`) |
|---|---|---|
| `GetSixAxisSensorHandles(out, 2, &id, GetNpadStyleSet(id))` | main+0x1d0f0c4 | есть: FullKey → 1 handle, Handheld → 2 |
| `StartSixAxisSensor(handle)` для каждого | main+0x1d0f0ec | есть (no-op, сенсоры живут в front_android.c) |
| `GetSixAxisSensorState(&st, handle)` раз в кадр | main+0x1d0f020 | есть: `six_fill` |

`SixAxisSensorState` (0x60) у нас совпадает с SDK: игра читает только `angularVelocity` (st+0x1c/0x20/0x24, обороты/с).
`angle`, `direction`, `acceleration`, `attributes`, `samplingNumber` игра не читает.

## 2. Как игра использует гиро (main+0x1d0ef60, только игрок/индекс 0)

1. `joy_sixAxisEnabled` (+0x30 int, default **1**, не в профиле) == 0 → выход.
2. Проверка числа handles: FullKey (bit0) требует ровно 1, JoyDual (bit2) — 2; иначе handles запрашиваются заново.
3. Читает state (для JoyDual — handle[1], правый Joy-Con), обнуляет оси с |w| < `joy_sixAxisNXThreshold` (default 0.0).
4. **`joy_sixAxisAimToggle` == 0 → оси не пишутся.** Default **"0"**; это и есть пункт меню «прицеливание движением»
   (profile getter/setter main+0x10e7d10 / 0x10e7d30, сериализация в профиль main+0x10d8980).
5. yaw/pitch (сырые float, потом `fcvtzs` в int-события осей 0x30/0x31, main+0x1d0f6d8):
   - FullKey: yaw = gz·(−50·32767)·HorScale, pitch = gx·(−50·32767)·VertScale
   - JoyDual: те же x / z с другим знаком констант (50 · −32767)
   - Handheld: yaw = (gy+gz)·(−50·32767), pitch = gx·(−50·32767)
6. Потребитель (main+0x1f2e3f0, `JoystickMove`-аналог): ось/32767 × InvertX/Y × sens × `joy_sixAxisGlobalScale` (30, слайдер меню) →
   поле дельты мыши (+0x6c0) → общий путь мыши (m_yaw / m_pitch). sens = `joy_sixAxisCurrentSensitivity`, если оно < 0 — берётся
   `joy_sixAxisSensitivity` (0.05); **если sens ≤ 0 — гиро пропускается**.
7. `joy_sixAxisCurrentSensitivity` (default −1) переписывает код оружия (main+0xc5e6b0, в 1.0 это `ToggleZoomWork`):
   0 — без оружия (main+0xc5e9dc) и в одном из состояний оружия (main+0xc5e8b8); иначе значение оружия из decl
   (`sixAxisSens` / `ironSightZoom.sixAxisZoomSensitivity`) либо `joy_sixAxisDefaultSensitivityOverride` /
   `joy_sixAxisAltFireSensitivityOverride` (оба default 0.15, они ≥ 0 и поэтому перекрывают decl).

**`joy_sixAxisZoomOnly`** (default 1) и `joy_sixAxisDeadZone` (0.15) в 1.1 **никто не читает**: их GOT-слот используется только при
регистрации. Поэтому `+set joy_sixAxisZoomOnly 0` бесполезен и не добавлялся. «Только при прицеливании» в 1.1 получается
только через пункт 7 (CurrentSensitivity = 0).

## 3. Npad / style / handle

Лог `six-axis handles for npad 0 style 0x1: 1` — FullKey на npad 0 (DM_OPMODE=1), ровно то, что ждёт ветка FullKey. Handle =
`style_bit | id<<8 | idx<<16`, `six_fill` сверяет его со `style_of(id)`. Handheld (npad 0x20, DM_OPMODE=0) тоже покрыт (2 handles,
используется [0]). Расхождений не найдено.

## 4. Оси (landscape)

front_android.c переводит natural-оси в экранные по `Display.getRotation()` (проверено по стандартной таблице для ROTATION_90/270),
hle_hid.c для FullKey отдаёт контроллер «лежит перед игроком»: ctrl.x = phone.x, ctrl.y = −phone.z, ctrl.z = phone.y.
Поворот телефона влево (вокруг экранной оси y) → gz > 0 → событие < 0 → yaw += (влево). Наклон верхнего края к себе
(вокруг x, +) → взгляд вверх. Знаки совпадают с поведением Joy-Con / Pro Controller. Единицы: rad/s / 2π = обороты/с — верно.

**Итог:** HLE-сторона по статике корректна; в игре гиро гейтится `joy_sixAxisAimToggle` и `joy_sixAxisCurrentSensitivity`.
Без запуска нельзя сказать, какой из гейтов закрыт у хозяина — поэтому добавлена диагностика, которая это покажет в логе.

## 5. Что изменено

`dm/host/hle_hid.c` (в блоке `DM_HID_SIXAXIS_HERE`, вызывается из `GetSixAxisSensorState`, игровой поток):
- `six_cvar`: находит idCVar гейтов через GOT 1.1-update (+0x50 = копия имени, сверяется `strcmp`; если не совпало —
  одна строка лога и ничего не трогаем — защита от другого exe).
- `six_gate`: при **любом изменении** логирует строку
  `hid: gyro gate: joy_sixAxisEnabled E, joy_sixAxisAimToggle A, CurrentSensitivity C (<0: Sensitivity S; 0 = off), GlobalScale G, NXThreshold T -> gyro look on|OFF`.
- `DM_GYRO_FORCE=1` (или `=<sens>`, напр. `0.3`): держит Enabled/AimToggle = 1 и заменяет обнулённый CurrentSensitivity на 0.15
  (или заданное). Пишет только value-поля (+0x30/+0x34), которые читает игра; SetFloat оружия их снова перезапишет. По умолчанию выкл.
- `DM_GYRO_LOG=1`: раз в секунду `gyro: N reads, sampling S, attr 1, peak |w| (rev/s) x .. y .. z .., acc ..` — что реально отдали игре.

Компиляция: `cmake --build work/build-android --target dm` — OK (только старый warning в hle_hid.c:333).

## 6. Что проверить на устройстве

1. Обычный запуск, «прицеливание движением» включено в меню, зайти в уровень с оружием, покрутить телефон. В logcat:
   - `hid: six-axis sensor from the phone's gyroscope` и `[input] motion sensors sampling`;
   - `hid: gyro gate: ...` — смотреть значения:
     - `joy_sixAxisAimToggle 0` → меню не сохранило/не применило переключатель (переключить ещё раз, проверить после рестарта);
     - `CurrentSensitivity 0.000` → гейт оружия (п. 2.7); проверить с `DM_GYRO_FORCE=1`;
     - всё `on`, но взгляд не двигается → смотреть шаг 2.
2. С `DM_GYRO_LOG=1`: `peak |w|` должен расти до ~0.2–1 rev/s при поворотах; `sampling` расти ~200/с. Если 0 — проблема в
   сенсорном потоке (rotation/регистрация), если есть, а гейт `on` и камера стоит — значит множители (GlobalScale/слайдер
   чувствительности/m_yaw) слишком малы: попробовать `DM_GYRO_FORCE=1.0` и слайдер чувствительности гиро в меню на максимум.
3. Направления: поворот телефона влево → камера влево; наклон верхнего края к себе → взгляд вверх. Если инвертировано —
   пункт меню «инверсия движения» (joy_sixAxisInvertY) или сообщить, поправим знак в `six_frame`.
4. Прицеливание (ADS) отдельно: гиро должно работать и с зумом, и без. Если только с зумом — это гейт оружия
   (CurrentSensitivity 0 без зума), лечится `DM_GYRO_FORCE`; тогда сделаем это поведением по умолчанию.

## 7. Итерация 2 (тест на устройстве: гейт "on", камера стоит, строк `gyro:` нет)

Статически: физический USB-геймпад npad не меняет (`style_of` зависит только от `dm_opmode`, геймпад идёт в `dm_pad_set`),
handle FullKey npad 0 — тот, что игра и читает. `hle_doom.c` six-axis не форсит (единственный обработчик — `hle_hid.c`).
Почему не было строк `six_trace` — по коду объяснить нельзя (она печатала на первом же чтении); вероятно, строки потерялись
в logcat (тяжёлые DM_FS_STATS / DM_GFX_PERF_SAMPLE / DM_NVN_COUNT, ring buffer/chatty). Надёжнее смотреть `files/.../boot.log`
(копия stderr, `jni_android.c`). Изменения:

- `hle_hid.c`: `six_fill` возвращает причину нулевого state; первые 3 чтения и каждая смена причины логируются **всегда**:
  `hid: gyro state read N: handle .. (style .., npad .. connected ..): filled|style not connected|no gyroscope|no sample yet, sampling .., w (rev/s) .., acc ..`.
  С `DM_GYRO_LOG=1` — раз в секунду `hid: gyro state: R reads/s, sampling .., peak |w| ..`.
- `DM_GYRO_GAIN=<k>` умножает угловую скорость, отдаваемую игре (проверка гипотезы «реакция слишком мала»).
- `front_android.c`: результат `ASensorEventQueue_registerSensor` раньше не проверялся (при отказе — тишина и при этом лог «sampling»);
  теперь rc в логе `[input] motion sensors sampling (gyro rc .., accel rc ..)` и fallback на `enableSensor + setEventRate`.
  `[input] first gyro sample: ...` при первом событии; если за 5 с событий нет — `[input] gyro: 0 events in 5 s` (до 2 раз);
  с `DM_GYRO_LOG=1` — счёт событий каждые 5 с. `ALooper_pollOnce` с таймаутом 2 с, чтобы «тишина» была видна.

Тест: env `DM_GYRO_FORCE=1`, `DM_GYRO_LOG=1` (остальные тяжёлые DM_* убрать), оружие в руках, крутить телефон ~10 с; затем ещё раз
с `DM_GYRO_GAIN=8`. Снять `boot.log`, grep `gyro|six-axis|motion sensors`.
- нет `first gyro sample` / `0 events` → сенсор не отдаёт события (смотреть rc);
- `gyro state ... no sample yet` → события не доходят до hle_hid;
- `style not connected` → handle/npad не тот;
- `filled`, `peak |w|` > 0.2 при поворотах, `reads/s` ≈ FPS, гейт on, а камера стоит даже с GAIN=8 → значит, оси 0x30/0x31 не
  доходят до взгляда в самой игре; следующий шаг — runtime-проба в `main+0x1d0f6d8` (событие оси 0x30) и `main+0x1f2e478`.
