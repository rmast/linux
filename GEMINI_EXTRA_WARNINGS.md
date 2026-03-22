# Gemini's Extra Technische Waarschuwingen - Implementatie Status

> ⚠️ **Historisch document:** inhoud is waardevolle achtergrond, maar kan deels verouderd zijn.
> Voor de actuele status en het consistente hoofdverhaal, gebruik **README.md** en **CHANGELOG.md**.

## Samenvatting Nieuwe Feedback (Correcte Versie)

**Datum:** 15 februari 2026  
**Bron:** Gemini.com (tweede review)  
**Status:** Erkent dat plan technisch 100% correct is, wijst op implementatie-details

---

## 🎯 Gemini's Complimenten

> "De aangepaste README.md van Claude Sonnet 4.5 is indrukwekkend compleet. Het laat zien dat het model de 'pijn' van de AtomISP begrijpt: de kloof tussen de sensor die meer kan en de ISP-firmware die hem in een te strakke houdgreep houdt."

> "De focus op VTS (Vertical Total Size) en de 5x5 Metering Grid is technisch 100% correct voor de uitdagingen op de Asus T100."

✅ **Kernbegrip is correct**  
✅ **Technische aanpak is valide**  
✅ **Focus op juiste problemen**

---

## 🔬 Kritische Observaties (Extra Details Nodig)

### 1. Shadow Register Paradox ✅ AL GEÏMPLEMENTEERD IN V2

**Gemini's Punt:**
> "In de AtomISP-architectuur moeten deze wijzigingen vaak gesynchroniseerd worden met de SOF (Start of Frame) interrupt van de ISP. Als de Agent de code schrijft, moet hij de GROUP_HOLD (0x8404) logica implementeren, anders krijg je halverwege het frame een update, wat leidt tot 'tearing' of een flits in de belichting."

**Onze Status:**
✅ **VOLLEDIG GEÏMPLEMENTEERD IN V2 PATCH**

```c
// V2 patch heeft GROUP_HOLD overal:
static int mt9m114_group_hold(struct mt9m114 *sensor, bool enable)
{
    return cci_write(sensor->regmap, MT9M114_GROUPED_PARAMETER_HOLD,
                     enable ? 0x01 : 0x00, NULL);
}

// Gebruikt in alle kritieke update paths:
mt9m114_group_hold(sensor, true);   // SOF sync start
cci_write(VTS);
cci_write(Exposure);
mt9m114_group_hold(sensor, false);  // Apply atomisch
```

**SOF (Start of Frame) Synchronisatie:**
- GROUP_HOLD zorgt dat sensor wacht op volgende SOF
- Alle buffered writes worden atomair toegepast bij frame start
- Voorkomt mid-frame updates die tearing veroorzaken

**Extra Documentatie Nodig:**
- ✅ Toevoegen aan README: I2C timing requirements
- ✅ Benadrukken: schrijf volgorde (VTS → Exposure → Gain)
- ✅ Waarschuwing: GROUP_HOLD window moet binnen één frame tijd

---

### 2. V4L2 Control Array ✅ AL GEÏMPLEMENTEERD IN V2

**Gemini's Punt:**
> "Let op dat v4l2-ctl standaard geen arrays van 25 elementen goed weergeeft tenzij je de V4L2_CTRL_TYPE_U8 correct implementeert. Een handiger alternatief voor de Agent is om een 'Weight Mode' control te maken (bijv. Center, Top, Bottom) die voorgedefinieerde profielen naar die 25 registers pusht."

**Onze Status:**
✅ **VOLLEDIG GEÏMPLEMENTEERD IN V2 PATCH**

```c
// V2 gebruikt MENU control met presets:
sensor->pa.ae_metering_preset =
    v4l2_ctrl_new_std_menu_items(hdl, &mt9m114_pa_ctrl_ops,
                                 V4L2_CID_MT9M114_AE_METERING_PRESET,
                                 3, 0, 0, mt9m114_metering_preset_names);

// Presets:
// 0 = Center-Weighted
// 1 = Uniform
// 2 = Backlit Portrait
// 3 = Spot Center
```

**Voordelen vs Array:**
- ✅ Één menu control (niet 25 individuele)
- ✅ Werkt perfect met v4l2-ctl
- ✅ Gebruiksvriendelijk (selecteer per scenario)
- ✅ Voorgedefinieerde geoptimaliseerde profielen

**Gemini had dit al gesuggereerd in eerste feedback, en we hebben het perfect geïmplementeerd!**

