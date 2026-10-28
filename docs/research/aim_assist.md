# DOOM (Switch 1.1 update): aim assist на Android — разбор гейтов и что проверить на устройстве

Статика: `work/exefs-update/main.raw` (адреса `main+off`), имена функций — из символов базовой 1.0
(`work/exefs-base/main.symbols.json`); в 1.0 GOT-слоты резолвятся через RELA (`R_AARCH64_RELATIVE`, addend = объект idCVar).
Логика в 1.1 та же (проверено по ключевым местам), смещения полей usercmdGen совпадают.

## 1. Цепочка решения «работает ли aim assist»

`idPlayer::ProcessInput_ServerAndClient_Local` (1.0 0xaac7bc) → `idPlayerAimAssist::Update` (1.0 0xa742d4, 1.1 main+0x2a5d70) — вызывается,
если у игрока есть оружие. Внутри по порядку:

| # | Гейт | Где читается | Default | Комментарий |
|---|---|---|---|---|
| 1 | `g_setting_aim_assist != 0` (через `GetEngine()->[+0x100]`) | 1.1 main+0x2a5de0 | "1" | 2 = только SP (проверка MP-флага), 3 = только MP, 4 = требует in_joystick |
| 2 | **`in_joystick != 0`** | 1.1 main+0x2a5de8 | "1", но **сериализуется в профиль** (`idPlayerProfileShell::InternalSerialize`) | `cmp setting,0; ccmp joy,0` — выход, если любой 0 |
| 3 | controller vfunc +0x18 | main+0x2a5f94 | — | не трогали |
| 4 | профиль `+0x4698` != 0 | main+0x2a6064 | 1 (`ConstructDefaults`) | **это и есть пункт меню «Aim Assist»** (ControllerSettings / MouseKeyboardSettings LoadData/AdjustField) |
| 5 | `aimAssistData.enabled` (+4) у оружия | main+0x2a6090 | decl оружия | `aa_targetForceUseDefaults 1` подставляет встроенные данные с enabled=1 |
| 6 | `usercmdGen->GetUseAimAssist()` (vtable +0x90) | после расчёта, перед `ApplyAimAssist` | — | см. п. 2 |

`aa_targetAdhesionPlayerLookSpeedThreshold` = 0.1 °/s (ничтожно), `aa_targetAdhesionPlayerSpeedThreshold` = 10 — **adhesion
(«прилипание») включается только когда игрок двигается** (> 10 ед/с). Friction (замедление) от движения не зависит.

## 2. Как флаг useAimAssist зависит от устройства ввода

`idUsercmdGenLocal` (1.0 символы, 1.1 те же смещения):
- `GetUseAimAssist` = `ldrb w0,[x0,#0x76c]` (1.1 main+0x1f2ed00), `JoystickInUse` = `+0x76e` (main+0x1f2ed10).
- `HandleJoystickAxis` / конец `JoystickMove` (1.1 main+0x1f2bfac / 0x1f2cfc4): если стик отклонён (после dead zone) →
  `+0x76c = !byte(+0x680)` (+0x680 = 0 в конструкторе, других записей нет).
- `KeyMove` (main+0x1f2bb68): если зажато **действие движения** (forward/back/strafe/up/down в таблице действий +0x5fc) →
  `+0x76c = 0`. Таблицу заполняют и клавиатура, и **кнопки геймпада** (`Joystick()` → `GetUsercmdAction` → `+0x5fc[action] = 1`).
  То есть кнопка, забинженная на `_moveUp` (прыжок?) и т.п., на этот кадр выключает aim assist; следующий кадр со стиком включит.
- Мышь/клавиатура в 1.1 не импортируются (`nn::hid` только npad/six-axis/vibration/touch), так что «нас приняли за мышь» невозможно:
  наш FullKey npad 0 → `idNXJoystick` → стики → JoystickMove → флаг ставится. Тип контроллера/style на гейты не влияет.

## 3. Куда идёт эффект (и почему наши линейные стики не мешают)

- `idAimAssist::ApplyAimAssist` (1.0 0x98ac18) сглаживает выходы (`aimAssist_InterpolateValues`): adhesion-углы (+0x2c..0x34),
  friction-скаляр (+0x38), ещё одна тройка углов (+0x3c..0x44).
