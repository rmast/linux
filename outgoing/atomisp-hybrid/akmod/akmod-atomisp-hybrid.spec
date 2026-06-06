%global kmod_name atomisp-hybrid

Name:           akmod-%{kmod_name}
Version:        0
Release:        1%{?dist}
Summary:        Hybrid atomisp camera modules (atomisp, ipu-bridge, mt9m114)
License:        GPL-2.0-only
URL:            https://example.invalid/%{kmod_name}
Source0:        %{kmod_name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  akmods
BuildRequires:  gcc
BuildRequires:  kmodtool
BuildRequires:  make
BuildRequires:  tar
Requires:       akmods
Requires:       gcc
Requires:       make

%description
akmod package for out-of-tree camera modules based on a patched atomisp tree.
The source tarball is shared with DKMS and is expected to contain:
- top-level Makefile
- dkms.conf
- drivers/staging/media/atomisp subtree
- external/ipu-bridge and external/mt9m114 wrappers

%prep
%setup -q -n %{kmod_name}-%{version}

%build
# akmods builds modules at install/boot time for target kernels.

%install
mkdir -p %{buildroot}%{_usrsrc}/akmods/%{kmod_name}-%{version}
cp -a . %{buildroot}%{_usrsrc}/akmods/%{kmod_name}-%{version}

%files
%license COPYING
%doc VERSION BASE_COMMIT COMMITS.txt
%{_usrsrc}/akmods/%{kmod_name}-%{version}

%changelog
* Sat Jun 06 2026 Maintainer <maintainer@example.invalid> - 0-1
- Initial hybrid akmod scaffold for atomisp patchset
