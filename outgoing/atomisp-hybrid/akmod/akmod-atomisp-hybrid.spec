%global kmod_name atomisp-hybrid
%global src_version %{?src_version}%{!?src_version:0}

Name:           akmod-%{kmod_name}
Version:        %{src_version}
Release:        1%{?dist}
Summary:        Hybrid atomisp camera modules (atomisp, ipu-bridge, mt9m114)
License:        GPL-2.0-only
URL:            https://example.invalid/%{kmod_name}
Source0:        %{kmod_name}-kmod-%{version}-%{release}.src.rpm

BuildArch:      noarch
Requires:       akmods
Requires:       kmodtool
Recommends:     atomisp-firmware

%description
akmod package that installs a kmod source RPM payload into /usr/src/akmods.
akmods scans /usr/src/akmods/*-kmod.latest and rebuilds the linked source RPM
for the running kernel.

%prep
:

%build
:

%install
mkdir -p %{buildroot}%{_usrsrc}/akmods
install -m 0644 %{SOURCE0} %{buildroot}%{_usrsrc}/akmods/%{kmod_name}-kmod-%{version}-%{release}.src.rpm
ln -s %{kmod_name}-kmod-%{version}-%{release}.src.rpm %{buildroot}%{_usrsrc}/akmods/%{kmod_name}-kmod.latest

%files
%{_usrsrc}/akmods/%{kmod_name}-kmod-%{version}-%{release}.src.rpm
%{_usrsrc}/akmods/%{kmod_name}-kmod.latest

%changelog
* Fri Jun 21 2026 Maintainer <maintainer@example.invalid> - 1-1
- Rebuild wrapper with updated kmod SRPM

* Sat Jun 06 2026 Maintainer <maintainer@example.invalid> - 0-1
- Install kmod SRPM payload and -kmod.latest symlink for akmods
