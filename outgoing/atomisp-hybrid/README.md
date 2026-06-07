# atomisp-hybrid (DKMS + akmod)

Deze map bevat een hybride constructie waarmee je dezelfde module-bronset kunt gebruiken voor zowel DKMS als akmod op Fedora.

Doel:
- wijzigingen sinds commit `3cb1fb7a56d2` meenemen uit je lokale kernelbranch
- modules bouwen tegen de distro-kernel (bijv. `7.0.1-200.fc44.x86_64`)
- dezelfde source-tarball bruikbaar maken voor DKMS en akmod

De oplossing bouwt deze modules:
- `atomisp`
- `atomisp_gmin_platform`
- `ipu-bridge`
- `mt9m114`

## 1. Sourceboom + tarball genereren

Voer uit vanuit de kernelboom:

```bash
./outgoing/atomisp-hybrid/export-atomisp-hybrid.sh \
  --base 3cb1fb7a56d2 \
  --name atomisp-hybrid \
  --out ./outgoing/atomisp-hybrid/dist
```

Output:
- `outgoing/atomisp-hybrid/dist/atomisp-hybrid-<version>/`
- `outgoing/atomisp-hybrid/dist/atomisp-hybrid-<version>.tar.gz`
- `outgoing/atomisp-hybrid/dist/atomisp-hybrid-<version>.patch`

## 2. DKMS gebruik

```bash
cd outgoing/atomisp-hybrid/dist
ver="$(ls -1d atomisp-hybrid-* | grep -v '\\.tar\\.gz$' | head -n1 | sed 's#/$##')"
sudo cp -a "$ver" "/usr/src/$ver"
sudo dkms add -m atomisp-hybrid -v "${ver#atomisp-hybrid-}"
sudo dkms build -m atomisp-hybrid -v "${ver#atomisp-hybrid-}"
sudo dkms install -m atomisp-hybrid -v "${ver#atomisp-hybrid-}"
```

Controle:

```bash
modinfo atomisp | head
modinfo ipu-bridge | head
modinfo mt9m114 | head
```

## 3. akmod gebruik

Gebruik dezelfde tarball samen met:

- [outgoing/atomisp-hybrid/akmod/atomisp-hybrid-kmod.spec](outgoing/atomisp-hybrid/akmod/atomisp-hybrid-kmod.spec)
- [outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec](outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec)

Kernidee:
- plaats de gegenereerde tarball in je rpmbuild SOURCES
- zet de spec in SPECS
- build met `rpmbuild -bs` (SRPM) of `rpmbuild -ba`

Praktisch stappenpad (Fedora):

```bash
rpmdev-setuptree
cp outgoing/atomisp-hybrid/akmod/atomisp-hybrid-kmod.spec ~/rpmbuild/SPECS/
cp outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec ~/rpmbuild/SPECS/

# Gebruik de versie uit de gegenereerde dist-tarballnaam.
ver="$(basename outgoing/atomisp-hybrid/dist/atomisp-hybrid-*.tar.gz .tar.gz | sed 's/^atomisp-hybrid-//')"
cp "outgoing/atomisp-hybrid/dist/atomisp-hybrid-${ver}.tar.gz" ~/rpmbuild/SOURCES/

# 1) Maak eerst de kmod source RPM
rpmbuild -bs --define "src_version ${ver}" ~/rpmbuild/SPECS/atomisp-hybrid-kmod.spec

# 2) Gebruik die SRPM als input voor de akmod wrapper
cp ~/rpmbuild/SRPMS/atomisp-hybrid-kmod-${ver}-1.fc44.src.rpm ~/rpmbuild/SOURCES/

# Controleer desnoods expliciet dat de ingebedde spec ook echt deze versie draagt.
rpm -qp --qf '%{NAME} %{VERSION}-%{RELEASE}\n' ~/rpmbuild/SRPMS/atomisp-hybrid-kmod-${ver}-1.fc44.src.rpm

# 3) Bouw en installeer de akmod noarch package
rpmbuild -ba --define "src_version ${ver}" ~/rpmbuild/SPECS/akmod-atomisp-hybrid.spec
sudo dnf install ~/rpmbuild/RPMS/noarch/akmod-atomisp-hybrid-${ver}-1.fc44.noarch.rpm
```

Belangrijk:
- installeer **niet** handmatig de `.src.rpm`; die wordt door de akmod package onder `/usr/src/akmods` geplaatst
- `akmods` gebruikt specifiek `/usr/src/akmods/atomisp-hybrid-kmod.latest` en rebuild daarna de gelinkte `atomisp-hybrid-kmod-*.src.rpm`
- meerdere oude `atomisp-hybrid-*.tar.gz` bestanden in `~/rpmbuild/SOURCES/` zijn niet erg; alleen de naam die exact matcht met `Source0` van de spec wordt gebruikt
- de **wrapper** `akmod-atomisp-hybrid.spec` versioneren is niet genoeg; ook de onderliggende `atomisp-hybrid-kmod-*.src.rpm` moet correct zijn
- als de kmod-spec `src_version` naar `0` laat vallen zonder `--define src_version`, dan zoekt `akmodsbuild` tijdens `%prep` naar `atomisp-hybrid-0.tar.gz`

Build voor actieve kernel forceren en laden:

```bash
sudo akmods --force --kernels "$(uname -r)" --akmod atomisp-hybrid
sudo depmod -a "$(uname -r)"
sudo modprobe atomisp
sudo modprobe ipu-bridge
sudo modprobe mt9m114
```

Controle:

```bash
modinfo atomisp | head
lsmod | grep -E 'atomisp|ipu_bridge|mt9m114'
```

Controleer ook dat vervangende modules echt uit `updates/` komen en niet uit de distro-kernel:

