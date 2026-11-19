# EntryNav2 — он-кар сессия, AA-путь (CarPlay-стрелки на кластер), ШИМ v4

Цель: довести CarPlay-стрелки до кластера. Всё observe-first: **CARPLAY-крэш = ResetMgr-ребут = отвязка iPhone**.
v4 построен так, чтобы итерировать БЕЗ ребутов (горячий конфиг + анти-петля), поэтому опасный шаг теперь безопасен.

## Что готово (оффлайн)
- **Кластерная половина** `build/carplay-kombi` — ПРОВЕРЕНА на реальной DKOMBI2 (стрелка зажигается, countdown, чистый стоп).
- **Шим v4** `build/carplay-shim.so` (22760 B, GLIBC_2.4). Три механизма:
  - **Горячий конфиг**: все `CP_*` читаются живьём из `/var/local/cpshim.conf` (правит `cpconf.py`) — **без рестарта CARPLAY**.
  - **Teardown-safe**: воркер не трогает iap вхолостую + stop-флаг в деструкторе → передеплой больше НЕ роняет CARPLAY (это и был баг 4015).
  - **Анти-петля**: subscribe срабатывает только если `CP_SUB_GEN` > записанного в `/var/local/cpshim.gen`. Краш записывает gen ДО вызова → рестарт не повторяет → один краш, не петля-ребут. Чтобы повторить — поднять `CP_SUB_GEN`.
- **Диагноз subscribe-краша**: register сериализует NULL `NmeString` из Cinemo guidance-объекта → SIGSEGV. Респондер не виноват. Первый кандидат фикса — ненулевой ctx (`CP_CTX_BUF=1`).

## Предусловия
- **ДВИГАТЕЛЬ ЗАВЕДЁН** (на аксессуаре HU ребутится ~2 мин).
- HU на ENET `169.254.1.0:22`, iPhone по CarPlay (wlan0 172.16.222.x). Лог: `/var/local/cpshim.log`.

> **v6 (2026-11-17):** деплой теперь **single-shot с авто-откатом** — одна попытка, проверка подъёма CARPLAY, при неуспехе сам возвращает сток. Ребут-петли (как 2026-11-17, два ребута) больше быть не может. Сниффер `CP_IAP_SNIFF` стал **load-time** (задаётся в `CP_ENV` при деплое, НЕ через cpconf). ПЕРЕД любым деплоем убедиться, что `build/carplay-shim.so` пересобран как **v6** (баннер `=== cpshim v6 loaded ... ===`), иначе это крашащий v5.

## Фаза A — рекон сниффером (v6, single-shot, БЕЗ подписки)
Объявляем RGD + включаем пассивный сниффер iAP2 одним деплоем:
```
set CP_ENV="CP_RGD=1" "CP_IAP_SNIFF=1"
python deploy_sub.py
```
Скрипт сам: зальёт v6 → рестартнёт CARPLAY (один короткий блип) → проверит, что поднялся и держится ~12 с → напишет `DEPLOY_OK` (или сам откатит к стоку и напишет `DEPLOY_FAILED`). Ждём в логе `=== cpshim v6 loaded ... SNIFF=1 ===`.

## Фаза B — чтение сниффера: идут ли данные манёвра
```
python diag.py            # хвост /var/local/cpshim.log
```
Ищем строки `IAP rd fd=... msgid=0x52xx`. **0x5201/0x5202 входящие = iPhone шлёт манёвры** → парсим их и форвардим на кластер (крашащая подписка НЕ нужна). Нет 0x52xx → нужен явный сырой 0x5200 (path B). CarPlay при этом не трогается (сниффер только читает).

## Фаза C — ПРОВЕРКА teardown-фикса (ключевое)
Передеплой или стоп/старт CARPLAY, затем убедиться, что в `/var/local/data/ResetMgr/CoreDump/` НЕ появился новый кор. Если чисто — teardown-баг побеждён, можно итерировать свободно.

## Фаза D — безопасная проба subscribe (БЕЗ ребутов благодаря анти-петле)
1. Сначала канал-recon (gen НЕ тратится):
```
python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=1 CP_NO_CALL=1
```
В логе — `channel vt[80] ... in libNmeVfs` (адрес register-метода).
2. Одна боевая попытка с ненулевым ctx:
```
python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=1 CP_CTX_BUF=1
```
Смотрим лог: `StartRouteGuidanceUpdates rc=... (SURVIVED)` → прорыв. Если упал — CARPLAY рестартится ОДИН раз (анти-петля), ребута нет.
3. Повторить с вариантом (новый gen обязателен):
```
python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=2 CP_CTX_BUF=1
```
Каждый краш → снять кор `/var/local/data/ResetMgr/CoreDump/0N/`, скачать на RE-машину, `analyze_core.py` + дизасм libNmeVfs reg-пути (`/tmp/hu/libNmeVfs.so`), найти слот/ctx, откуда берётся component-строка.

## Фаза E — калибровка + форвард (после «SURVIVED» и прихода манёвров)
По `MAN hex`-дампу выставить `CP_OFF_UNIT/CP_OFF_METRES/CP_STR_DIST/CP_STR_ROAD` + `CP_TYPEMAP` (из `docs/maneuver-decode.md`), поднять `carplay-kombi`, включить `CP_FORWARD=1` — всё через `cpconf.py`, без рестартов.

## Реверт
`rm /run/systemd/system/CARPLAY.service.d/zz-shim.conf; systemctl daemon-reload; systemctl stop CARPLAY; kill $(pidof CARPLAY); systemctl start CARPLAY` — или холодный сон HU. Шим и конфиг реверсивны, кодировки не трогаем.

## Если subscribe никак не оживает
Диагноз указывает, что guidance-объекту нужны валидные строки, которые на MGU даёт navi_handler. Варианты по возрастанию усилий: ненулевой ctx (CP_CTX_BUF) → инициализировать конкретный NmeString-слот респондера (найти слот дизасмом libNmeVfs) → в пределе перенести логику MGU navi_handler/RouteGuidanceConverters.
