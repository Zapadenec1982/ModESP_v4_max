# Memory: Decisions, Errors, Workarounds

## Decisions

| Decision | Date | WHY |
|---|---|---|
| OTA dual-partition layout (ota_0 + ota_1) | pre-v1.0 | Rollback on failed OTA — critical for field devices without physical access |
| NVS 16K → 32K | 2026-03-15 | AWS IoT certs (3 PEM files ~4KB each) + growing persist key count |
| ETL over STL | pre-v1.0 | Fixed-size containers prevent heap fragmentation in 24/7 industrial operation |
| Svelte 4 (not 5) | pre-v1.0 | Svelte 5 runes not stable at project start; bundle size critical for 960K spiffs |
| i18n lazy-load packs | 2026-03-16 | 4 languages × 674 keys = too large for single bundle; fetch on language switch |
| PCF8574 I2C expanders | pre-v1.0 | KC868-A6 board uses I2C relay/input expanders, not direct GPIO |

## Errors — NEVER delete entries

| Error | Solution | Date |
|---|---|---|
| | | |

## Workarounds

| Problem | Workaround | Remove when |
|---|---|---|
| | | |

> Policy: > 100 non-empty lines → cascade (flush → compaction → summarization)