```bash
modinfo mt9m114 | head
modinfo ipu_bridge | head
```

Als je daar nog paden ziet onder `/kernel/drivers/...`, dan is de override nog niet actief.
De package installeert de modules onder `extra/atomisp-hybrid/` en gebruikt daarnaast
`depmod.d` overrides voor `atomisp`, `atomisp_gmin_platform`, `mt9m114` en `ipu_bridge`, zodat:

- `akmods` de kmods als "al gebouwd" herkent
- jouw vervangende modules toch voorrang krijgen op de Fedora in-tree modules

Voor `ipu_bridge` staat in de package zowel `ipu_bridge` als `ipu-bridge` als override,
zodat naamnormalisatie tussen module-naam en bestandsnaam geen verkeerde resolver-keuze geeft.

Waarom niet in `updates/`:
- `updates/` heeft inderdaad van nature voorrang in module lookup
- maar `akmods` controleert expliciet op package-state rond `extra/<naam>`
- direct onder `updates/<naam>` installeren leidt daardoor vaak tot onnodige rebuilds bij boot
- met `extra/` + `depmod` overrides krijg je dezelfde functionele voorrang, zonder die akmods-loop

Als `modprobe` faalt, check eerst de akmods-buildlog:

```bash
sudo ls -1 /var/cache/akmods/atomisp-hybrid/
sudo tail -n 120 /var/cache/akmods/atomisp-hybrid/*.log
```

Let op:
- de spec is bewust minimaal gehouden als startpunt
- afhankelijk van je lokale akmods macroset kun je `%kernel_module_package`-details verder aanscherpen

### Specifiek voor `%prep` fout op `atomisp-hybrid-0.tar.gz`

Als je deze fout ziet terwijl `/usr/src/akmods/atomisp-hybrid-kmod.latest` al naar een versie-SRPM wijst,
dan zat de valkuil meestal in de spec-macro fallback (`src_version -> 0`) tijdens `akmodsbuild --rebuild`.

Na installeren van een nieuwere akmod package met gefixte spec, maak oude cache-output weg en forceer opnieuw:

```bash
sudo rm -f /var/cache/akmods/atomisp-hybrid/*.log
sudo rm -f /var/cache/akmods/atomisp-hybrid/*.failed.log
sudo rm -f /var/cache/akmods/atomisp-hybrid/*.rpm
sudo akmods --force --kernels "$(uname -r)" --akmod atomisp-hybrid
```

## 4. Belangrijke randvoorwaarden

- Installeer `kernel-devel` en `kernel-headers` voor de kernel waarop je wilt laden.
- `akmods` moet `akmodsbuild` kunnen uitvoeren (`command -v akmodsbuild`).
- Op sommige systemen heet het pakket `akmods-build`, op andere zit `akmodsbuild` al in `akmods`.
- De kmod package installeert de modules onder `/lib/modules/<kernel>/extra/atomisp-hybrid/`.
- Een bestand onder `/usr/lib/depmod.d/atomisp-hybrid.conf` geeft `mt9m114` en `ipu_bridge` expliciet voorrang uit `extra/atomisp-hybrid`.
- Als Fedora kernelconfig `CONFIG_VIDEO_ATOMISP` niet op `m` staat, forceert deze build dat alleen binnen de externe module-build.
- Dit verandert niets aan je wifi-stack; je kunt dus distro-kernel + losse camera-modules combineren.

## 5. Waarom akmods anders elke boot opnieuw bouwt

`akmods` beschouwt een kmod pas als aanwezig wanneer er een directory bestaat onder:

- `/lib/modules/<kernel>/extra/<naam>/`

De tool controleert hier expliciet op. Als je package alleen onder `updates/<naam>/`
installeert, dan denkt `akmods` bij elke boot dat de kmod nog ontbreekt en rebuildt hij opnieuw.

Daarom gebruikt deze package nu:

- install onder `extra/atomisp-hybrid/`
- plus `depmod.d` overrides voor modules die de in-tree variant moeten vervangen

## 6. Waarom "No akmod packages found"

`akmods` scant **niet** op willekeurige bronmappen onder `/usr/src/akmods`.
De tool zoekt specifiek:

- `/usr/src/akmods/<naam>-kmod.latest`
- en dat moet wijzen naar een echte source RPM: `/usr/src/akmods/<naam>-kmod-<version>-<release>.src.rpm`

Voorbeeld voor deze module:

- `/usr/src/akmods/atomisp-hybrid-kmod.latest`
- `/usr/src/akmods/atomisp-hybrid-kmod-<version>-1.fc44.src.rpm`

## 7. Diagnose als akmods elke boot opnieuw bouwt

Als je nog steeds ~7 minuten rebuild ziet bij reboot, controleer dan direct deze drie punten:

```bash
uname -r
sudo ls -ld /lib/modules/"$(uname -r)"/extra/atomisp-hybrid
rpm -qf /lib/modules/"$(uname -r)"/extra/atomisp-hybrid
sudo tail -n 120 /var/cache/akmods/atomisp-hybrid/*.log
sudo journalctl -b -u akmods --no-pager
```

Interpretatie:

- ontbreekt `/lib/modules/<kver>/extra/atomisp-hybrid`, dan denkt akmods dat er niets geïnstalleerd is
- als de map bestaat maar de log zegt "could not be installed", dan faalt de binaire kmod-installatie en bouwt hij elke boot opnieuw
- als de map bestaat en package-eigendom klopt, hoort akmods niet opnieuw te bouwen tenzij `src_version` omhoog is gegaan

Snelle diagnose:

```bash
rpm -q akmods akmods-build
ls -l /usr/src/akmods
```

Als je alleen een map zoals `/usr/src/akmods/atomisp-hybrid-0` ziet, dan kan `akmods` die niet gebruiken.