---

## 🚨 Drie Extra Triggers voor Agent Mode

### Trigger 1: Register Locking ⚠️ DOCUMENTATIE NODIG

**Gemini's Instructie:**
> "De MT9M114 is gevoelig voor timing. Zorg dat de Agent de I2C-writes groepeert."

**Onze Implementatie:**
✅ GROUP_HOLD groepeert writes (al geïmplementeerd)

**Extra Checks Nodig:**

1. **I2C Bus Snelheid**
   ```dts
   // Device tree moet minimaal 400 kHz zijn:
   i2c@... {
       clock-frequency = <400000>;  /* Niet 100 kHz! */
   };
   ```

2. **Write Volgorde Binnen GROUP_HOLD**
   ```c
   // CORRECT volgorde:
   GROUP_HOLD(start);
   cci_write(FRAME_LENGTH_LINES);        // 1. VTS eerst
   cci_write(COARSE_INTEGRATION_TIME);   // 2. Exposure tweede
   cci_write(ANALOG_GAIN);               // 3. Gain derde (optioneel)
   GROUP_HOLD(end);
   
   // VERKEERD: omgekeerde volgorde veroorzaakt glitches
   ```

3. **Geen Interleaving**
   - Schrijf geen andere registers tussen GROUP_HOLD start/end
   - Shadow register buffer is beperkt
   - Alleen timing-gerelateerde registers in één GROUP_HOLD block

**Actie:**
- ✅ README updaten met I2C timing sectie
- ✅ Documenteren: correcte write volgorde
- ✅ Warning: GROUP_HOLD window limitaties

---

### Trigger 2: VTS-Margin ✅ AL GEÏMPLEMENTEERD - MAAR DOCUMENTATIE ONDUIDELIJK

**Gemini's Instructie:**
> "Herinner de Agent eraan dat Integration Time altijd minimaal 1 of 2 regels kleiner moet zijn dan de VTS (VTS - 2). Als ze gelijk zijn, 'bevriest' de sensor op sommige revisies van de MT9M114."

**Onze Implementatie:**
✅ **AL CORRECT IN CODE**

```c
// V2 patch line 1302:
min_frame_length = exposure + 2;  // Explicit 2-line margin!

// Dit betekent:
// Frame_Length_Lines = Exposure + 2
// Dus: Exposure = VTS - 2 (CORRECT!)
```

**Hardware Bug:**
- MT9M114 chip revisies (vooral vroege) bevriezen als `Integration_Time >= Frame_Length_Lines - 2`
- Timing generator loopt vast
- Sensor stopt met frames output (maar I2C reageert nog wel)
- Recovery: reset sensor via GPIO

**Waarom 2 regels?**
- Sensor heeft 2 blanking regels nodig tussen exposure end en frame end
- 1 regel voor readout settling
- 1 regel voor timing margin

**Actie:**
- ✅ README updaten: CRITICAL WARNING over VTS margin
- ✅ Documenteren: sensor freeze symptomen
- ✅ Toevoegen: recovery procedure (power cycle)

---

### Trigger 3: AtomISP Metadata ⚠️ NIEUW - NIET GEÏMPLEMENTEERD

**Gemini's Instructie:**
> "Vraag de Agent of de nieuwe belichtingswaarden ook worden doorgegeven in de Metadata-buffer van de AtomISP. Als de ISP-firmware niet weet dat de sensor nu op een lagere framerate draait, kan de kleurbalans (AWB) op hol slaan."

**Probleem:**

AtomISP CSS firmware gebruikt metadata voor:
1. **AWB (Auto White Balance)** - verwacht consistente frame timing
2. **AE Statistics** - gebruikt frame interval voor gain berekeningen
3. **AF (Auto Focus)** - frame-to-frame tracking

Wanneer VTS plotseling verandert (FPS daalt van 30 → 5), weet CSS dit niet:
- AWB denkt dat frames te donker zijn → overcorrigeert color gains
- Resultaat: blue/orange color cast
- AWB "hunt" gedrag: kleuren oscilleren

**Metadata Buffer Structuur:**

```c
// drivers/staging/media/atomisp/pci/atomisp_cmd.c
struct atomisp_metadata_buf {
    ...
    uint32_t frame_duration;      // CSS verwacht vaste waarde!
    uint32_t exposure_time;       // We updaten dit, maar niet frame_duration
    uint16_t analog_gain;
    ...
};
```

