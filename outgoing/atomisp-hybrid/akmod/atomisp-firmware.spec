%global fw_dir /lib/firmware/intel/ipu

Name:           atomisp-firmware
Version:        20260607
Release:        1%{?dist}
Summary:        Intel atomisp ISP firmware blobs (2400b0, 2401a0, 2401a0-legacy)
# Intel redistributable firmware – not GPL; see LICENSE.intel-firmware in the source
# Shipped as uncompressed .bin: the kernel firmware loader's XZ fallback does not
# reliably find .bin.xz when the atomisp module is loaded out-of-tree (akmod) at
# boot, even though CONFIG_FW_LOADER_COMPRESS_XZ=y is set. In-tree builds are
# unaffected. Shipping .bin avoids the dependency on load-time XZ decompression.
License:        Redistributable, no modification permitted
URL:            https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
Source0:        shisp_2400b0_v21.bin
Source1:        shisp_2401a0_v21.bin
Source2:        shisp_2401a0_legacy_v21.bin

BuildArch:      noarch

Provides:       atomisp-firmware = %{version}

%description
Firmware blobs for the Intel Atom ISP (atomisp driver).
Required for:
  - BayTrail (Z3xxx) – shisp_2400b0_v21.bin     (e.g. Asus T100TA)
  - CherryTrail (Z8xxx) – shisp_2401a0_v21.bin   (e.g. HP x2 210)
  - CherryTrail legacy – shisp_2401a0_legacy_v21.bin

%install
install -d %{buildroot}%{fw_dir}
install -m 0644 %{SOURCE0} %{buildroot}%{fw_dir}/
install -m 0644 %{SOURCE1} %{buildroot}%{fw_dir}/
install -m 0644 %{SOURCE2} %{buildroot}%{fw_dir}/

%files
%dir %{fw_dir}
%{fw_dir}/shisp_2400b0_v21.bin
%{fw_dir}/shisp_2401a0_v21.bin
%{fw_dir}/shisp_2401a0_legacy_v21.bin

%changelog
* Sun Jun 07 2026 R. Mast <rmast@example.invalid> - 20260607-1
- Initial packaging of atomisp ISP firmware blobs
