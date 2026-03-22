# MT9M114 low-light worktree (Asus T100 / AtomISP)

Dit werkpad bevat de lopende low-light verbeteringen voor `drivers/media/i2c/mt9m114.c` en de bijbehorende AtomISP-timing/metadata ondersteuning.

Doel: stabiel gedrag in donkere scènes zonder willekeurige donkere starts, met voorspelbare ON/OFF-transities in strict YUV runtime.

## Huidige status

De codebasis bevat inmiddels twee lagen werk:

1. **Sensor- en timingfundament**
   - VTS/exposure/gain afstemming en praktische guardrails.
   - Stream-lock en runtime-diagnostiek rond start/stop en state-transities.

2. **Strict deep-lowlight runtime control loop**
   - Runtime detectie + enter/exit logica in `mt9m114`.
   - Donker-scène seeding naar lage fps + hoge exposure/gain.
   - Herstelpaden voor zero-stats/stale telemetry.
   - Histogram-gebaseerde suppress/allow beslissingen waar data bruikbaar is.
   - Extra startup-rescue om run-to-run donkere opstarters te verminderen.

## Belangrijkste bestanden

- `drivers/media/i2c/mt9m114.c`
  - Hoofdimplementatie (deep-lowlight state machine, seeding, recovery, logging).

- `drivers/staging/media/atomisp/pci/atomisp_compat_css20.c`
  - AtomISP-compatibiliteitsdeel waar relevante timing/metadata-aanpassingen landen.

- `tools/atomisp/atomisp_frame_duration.c`
- `tools/atomisp/atomisp_stream_frame_duration.c`
  - Hulpprogramma’s voor frame-duration observatie en validatie.

## Validatie-aanpak (praktisch)

Aanbevolen regressietest voor lamp-aan/lamp-uit:

1. Start stream in vaste testopstelling.
2. Laat meerdere korte runs na elkaar lopen (min. 4, bij voorkeur 10+).
3. Verzamel `dmesg` regels rond:
   - `deep-lowlight pending`
   - `transition seed ON`
   - `startup-sample`
   - `startup black rescue`
4. Controleer op:
   - geen verkeerde fallback (`fps=5`) in echte donkere invalid/zero-stats paden;
   - consistente visuele opstart zonder willekeurige donkere runs;
   - geen late blackouts aan het eind van de run.

## Bekende grenzen

- AtomISP en sensor-telemetrie blijven gevoelig voor race/transiënten bij stream start/stop.
- Incidentele glitch-samples (`exp=0`, `gain=0`, `luma=0/center=0`) kunnen voorkomen; de runtime logica probeert die op te vangen.
- Gedrag moet altijd op echte hardware worden bevestigd; compile-success alleen is niet voldoende.

## Documentatieset (canoniek vs historisch)

### Canoniek

- `README.md` (dit bestand): actuele samenvatting en gebruikscontext.
- `CHANGELOG.md`: chronologisch overzicht van wat functioneel is veranderd.

### Historisch / detailreferentie

De onderstaande bestanden bevatten waardevolle achtergrond, maar overlappen deels en weerspiegelen niet altijd de nieuwste runtime-tuning als zelfstandige bron:

- `GEMINI_EXTRA_WARNINGS.md`
- `GEMINI_FEEDBACK_V2_ADDRESSED.md`
- `IMPLEMENTATION_SUMMARY.md`
- `INDEX.txt`
- `MT9M114_LOWLIGHT_ANALYSIS.md`
- `PACKAGE_STATUS.md`
- `PATCH_APPLIED_MANUALLY.md`
- `TECHNICAL_WARNINGS_CRITICAL.md`
- `V2_CHANGES_SUMMARY.md`

Gebruik deze vooral als context/archief, niet als enige waarheid voor de huidige branchstatus.

## Document governance

Voor deze repository geldt:

- **Canonieke documentatie (leidend):**
   - `README.md`
   - `CHANGELOG.md`

- **Historische documentatie (niet leidend):**
   - Alle overige projectnotities/samenvattingen uit eerdere fases.

Bij tegenstrijdigheid of overlap prevaleert altijd de canonieke documentatie.
Nieuwe functionele of procesmatige updates horen daarom eerst in `README.md` en/of `CHANGELOG.md`.

## Build-snippet

Voor een snelle compile-check van de driver:

```bash
make -C /path/to/kernel/tree drivers/media/i2c/mt9m114.o
```

In deze workspace is dat doorgaans:

```bash
make -C /home/rmast/m2 drivers/media/i2c/mt9m114.o
```
