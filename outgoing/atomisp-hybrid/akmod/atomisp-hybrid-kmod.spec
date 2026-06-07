%global kmod_name atomisp-hybrid
%global src_version %{?src_version}%{!?src_version:0}
%global kernels %{?kernels:%{kernels}}%{!?kernels:%(uname -r)}
%global debug_package %{nil}
%global _debugsource_packages 0
%global _debuginfo_subpackages 0
%global _enable_debug_packages 0
%global _source_date_epoch_from_changelog 0

Name:           %{kmod_name}-kmod
Version:        %{src_version}
Release:        1%{?dist}
Summary:        Out-of-tree camera kmods (atomisp, ipu-bridge, mt9m114)
License:        GPL-2.0-only
URL:            https://example.invalid/%{kmod_name}
Source0:        %{kmod_name}-%{version}.tar.gz

BuildRequires:  elfutils-libelf-devel
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  kernel-devel
Requires(post): /usr/sbin/depmod
Requires(postun): /usr/sbin/depmod

ExclusiveArch:  x86_64

%description
Kernel module package for out-of-tree camera modules based on a patched atomisp tree.
This SRPM is intended to be rebuilt by akmodsbuild for target kernel release(s).

%prep
%setup -q -n %{kmod_name}-%{version}

# Make out-of-tree atomisp builds robust when rebuilding via akmodsbuild.
# Some source tarballs may still contain the in-tree-only Makefile variant.
ATOMISP_MK=drivers/staging/media/atomisp/Makefile
if grep -q '^atomisp = \$(srctree)/drivers/staging/media/atomisp/$' "$ATOMISP_MK"; then
  sed -i \
    -e 's|^atomisp = \$(srctree)/drivers/staging/media/atomisp/$|ifdef M\natomisp = $(M)/\nelse\natomisp = $(srctree)/drivers/staging/media/atomisp/\nendif|' \
    "$ATOMISP_MK"
fi
if grep -q '^ccflags-y += \$(INCLUDES) \$(DEFINES) -fno-common$' "$ATOMISP_MK"; then
  sed -i \
    -e 's|^ccflags-y += \$(INCLUDES) \$(DEFINES) -fno-common$|ccflags-y += -I$(M)/pci -I$(M)/include/linux $(INCLUDES) $(DEFINES) -fno-common|' \
    "$ATOMISP_MK"
fi

%build
for kver in %{kernels}; do
  make KDIR=/usr/src/kernels/${kver} all
done

%install
rm -rf %{buildroot}
install -d %{buildroot}/usr/lib/depmod.d
cat > %{buildroot}/usr/lib/depmod.d/atomisp-hybrid.conf <<'EOF'
override mt9m114 * extra/atomisp-hybrid
override ipu_bridge * extra/atomisp-hybrid
EOF
for kver in %{kernels}; do
  install -d %{buildroot}/lib/modules/${kver}/extra/%{kmod_name}
  install -m 0644 drivers/staging/media/atomisp/atomisp.ko %{buildroot}/lib/modules/${kver}/extra/%{kmod_name}/
  install -m 0644 drivers/staging/media/atomisp/pci/atomisp_gmin_platform.ko %{buildroot}/lib/modules/${kver}/extra/%{kmod_name}/
  install -m 0644 external/ipu-bridge/ipu-bridge.ko %{buildroot}/lib/modules/${kver}/extra/%{kmod_name}/
  install -m 0644 external/mt9m114/mt9m114.ko %{buildroot}/lib/modules/${kver}/extra/%{kmod_name}/
done

%post
for kver in %{kernels}; do
  /usr/sbin/depmod -a ${kver} >/dev/null 2>&1 || :
done

%postun
for kver in %{kernels}; do
  /usr/sbin/depmod -a ${kver} >/dev/null 2>&1 || :
done

%files
/usr/lib/depmod.d/atomisp-hybrid.conf
/lib/modules/*/extra/%{kmod_name}/*.ko*

%changelog
* Sat Jun 06 2026 R. Mast <rmast@example.invalid> - 0-1
- Initial kmod SRPM scaffold for atomisp-hybrid
