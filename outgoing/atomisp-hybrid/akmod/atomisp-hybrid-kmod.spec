%global kmod_name atomisp-hybrid
%global kernels %{?kernels:%{kernels}}%{!?kernels:%(uname -r)}

Name:           %{kmod_name}-kmod
Version:        0
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

%build
for kver in %{kernels}; do
  make KDIR=/usr/src/kernels/${kver} all
done

%install
rm -rf %{buildroot}
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
/lib/modules/*/extra/%{kmod_name}/*.ko*

%changelog
* Sat Jun 06 2026 Maintainer <maintainer@example.invalid> - 0-1
- Initial kmod SRPM scaffold for atomisp-hybrid
