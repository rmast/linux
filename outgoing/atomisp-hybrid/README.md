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
cp outgoing/atomisp-hybrid/dist/atomisp-hybrid-0.tar.gz ~/rpmbuild/SOURCES/

# 1) Maak eerst de kmod source RPM
rpmbuild -bs ~/rpmbuild/SPECS/atomisp-hybrid-kmod.spec

# 2) Gebruik die SRPM als input voor de akmod wrapper
cp ~/rpmbuild/SRPMS/atomisp-hybrid-kmod-0-1.fc44.src.rpm ~/rpmbuild/SOURCES/

# 3) Bouw en installeer de akmod noarch package
rpmbuild -ba ~/rpmbuild/SPECS/akmod-atomisp-hybrid.spec
sudo dnf install ~/rpmbuild/RPMS/noarch/akmod-atomisp-hybrid-0-1.fc44.noarch.rpm
```

Belangrijk:
- installeer **niet** handmatig de `.src.rpm`; die wordt door de akmod package onder `/usr/src/akmods` geplaatst
- `akmods` gebruikt specifiek `/usr/src/akmods/atomisp-hybrid-kmod.latest` en rebuild daarna de gelinkte `atomisp-hybrid-kmod-*.src.rpm`

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

Als `modprobe` faalt, check eerst de akmods-buildlog:

```bash
sudo ls -1 /var/cache/akmods/atomisp-hybrid/
sudo tail -n 120 /var/cache/akmods/atomisp-hybrid/*.log
```

Let op:
- de spec is bewust minimaal gehouden als startpunt
- afhankelijk van je lokale akmods macroset kun je `%kernel_module_package`-details verder aanscherpen

## 4. Belangrijke randvoorwaarden

- Installeer `kernel-devel` en `kernel-headers` voor de kernel waarop je wilt laden.
- `akmods` moet `akmodsbuild` kunnen uitvoeren (`command -v akmodsbuild`).
- Op sommige systemen heet het pakket `akmods-build`, op andere zit `akmodsbuild` al in `akmods`.
- De kmod package installeert de modules onder `/lib/modules/<kernel>/updates/atomisp-hybrid/` zodat `mt9m114` en `ipu-bridge` voorrang krijgen op de in-tree Fedora modules.
- Als Fedora kernelconfig `CONFIG_VIDEO_ATOMISP` niet op `m` staat, forceert deze build dat alleen binnen de externe module-build.
- Dit verandert niets aan je wifi-stack; je kunt dus distro-kernel + losse camera-modules combineren.

## 5. Waarom "No akmod packages found"

`akmods` scant **niet** op willekeurige bronmappen onder `/usr/src/akmods`.
De tool zoekt specifiek:

- `/usr/src/akmods/<naam>-kmod.latest`
- en dat moet wijzen naar een echte source RPM: `/usr/src/akmods/<naam>-kmod-<version>-<release>.src.rpm`

Voorbeeld voor deze module:

- `/usr/src/akmods/atomisp-hybrid-kmod.latest`
- `/usr/src/akmods/atomisp-hybrid-kmod-0-1.fc44.src.rpm`

Snelle diagnose:

```bash
rpm -q akmods akmods-build
ls -l /usr/src/akmods
```

Als je alleen een map zoals `/usr/src/akmods/atomisp-hybrid-0` ziet, dan kan `akmods` die niet gebruiken.