- `idPlayer::UpdateViewAngles` (1.0 0xab2834): **если GetUseAimAssist** — к углам взгляда прибавляются adhesion-углы.
- `idPlayer::GetInputSettings` (1.0 0xabc72c): **если GetUseAimAssist** — sensitivity scale *= friction-скаляр (player+0x28ce0).
  Это множитель чувствительности, который usercmdGen применяет к look-стику уже после кривой. Наши `joy_*` (dead zone 0,
  gamma 0, dampenLook 0, deltaPerMSLook 1000) его не обходят: friction режет скорость поворота и при линейном стике,
  и для touch-look (он идёт как right-stick tilt). Gyro идёт через mouse-delta (+0x6c0) — к нему friction, вероятно, не применяется.
- Bullet magnetism (`idHands::FireWeapon`, `idWeapon::GetBulletMagnetismDirection`) тоже смотрит GetUseAimAssist / in_joystick.

**Итог статики:** причина «не через тот input path» не подтвердилась — путь стиков именно тот, что модифицирует aim assist.
Кандидаты, которые видны только в рантайме: `in_joystick = 0` из сохранённого профиля, `g_setting_aim_assist = 0`,
кнопки с действием движения, слабые/выключенные данные aim assist у оружий SP (DOOM SP aim assist на Switch изначально мягкий),
и то, что adhesion требует движения игрока. Поэтому добавлена диагностика + форсирующие переключатели.

## 4. Что изменено (`dm/host/hle_hid.c`, блок перед `GetNpadState(FullKey)`, вызов `aa_gate()` из FullKey/Handheld)

- GOT-слоты 1.1: `g_setting_aim_assist` 0x48d9f38, `in_joystick` 0x4880710, `aa_targetForceUseDefaults` 0x4880750,
  `aa_DESIGN_override` 0x4880758, `aa_DESIGN_*` 0x4880760..0x4880798, пороги 0x4880718/0x4880720, `aa_MagnetismWithoutAimAssist` 0x4880ad0;
  `usercmdGen` — GOT 0x4880728 → указатель на объект. Каждый idCVar сверяется по имени (+0x50), usercmdGen — по vtable
  (+0x90 == main+0x1f2ed00, +0x98 == main+0x1f2ed10). Не совпало → одна строка лога, ничего не трогаем.
- Всегда: при изменении (до 60 строк) лог
  `hid: aim assist gate: g_setting_aim_assist S, in_joystick J, usercmdGen useAimAssist U (+0x680 X), joystickInUse I, aa_targetForceUseDefaults F, aa_DESIGN_override O (friction min/max .., adhesion .., radius ..), thresholds look .. move .. -> on|OFF`.
- `DM_AA_LOG=1`: раз в секунду `useAimAssist N/M polls` (доля опросов, где флаг стоял от самой игры).
- `DM_AIM_ASSIST_FORCE=1`: держит `g_setting_aim_assist`/`in_joystick` ≠ 0 и `useAimAssist = 1` на каждом опросе;
  `=2` — плюс `aa_targetForceUseDefaults 1` (встроенные данные aim assist вместо decl оружия). По умолчанию выкл.
- `DM_AA_SET="name=value,..."`: на каждом опросе пишет value-поля (int +0x30 и float +0x34) перечисленных cvar из таблицы.
  Нужен потому, что `aa_DESIGN_override` при смене значения (флаг modified, в 1.1 бит 18 по +0x68) **копирует значения текущего
  оружия в aa_DESIGN_*** и затирает их — стартовый `+set aa_DESIGN_Friction... X` бесполезен, а DM_AA_SET переживает это.

Компиляция: `cmake --build work/build-android --target dm` — OK (только старый warning hle_hid.c:333).

## 5. Эксперименты для устройства (по порядку)

1. Базовый: `DM_AA_LOG=1`. Зайти в уровень, пошевелить стиком, найти `hid: aim assist gate:`.
   - `in_joystick 0` или `g_setting_aim_assist 0` → причина найдена; шаг 2 это подтвердит.
   - `useAimAssist` низкая доля в `N/M polls` при игре стиком → какая-то кнопка с movement-action его гасит.
   - Всё `on` → гейты открыты; дело в силе эффекта (шаги 3–4).
2. `DM_AIM_ASSIST_FORCE=1` (+ `DM_AA_LOG=1`). Если помогло — сделаем дефолтом.
3. `DM_AIM_ASSIST_FORCE=2` — если у оружий SP aim assist выключен в decl.
4. Посмотреть значения оружия: `DM_DOOM_ARGS="+set aa_DESIGN_override 1"` + `DM_AA_LOG=1`: в строке gate появятся значения
   оружия (friction min/max, adhesion, радиусы). Затем усилить, например:
   `DM_DOOM_ARGS="+set aa_DESIGN_override 1 +set aa_targetAdhesionPlayerSpeedThreshold 0"`
   `DM_AA_SET="aa_DESIGN_FrictionMultiplierMin=0.3,aa_DESIGN_FrictionMultiplierMax=0.6,aa_DESIGN_FrictionMultiplierMinZoom=0.25,aa_DESIGN_FrictionMultiplierMaxZoom=0.5,aa_DESIGN_AdhesionContributionPctMax=0.8,aa_DESIGN_AdhesionContributionPctMaxZoom=0.9"`
   (меньше FrictionMultiplier = сильнее замедление у цели; AdhesionContributionPct 0..1 = доля «доводки»; радиусы — подобрать по
   увиденным значениям оружия, например ×1.5). `aa_targetAdhesionPlayerSpeedThreshold 0` — adhesion без движения (обычный +set работает,
   этот cvar читается напрямую).