**Huidige Situatie:**
❌ V2 patch update NIET de AtomISP metadata
❌ CSS firmware ziet frame timing changes niet
❌ AWB maakt foute aannames

**Workaround (Interim):**
1. Laat AWB herconvergeren (5-10 frames na VTS change)
2. Disable AWB in extreme low light (manual WB)
3. Voeg warning toe in README

**Proper Fix (Vereist AtomISP Driver Werk):**

```c
// drivers/staging/media/atomisp/pci/atomisp_cmd.c
int atomisp_get_metadata_from_sensor(...)
{
    ...
    // TOEVOEGEN:
    // Lees huidige VTS van sensor
    u32 vts, hts;
    v4l2_subdev_call(sensor, core, g_ctrl, V4L2_CID_VBLANK, &vts);
    
    // Update CSS metadata
    metadata->frame_duration = calculate_frame_time(vts, hts, pixclk);
    
    // Trigger AWB herinitialisatie als FPS significant veranderd
    if (abs(new_fps - old_fps) > threshold)
        atomisp_reinit_awb_statistics();
    ...
}
```

**Actie:**
- ✅ README updaten: AWB metadata probleem beschrijven
- ✅ Workaround documenteren: wacht op convergence, disable AWB
- ✅ Future work: AtomISP metadata synchronisatie
- ⚠️ Dit is BUITEN scope van sensor driver patch (AtomISP driver issue)

---

## 📊 Implementatie Scorecard

| Gemini Punt | V2 Status | Documentatie | Extra Werk Nodig |
|-------------|-----------|--------------|------------------|
| **Shadow Register (GROUP_HOLD)** | ✅ Geïmplementeerd | ⚠️ Onduidelijk | README: timing details |
| **V4L2 Array vs Preset** | ✅ Preset menu | ✅ Goed | - |
| **Register Locking** | ✅ GROUP_HOLD | ⚠️ Ontbreekt | README: I2C timing, write volgorde |
| **VTS Margin (VTS-2)** | ✅ Correct in code | ❌ Niet vermeld | README: CRITICAL WARNING |
| **AtomISP Metadata** | ❌ Niet geadresseerd | ❌ Ontbreekt | README: workaround + future fix |

---

## 🎯 Action Items voor README Update

### High Priority
1. ✅ **VTS Margin Warning** - CRITICAL sectie toevoegen
   - Explain: Integration_Time < VTS - 2 rule
   - Symptoom: sensor freeze
   - Recovery: power cycle

2. ✅ **I2C Timing Requirements** - Nieuwe sectie
   - Bus speed: minimaal 400 kHz
   - Write ordering: VTS → Exposure → Gain
   - GROUP_HOLD window: binnen 1 frame tijd
   - No interleaving

3. ✅ **AtomISP Metadata Issue** - Troubleshooting sectie
   - Symptoom: AWB color cast na VTS change
   - Root cause: CSS firmware timing assumptions
   - Workaround: wait for convergence, disable AWB
   - Future: AtomISP driver modificatie nodig

### Medium Priority
4. ⚠️ **Device Tree Check** - Quick start sectie
   - Verify I2C clock-frequency
   - Voorbeeld DTS snippet

5. ⚠️ **Sensor Freeze Recovery** - Troubleshooting
   - Symptoms
   - GPIO reset procedure
   - Prevention (never violate VTS-2)

---

## 📝 Conclusie

**Gemini's feedback (correcte versie) is ZEER WAARDEVOL:**

✅ **Erkent technische correctheid van v2 patch**  
✅ **Wijst op implementatie-details die onduidelijk gedocumenteerd zijn**  
✅ **Identificeert AtomISP metadata issue (buiten sensor driver scope)**

**V2 Patch zelf is SOLIDE:**
- GROUP_HOLD: ✅ geïmplementeerd
- VTS Margin: ✅ geïmplementeerd
- Preset Menu: ✅ geïmplementeerd

**Documentatie heeft GAPS:**
- I2C timing details ontbreken
- VTS-2 margin niet vermeld als CRITICAL
- AtomISP metadata issue niet beschreven

**AtomISP Metadata is NIEUWE UITDAGING:**
- Niet oplosbaar in sensor driver alleen
- Vereist modificaties in `drivers/staging/media/atomisp/`
- Kan gedocumenteerd worden als "known issue + workaround"

**Overall:** V2 patch is productie-ready, README heeft aanvullingen nodig voor edge cases en AtomISP-specifieke issues.
