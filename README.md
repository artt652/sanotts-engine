# sanotts-engine

Нативный движок синтеза речи для голосов [sanoTTS](https://github.com/Ampixa/sanoTTS):
консольные программы на C со встроенным [espeak-ng](https://github.com/espeak-ng/espeak-ng)
1.52, без Python, numpy и системного `libespeak-ng`. Нейросеть — тот же C-код, которым голоса
sanoTTS звучат в браузерной демо-версии, только собранный в обычную программу, а не в WebAssembly.

- `sanotts_cli` — текст → WAV: фонемизация, синтез, паузы, выравнивание громкости, высота и тембр;
- `sanotts_tashkeel` — расстановка огласовок в арабском тексте (нужна арабскому голосу).

Готовые программы для Linux, Android и Windows — в `bin/`, исходники и скрипты сборки — в `src/`.
Сборка воспроизводима: из тех же исходников тем же компилятором получаются побайтно те же файлы
(`SHA256SUMS`).

## Содержимое

```
bin/<платформа>/sanotts_cli[.exe]       готовые программы (таблица ниже)
bin/<платформа>/sanotts_tashkeel[.exe]
voices/irina/, voices/denis/             русские голоса (браузерный формат sanoTTS)
SHA256SUMS                               sha256 всех файлов bin/ и voices/
src/
  sanotts_cli.c          обвязка: аргументы, чтение голоса, нарезка, звук, WAV, режимы --g2p/-I/-O
  snt_g2p.c              фонемизатор на espeak-ng (из sanoTTS mcu/ports/wasm/snt_g2p_wasm.c, с правками)
  espeak-fixes.patch     исправление espeak-ng 1.52, накладывается на копию исходников при сборке
  compat.h               подключается ко всем файлам (-include): недостающие в mingw POSIX-функции
  wincompat/endian.h     заголовок <endian.h> для mingw
  tashkeel/
    sanotts_tashkeel.c   огласовки: перенос на C браузерной реализации libtashkeel из sanoTTS
    tashkeel_model.h     раскладка весов модели tashkeel-q16.bin
  build_static.sh        сборка для Linux (musl) и Windows (mingw) — статически
  build_android.sh       сборка для Android — PIE на системных библиотеках bionic
  make_android_sysroot.sh  sysroot для build_android.sh из исходников bionic, без Android NDK
  android_stubs.py       заглушки libc/libm/libdl для линковки и проверка API 24
LICENSE                  GPL-3.0
```

Из репозитория sanoTTS (коммит `18e26b2b365bff41d211e516b0760021451438f1`) сборка берёт
C-рантайм нейросети (`mcu/src/snt_front_f32.c`, `mcu/src/snt_piperlite.c`, `mcu/include`,
`mcu/ports/wasm/snt_voice_wasm.c`, `cp_id_tables_multi.h`) и исходники espeak-ng 1.52
(`mcu/ports/wasm/espeak`) — тот же набор, что у браузерной и ESP32-сборок sanoTTS.

## Платформы

| Каталог `bin/` | Система | Формат |
|---|---|---|
| `x86_64` | Linux x86-64 | ELF, статически, musl |
| `aarch64` | Linux ARM64 (Raspberry Pi OS 64-bit, Armbian…) | ELF, статически, musl |
| `armv7l` | Linux ARM 32-bit (armhf, Raspberry Pi 2+) | ELF, статически, musl |
| `x86` | Linux x86 32-bit | ELF, статически, musl |
| `android-aarch64` | Android 7+ ARM64 | PIE, системные `libc/libm/libdl`, `/system/bin/linker64` |
| `android-armv7l` | Android 7+ ARM 32-bit | PIE, системные `libc/libm/libdl`, `/system/bin/linker` |
| `android-x86_64` | Android 7+ x86-64 (эмуляторы, Chromebook) | PIE, системные `libc/libm/libdl`, `/system/bin/linker64` |
| `windows-x86_64` | Windows 7+ 64-bit; Windows на ARM — в эмуляции x64 | PE32+, статически, KERNEL32 + UCRT |
| `windows-x86` | Windows 7+ 32-bit | PE32, статически, KERNEL32 + UCRT |

Статические Linux-сборки не зависят от дистрибутива и версии glibc. Android-сборки — обычные
Android-программы: их можно запускать и через системный загрузчик
(`/system/bin/linker64 ./sanotts_cli …`), как это делает Termux. На 64-битной Windows лучше
`windows-x86_64`: 32-битная сборка там тоже работает, но примерно на 16% медленнее.
UCRT есть в Windows 10+, в 7/8.1 ставится вместе с VC++ Redistributable 2015+.

## Что нужно для запуска

1. **Данные espeak-ng** — каталог с `phontab`, `phonindex`, `phondata`, `intonations`, `lang/` и
   словарями `<язык>_dict`. Базовые данные и готовые словари 16 языков — в sanoTTS:
   `mcu/ports/wasm/espeak-data-multi/`. Путь задаётся ключом `-d`; без него программа ищет
   `$SANOTTS_ESPEAK_DATA`, затем `<каталог программы>/../share/espeak-ng-data`.
2. **Голос** — каталог в браузерном формате sanoTTS (`web/voices/<имя>/`): `meta.json`,
   `front_f16.bin` (или `front_f32.bin`), `dec_f16.bin` (или `dec_f32.bin`).
3. **Русский словарь** собирается самой программой из исходников espeak-ng 1.52.0 —
   `dictsource/ru_rules`, `ru_list`, `ru_emoji` и `dictsource/extra/ru_listx` (23 МБ, полный
   список словоформ с ударениями) в одном каталоге:
   ```bash
   sanotts_cli -d share/espeak-ng-data --compile-dict ru_dictsource/ ru   # → share/espeak-ng-data/ru_dict
   ```
   Свои поправки ударений можно положить рядом в `ru_extra` (формат espeak: `жуков $2` —
   ударение на 2-й гласный).

## Голоса

Два русских голоса, обученных дистилляцией по рецепту sanoTTS (та же архитектура, что у
`russian` в sanoTTS, но крупнее: 1,84 млн параметров против 1,57 млн), формат — как у голосов в
`web/voices/` sanoTTS:

| Каталог | Голос | Учитель (Piper) | Параметры | Размер |
|---|---|---|---|---|
| `voices/irina/` | Ирина, женский | `ru_RU-irina-medium` | 1 836 364 | 3,7 МБ |
| `voices/denis/` | Денис, мужской | `ru_RU-denis-medium` | 1 836 524 | 3,7 МБ |

22 050 Гц, `espeak_voice` = `ru`, слот 10. Примерно в 3,5–4 раза быстрее реального времени на
x86-64 вместе с запуском программы.

```bash
sanotts_cli -d share/espeak-ng-data -v voices/irina -o out.wav -t "Привет! Я Ирина."
```

## sanotts_cli

```bash
sanotts_cli -d share/espeak-ng-data -v voices/russian -o out.wav -t "Привет, мир!"
echo "Привет, мир!" | sanotts_cli -d share/espeak-ng-data -v voices/russian -o out.wav
sanotts_cli -d share/espeak-ng-data -v voices/russian -o - -t "Привет" | aplay   # WAV в stdout
```

| Ключ | Что делает |
|---|---|
| `-d DIR` | данные espeak-ng |
| `-v DIR` | каталог голоса |
| `-o FILE` | WAV, 16 бит, моно, частота голоса; `-` — в stdout |
| `-t TEXT` | текст; без ключа читается stdin (UTF-8) |
| `-s X` | темп: множитель к `length_scale` голоса (>1 — медленнее) |
| `-p SEC` | пауза между предложениями |
| `-l SEC` | тишина в начале |
| `-L LUFS` | целевая громкость (по умолчанию −16): громкость речи по BS.1770 без пауз → усиление → лимитер с упреждением; `off` — только нормализация пика |
| `-P ST` | высота, полутоны (−12…12), темп не меняется: модель говорит медленнее в 2^(P/12) раза, звук пересэмплируется обратно |
| `-B DB` / `-T DB` | низкие (полка 250 Гц) / высокие (полка 3500 Гц) |
| `-O PREFIX` | потоково: каждая строка входа — свой WAV (`PREFIX000.wav`, …), готовый файл сразу объявляется в stdout строкой `N путь` (`N -` — строке нечего говорить); голос загружается один раз |
| `-I` | строки входа — готовые коды фонем (`1 0 28 0 30 … 2`), по фразе на строку |

Режимы без синтеза:

| Команда | Что делает |
|---|---|
| `--g2p [голос_espeak слот]` | каждая строка stdin → `=N id id …` в stdout; один запуск на много строк. Голос и слот — `espeak_voice` и `g2p_voice_slot` из `meta.json` (русский — `ru 10`) |
| `--ids TEXT [голос_espeak слот]` | коды фонем одного текста (отладка) |
| `--compile-dict DSOURCE/ [язык]` | собрать `<язык>_dict` в каталог данных из исходников espeak-ng |

`--g2p` и `-I` вместе позволяют вынести подготовку текста наружу: внешний фронтенд получает коды
фонем, правит их (ударения, паузы) и отдаёт движку на синтез. Код возврата 0 — успех; ошибки и
сводка (`1.29s at 22050 Hz, 1 chunk(s), loudness …`) — в stderr.

## sanotts_tashkeel

```bash
sanotts_tashkeel -m tashkeel-q16.bin -t "السلام عليكم"        # → السَّلَامُ عَلَيْكُمْ
sanotts_tashkeel -m tashkeel-q16.bin -o out.txt < in.txt
```
Модель — libtashkeel (mush42, MIT), та же, что Piper запускает перед espeak для арабских голосов;
веса `tashkeel-q16.bin` (2,4 МБ) — в sanoTTS: `web/tashkeel/`. Уже расставленные огласовки
модель сохраняет, неарабские символы проходят как есть. ~0,02 с на фразу плюс запуск.

## Сборка

Все скрипты принимают путь к репозиторию sanoTTS на коммите
`18e26b2b365bff41d211e516b0760021451438f1` и кладут `sanotts_tashkeel` рядом с `sanotts_cli`.
Временные пути в бинарник не попадают: пересборка даёт побайтно те же файлы, что в `bin/`
(проверено для `x86_64` и `android-aarch64`).

```bash
git clone https://github.com/Ampixa/sanoTTS && git -C sanoTTS checkout 18e26b2b365bff41d211e516b0760021451438f1
```

### Linux и Windows — `build_static.sh`

Компилятор — zig 0.16 как кросс-компилятор C (`pip install ziglang==0.16.0`), все цели с одной
машины. Скрипту нужна обёртка `CC` с целью:

```bash
mkcc() { printf '#!/bin/sh\nexec python3 -m ziglang cc -target %s "$@"\n' "$1" > "cc-$1"; chmod +x "cc-$1"; }

mkcc x86_64-linux-musl;   CC=$PWD/cc-x86_64-linux-musl   src/build_static.sh sanoTTS bin/x86_64/sanotts_cli
mkcc aarch64-linux-musl;  CC=$PWD/cc-aarch64-linux-musl  src/build_static.sh sanoTTS bin/aarch64/sanotts_cli
mkcc arm-linux-musleabihf; CC=$PWD/cc-arm-linux-musleabihf src/build_static.sh sanoTTS bin/armv7l/sanotts_cli
mkcc x86-linux-musl;      CC=$PWD/cc-x86-linux-musl      src/build_static.sh sanoTTS bin/x86/sanotts_cli
mkcc x86_64-windows-gnu;  CC=$PWD/cc-x86_64-windows-gnu  src/build_static.sh sanoTTS bin/windows-x86_64/sanotts_cli.exe
mkcc x86-windows-gnu;     CC=$PWD/cc-x86-windows-gnu     src/build_static.sh sanoTTS bin/windows-x86/sanotts_cli.exe
```

Подойдёт и обычный gcc/clang (`CC=gcc src/build_static.sh …`) — тогда программа будет связана
с libc системы сборки. Что делает скрипт:
1. копирует исходники espeak-ng из sanoTTS и накладывает `espeak-fixes.patch`;
2. собирает espeak-ng с `-Os` (на скорость синтеза не влияет), нейросеть и обвязку — с `-O3`,
   без `-march=native`, чтобы программа шла на любом процессоре своей архитектуры;
3. линкует статически с `--gc-sections` и `-s`; для Windows (определяется по `_WIN32` у
   компилятора) добавляет `wincompat/` и `-lshell32`.

**Почему musl, а не glibc.** Статический glibc при старте делает системные вызовы (rseq и др.),
которые seccomp-фильтр Android может убить, а Linux-сборку запускают и в Android-окружениях
(chroot, proot). Фонемы совпадают с glibc-сборкой 1:1, звук — до 1 младшего бита.

**Windows.** Недостающие в mingw POSIX-функции — в `compat.h` и `wincompat/`. Текст из `-t`
берётся из UTF-16 командной строки (иначе Windows отдаёт аргументы в кодировке ANSI), stdin и
stdout переводятся в двоичный режим.

### Android — `make_android_sysroot.sh` + `build_android.sh`

Android NDK не нужен: sysroot собирается из исходников bionic и compiler-rt. Нужны clang 18,
ld.lld, llvm-ar, llvm-nm, python3.

```bash
git clone https://github.com/aosp-mirror/platform_bionic bionic && git -C bionic checkout 731631f3
git clone --depth 1 --branch llvmorg-18.1.3 --filter=blob:none --sparse https://github.com/llvm/llvm-project
git -C llvm-project sparse-checkout set compiler-rt/lib/builtins

src/make_android_sysroot.sh bionic llvm-project/compiler-rt/lib/builtins android-sysroot

src/build_android.sh sanoTTS android-sysroot aarch64-linux-android    bin/android-aarch64/sanotts_cli
src/build_android.sh sanoTTS android-sysroot armv7a-linux-androideabi bin/android-armv7l/sanotts_cli
src/build_android.sh sanoTTS android-sysroot x86_64-linux-android     bin/android-x86_64/sanotts_cli
```

`make_android_sysroot.sh` берёт из bionic заголовки libc и kernel uapi, карты символов
`libc/libm/libdl`, собирает `crtbegin_dynamic.o`/`crtend_android.o`, а для arm32 — помощники
компилятора из compiler-rt (`libclang_rt.builtins-arm-android.a`).

`build_android.sh` собирает программу как настоящую Android-программу: PIE, API 24 (Android 7),
сегменты по 16 КБ, интерпретатор `/system/bin/linker64` (`linker` на arm32). Вместо библиотек
NDK `android_stubs.py` по картам символов bionic делает заглушки `libc.so`/`libm.so`/`libdl.so`
ровно под нужные символы — с проверкой, что каждый есть на API 24 для своей архитектуры (иначе
ошибка сборки); на устройстве загрузчик свяжет программу с настоящими системными библиотеками.
Флаг `DF_1_PIE` снимается, как это делает termux-elf-cleaner: загрузчик Android 7–8 его не знает.

**Почему не статическая сборка, как для Linux.** На Android 10+ программы из каталога
приложения запускаются через системный загрузчик, а тот принимает только PIE (ET_DYN):
статический ET_EXEC он отвергает (`has unexpected e_type: 2`).

### Проверка

```bash
sha256sum -c SHA256SUMS
bin/x86_64/sanotts_cli -d share/espeak-ng-data -v voices/russian -o t.wav -t "Проверка."
```

## Что изменено относительно sanoTTS и espeak-ng

`src/snt_g2p.c` — копия `mcu/ports/wasm/snt_g2p_wasm.c` с правками:
- логи в stderr, путь к данным espeak-ng — из `-d` (или `$SANOTTS_ESPEAK_DATA`);
- **исправлена потеря пунктуации внутри предложения**: espeak перед возвратом клаузы
  читает первый символ следующей, и обратный поиск `, ! ?` останавливался на нём;
- пробел на любой границе клауз (например, у тире), как у phonemizer, и инициализация
  без обязательного английского голоса.

Проверено: на 13 фразах фонемы совпадают с эталонным Python-фронтендом sanotts в 12
(расхождение — символ дефиса в словах вроде «ёлки-палки», так же ведёт себя браузерная версия);
на 11 языках с конфигами в sanoTTS 9 совпадают полностью (в немецком эталон раскладывает `ç` на
`c`+седиль, во французском подменяет голос espeak `fr` на `fr-fr`).

`src/espeak-fixes.patch` — если словаря языка нет (например, `en_dict`, а в тексте встретился
`°C`), espeak всё равно искал слова в недозагруженном переводчике и читал неинициализированную
память. В Linux это случайно обходилось (память от ОС приходит обнулённой), в Windows падало.
Теперь при неудачной загрузке словарь остаётся пустым, но корректным. Проверено AddressSanitizer
с заполнением выделяемой памяти мусором.

`src/sanotts_cli.c` — написан заново: чтение голоса (fp16 → fp32), нарезка на предложения,
паузы, выравнивание громкости, высота и тембр, запись WAV, потоковый режим, сборка словаря,
пакетная фонемизация и синтез из готовых кодов фонем.

`src/tashkeel/sanotts_tashkeel.c` — построчный перенос на C браузерной реализации огласовок
sanoTTS (`web/tashkeel/tashkeel.mjs`, без ONNX) с тем же порядком вычислений в double. На 1500
предложениях корпуса Tashkeela (без огласовок, с частью огласовок, с цифрами и латиницей)
результат побайтно совпал с эталоном sanoTTS.

## Лицензия

GPL-3.0 (`LICENSE`): программы включают sanoTTS и espeak-ng. Код `sanotts_cli.c` и
`sanotts_tashkeel.c` написан для этого движка и доступен также под MIT. Модель libtashkeel
(mush42) — MIT.