5. Если и с FORCE=2 + сильными значениями эффекта нет — значит закрыт гейт 3/4 (controller vfunc / профильный байт «Aim Assist»);
   следующий шаг — runtime-проба в main+0x2a5f94 / main+0x2a6064.

## 6. Итерация 2 (устройство: гейты открыты, `useAimAssist 44/44`, с FORCE=2 и сильными aa_DESIGN эффекта нет)

Статически, где искать дальше:
- Игрок: `*(GOT 0x487f170)` = `idGameLocal*`, `entities[0..11]` по +0x453f8 (1.1 main+0x51c8f0), idPlayerAimAssist = player+0x28ca8
  (1.1 main+0xab48b4). Перед Update вызываются `idAimTargetSelector::Update` (player+0x28e68 в 1.0) и `idARTargeting::Update`.
- Поля idAimAssist (1.0 Update/ApplyAimAssist/UpdateFriction; в 1.1 те же записи main+0x2a610c..0x2a6128):
  +0x08 сырой friction scale (Update ставит 1.0 **только пройдя все гейты**, включая профильный байт «Aim Assist» +0x4698),
  +0x0c..0x14 сырые adhesion-углы, +0x24 = 10 на входе в Update, +0x2c..0x34 сглаженный adhesion (к углам взгляда),
  +0x38 сглаженный friction (множитель чувствительности в GetInputSettings), +0x90 target entity, +0xa8 результат GetTarget
  (vtable +0x18; ≠0 = точка цели выбрана; без неё friction/adhesion не считаются).

Новое в `hle_hid.c` (`aa_probe()`, только при `DM_AA_LOG=1`): раз в секунду
`hid: aim assist probe: N polls: Update ran R, past gates P, target point T, target entity E; friction scale a..b (1 = none), |adhesion| max X deg (raw Y)`.
Сентинелы: на каждом опросе +0x24 := 0 и +0x08 := −1; следующий опрос видит, перезаписал ли их Update (игра опрашивает npad
несколько раз за кадр, поэтому R/P — доля, а не точный счёт кадров; важно, 0 или нет). Указатели проверяются (vtable внутри main).

Как читать:
- нет строки probe / «no local player» → цепочка до игрока в 1.1 другая (сообщить лог);
- `Update ran 0` → Update не вызывается (нет оружия в руках / vfunc +0x380 у игрока ≠ 0);
- `ran > 0, past gates 0` → закрыт controller-гейт или **профильный байт Aim Assist (+0x4698)**: переключить пункт меню off/on;
- `past gates > 0, target point 0` при наведении на демона → цели не выбираются (селектор целей / флаг isAimAssistTarget у AI / трассы);
- `target > 0`, friction < 1 или adhesion > 0 → эффект считается; если всё равно не ощущается — значит, он не доходит до нашего
  look-пути (тогда смотреть GetInputSettings → usercmdGen sensitivity и UpdateViewAngles).

## 7. Итерация 3: куда идут friction (+0x38) и adhesion (+0x2c..0x34) и где они могут теряться

Устройство: Update работает, цели находятся, friction до 0.500, |adhesion| до 1.575°, но эффекта не чувствуется — ни со
стандартной кривой стика, ни с override.

**Friction** (1.1):
1. `idPlayer::GetInputSettings` (main+0xac4db0): `scale = GetSensitivityScale()`, и **если GetUseAimAssist** — `scale *= player[+0x28ce0]`
   (= aimAssist+0x38, main+0xac4e9c); `settings+4 = scale`.
2. Игровой кадр кладёт настройки каждого игрока в кэш idCommonLocal (1.0 `idCommonLocal::Frame` 0x1368adc → common+0x10648;
   в 1.1 кэш по common+0x10ac8).
