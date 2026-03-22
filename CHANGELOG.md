# Changelog

Alle noemenswaardige wijzigingen in deze worktree worden hier samengevat.

## 2026-03-22

### Documentatie geconsolideerd

- README herschreven naar één actuele samenvatting van:
  - scope;
  - huidige implementatiestatus;
  - validatie-aanpak;
  - bekende beperkingen.
- Historische documentatie als referentie gepositioneerd i.p.v. primaire waarheid.

### Historie opgeschoond (vanaf `505ca1b88787`)

Commitreeks is geherstructureerd naar vier inhoudelijke stappen:

1. `media: mt9m114/atomisp bring-up prep and baseline scaffolding`
   - vroege basiswerkzaamheden en voorbereidend bring-upwerk.

2. `media: atomisp: stabilize frame-duration metadata propagation`
   - frame-duration/metadata pad in AtomISP en tooling robuuster gemaakt.

3. `media: mt9m114: fix stream-state locking and add runtime diagnostics`
   - lockpad en stream-state robuustheid verbeterd.
   - diagnostische logging toegevoegd voor reproduceerbare analyse.

4. `media: mt9m114: implement and harden strict deep-lowlight control loop`
   - deep-lowlight runtime implementatie en iteratieve hardening in `mt9m114.c`.
   - inclusief seeding, hysterese, histogramgating, stale/zero-stats herstel en startup-stabilisatie.

## 2026-02 (samengevat uit oudere v2-documenten)

De oorspronkelijke “v2 patch package”-documenten beschrijven deze fundamentele thema’s:

- Dynamic VTS/exposure afstemming voor low-light;
- metering-presets i.p.v. onhandige losse grid-configuratie;
- guardrails rond timing/streaming/AtomISP-interactie;
- uitgebreide test- en deploy-notities.

Belangrijk: die documenten blijven nuttig als achtergrond, maar de actuele branchstatus wordt primair door `README.md` + deze changelog vastgelegd.

## Opmerking over inhoudsconsistentie

Tijdens opschoning zijn commitboodschappen en documentatie samengevoegd om overlap, verouderde claims en tegenstrijdige statusduiding te reduceren.
De code-inhoud is daarbij behouden; de wijziging is vooral structurering en narratief.
