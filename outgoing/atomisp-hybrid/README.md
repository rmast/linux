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

Gebruik dezelfde tarball samen met [outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec](outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec).

Kernidee:
- plaats de gegenereerde tarball in je rpmbuild SOURCES
- zet de spec in SPECS
- build met `rpmbuild -bs` (SRPM) of `rpmbuild -ba`

Let op:
- de spec is bewust minimaal gehouden als startpunt
- afhankelijk van je lokale akmods macroset kun je `%kernel_module_package`-details verder aanscherpen

## 4. Belangrijke randvoorwaarden

- Installeer `kernel-devel` en `kernel-headers` voor de kernel waarop je wilt laden.
- Als Fedora kernelconfig `CONFIG_VIDEO_ATOMISP` niet op `m` staat, forceert deze build dat alleen binnen de externe module-build.
- Dit verandert niets aan je wifi-stack; je kunt dus distro-kernel + losse camera-modules combineren.