3. `idCommonLocal::GenerateUserCommands` (1.1 main+0x1ecf480) → `usercmdGen->GetUsercmd(user, settings, …)` (vtable +0x78),
   а она (main+0x1f2e254) копирует settings в usercmdGen+0x650..0x67c. **Две ветки** (main+0x1ecf568..0x1ecf57c):
   - обычная (main+0x1ecf7cc): `settings = кэш` → friction учитывается;
   - **запасная** (main+0x1ecf6cc): если у local user нет player index (`session->vfunc+0x318(user) < 0`) или common+0x10998 == 2 —
     settings собираются заново из cvar (`in_invertLook`, `joy_pitchSpeed`, `joy_yawSpeed`, `m_sensitivity`, `joy_sixAxisSensitivity`),
     **scale = 1.0**: в этой ветке friction теряется.
4. `JoystickMove` (1.1 main+0x1f2cc6c): yaw-множитель = +0x658 × **+0x654**, pitch = +0x65c × +0x654 → интегратор скорости
   (main+0x1f2d020: цель = множитель × стик × edge accel; при `joy_dampenLook 0` цель = скорость сразу) → cmd angles += dt × скорость.
   Наши `joy_*` (dead zone, gamma, dampen, deltaPerMSLook, edge accel, smoothing) этот множитель **не обходят**:
   friction делит итоговую скорость при любой кривой (это и подтвердил тест с DM_DOOM_LINEAR_STICKS=0).
   Гиро идёт в другую ветку (оси 0x30/0x31, свой множитель) — friction к ней не применяется.

**Adhesion**: `idPlayer::UpdateViewAngles` (1.1 main+0xaba72c): если GetUseAimAssist — к углам взгляда прибавляются aimAssist+0x2c..0x34
(и +0x3c..0x44), затем SetViewAngles. От пути ввода не зависит вообще. Это смещение в текущем кадре, а не накапливаемый поворот,
поэтому ~1.5° воспринимается как лёгкий «доворот», а не как прилипание.

Отсюда главная гипотеза: **у нас usercmd строится по запасной ветке** (local user ↔ player index, у нас HLE account/user —
см. `doom_user_by_dev_obs`, DM_INPUT_OBS) → +0x654 всегда 1.0 и friction не попадает в стик. Статически это не проверить;
новая проба показывает это напрямую.

Изменения в `hle_hid.c`:
- `aa_probe` теперь пишет ещё `usercmdGen look sensitivity (+0x654) min..max, yaw x.. pitch x..`. **Если friction < 1, а +0x654
  ровно 1.000..1.000 — работает запасная ветка** (следующий шаг: понять, почему `vfunc+0x318(user)` < 0, или
  common+0x10998 == 2; это уже лечится в HLE user/account). Если +0x654 опускается до ~0.5 вместе с friction, значит friction доходит
  до стика, и дело только в силе эффекта.
- Запасной вариант `DM_AA_OWN=<strength>` (0 = выкл): пока useAimAssist и есть точка цели, правый стик (физический и экранный,
  FullKey/JoyDual/Handheld) умножается на `clamp(lerp(1, friction, strength), 0.1, 1)`. `DM_AA_OWN_ADH=<k>` добавляет adhesion
  как наклон стика: `k × deg / 10` полного отклонения (знак: yaw + → стик x −, pitch + → стик y −; если тянет от цели — k < 0).
  Раз в секунду: `hid: aim assist own: N polls on target, stick scale avg .., game friction ..`.

Тест (каждый — заход в уровень, прицел на демона и рядом 10 с, стрейф):
1. `DM_AA_LOG=1` → строка probe: сравнить `friction scale` и `usercmdGen look sensitivity (+0x654)`.
2. `DM_AA_LOG=1 DM_AA_OWN=1` → friction игры ×1 на стике (замедление до ~0.5 у цели).
3. `DM_AA_LOG=1 DM_AA_OWN=1.6` → сильнее (0.5 → 0.2).
4. `DM_AA_LOG=1 DM_AA_OWN=1 DM_AA_OWN_ADH=1` (а если тянет от цели — `DM_AA_OWN_ADH=-1`; сильнее — 2).

## 8. Итерация 4: откуда «пол» friction 0.5 и почему aa_DESIGN_* не влияли

Устройство: +0x654 следует за friction (0.5 на цели) — обычная ветка, нативный путь работает; эффект просто слабый, и override не менял чисел.

Причина: у игры два набора методов, и по умолчанию включены **«MP»-методы**: `aa_useMPFriction`, `aa_useMPAdhesion`,
`aa_mpAdhesionOffInput` все зарегистрированы с default **"1"** (1.1 main+0x2d2b98 / 0x2d2bd4 / 0x2d2c10, строка "1" main+0x3869d8b).
- `idPlayerAimAssist::UpdateFriction` (1.0 0xa75a70): при `aa_useMPFriction` ≠ 0 считает свой вариант: радиус lerp(+0x84,+0x88)
  × distance modifier, внутренний радиус × +0x8c, множитель — lerp(**+0x9c** в центре, +0x98 на краю) × фактор скорости цели
  (+0xa8/+0xac), friction = 1 − clamp(·,0,1). Поля +0x98/+0x9c — данные оружия из decl (0.5 на оружиях Switch → пол 0.5);
  `GetOverrideAimAssistData` (aa_DESIGN_*) их **не трогает** (пишет только +0x48, +0x68, +0x6c, +0x84, +0x90, +0x94, +0xa0, +0xa4).
- При `aa_useMPFriction 0` — `idAimAssist::UpdateFriction` (1.0 0x98b1e0): R = lerp(+0x84,+0x88), inner = R × +0x8c;
  множитель = **FrictionMultiplierMax (+0x94)** в центре → **Min (+0x90)** на радиусе (Zoom: +0xa4/+0xa0), friction = 1 − clamp(·,0,1).
  То есть **больше Multiplier = сильнее**: прошлый тест «min/max 0.15/0.25» в SP-методе дал бы friction 0.75–0.85 (слабее).
- Adhesion: `idPlayerAimAssist::UpdateAdhesion` (1.0 0xa755d0) при `aa_useMPAdhesion` ≠ 0 идёт в MP-ветку с данными decl; при 0 —
  `idAimAssist::UpdateAdhesion` (1.0 0x98af3c): шаг = min(угловая ошибка до цели, cap decl +0x54/+0x58) × AdhesionContributionPct
  (+0x68, Zoom +0x6c) × distance modifier, радиус +0x48 (AdhesionOptimalRadius). 1.575° — это скорее ошибка/cap decl, а не константа кода.
- Литералы кода (clamp-вектор {1,0,0,0}, FLT_SMALLEST) не виноваты; CUTS-патч не нужен.

Порядок кадра не мешает: значения cvar читаются внутри Update, запись через HLE в любой момент попадает в следующий Update.

Изменения в `hle_hid.c`:
- В таблицу добавлены `aa_useMPAdhesion` (GOT 0x4880730), `aa_mpAdhesionOffInput` (0x4880738), `aa_useMPFriction` (0x4880748);
  в строке gate: `MP friction F adhesion A offInput O` (ожидаем 1 1 1 по умолчанию). Ими можно управлять и через DM_AA_SET.
- `DM_AA_STRENGTH=<s>`: ставит aa_useMPFriction/aa_useMPAdhesion = 0 и aa_DESIGN_override = 1; когда игрок в мире ~120 опросов,
  берёт за базу текущие aa_DESIGN_* (если был `+set aa_DESIGN_override 1` — это SP-значения оружия; нули → 0.35/0.6 friction,
  0.6/0.8 adhesion, радиус 160) и держит их × s (multiplier ≤ 0.95, adhesion ≤ 1, радиусы × √s при s > 1). Лог:
  `hid: aim assist strength: base friction min/max ...`. DM_AA_SET применяется после и перекрывает.

Тест (уровень, демон, 10 с наведения/стрейфа; `DM_AA_LOG=1` всегда):
1. Только `DM_AA_LOG=1` — проверить `MP friction 1 adhesion 1` в строке gate.
2. `DM_DOOM_ARGS="+set aa_DESIGN_override 1" DM_AA_STRENGTH=1` — SP-методы со значениями оружия (или дефолтами); смотреть base в логе
   и `friction scale` в probe.
3. `DM_DOOM_ARGS="+set aa_DESIGN_override 1" DM_AA_STRENGTH=1.5` — сильнее.
4. Явно: `DM_AA_SET="aa_useMPFriction=0,aa_useMPAdhesion=0,aa_DESIGN_override=1,aa_DESIGN_FrictionMultiplierMin=0.4,aa_DESIGN_FrictionMultiplierMax=0.75,aa_DESIGN_FrictionMultiplierMinZoom=0.5,aa_DESIGN_FrictionMultiplierMaxZoom=0.85,aa_DESIGN_AdhesionContributionPctMax=0.8,aa_DESIGN_AdhesionContributionPctMaxZoom=0.9,aa_DESIGN_FrictionOptimalRadius=200,aa_DESIGN_AdhesionOptimalRadius=200"`
   (ожидаем friction ~0.25 в центре цели).
5. Если SP-адгезия хуже MP — оставить MP: `DM_AA_STRENGTH=1.5 DM_AA_SET="aa_useMPAdhesion=1"`.
