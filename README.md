# MAS-Project

## Environment Setup

### Hardware Specification:

- Vendor: PhoenixNAP Bare Metal Cloud ds.m4.xlarge
- Hardware: Dual 4th Xeon Gold 5418Y (With 1 DSA accelerator per CPU)
- Stock OS: Ubuntu Jammy 22.04

### Software Setup Procedure:

1. Upgrade Ubuntu to 22.10 for newer kernel version:

   ```sh
   sudo apt-get update
   sudo apt-get upgrade
   sudo sed 's/Prompt=lts/Prompt=normal/g' -i /etc/update-manager/release-upgrades
   yes | sudo do-release-upgrade
   ```

   The OS will reboot after upgrading the OS.

2. Download development packages:

   ```sh
   sudo sed '/deb-src/s/^#//g' -i /etc/apt/sources.list
   sudo apt-get update
   sudo apt-get install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf llvm build-essential autoconf automake autotools-dev libtool pkgconf asciidoc xmlto uuid-dev libjson-c-dev libkeyutils-dev libz-dev libssl-dev debhelper devscripts debmake quilt fakeroot lintian asciidoctor file gnupg patch patchutils
   ```

3. Add boot parameters to enable 1GB hugepages and DSA compatibility:

   ```sh
   sudo echo 'GRUB_CMDLINE_LINUX_DEFAULT="${GRUB_CMDLINE_LINUX_DEFAULT} intel_iommu=on,sm_on no5lvl default_hugepagesz=1G hugepagesz=1G hugepages=32"' | sudo tee -a /etc/default/grub
   sudo update-grub
   sudo reboot now
   ```

4. Install DSA config tool and library:

   ```sh
   git clone https://github.com/intel/idxd-config.git
   cd idxd-config
   ./autogen.sh
   ./configure CFLAGS='-g -O2' --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib --enable-test=yes
   make
   sudo make check
   sudo make install
   ```

5. Config Intel DSA:

   ```sh
   sudo accel-config config-device dsa0
   sudo accel-config config-engine dsa0/engine0.2 --group-id=0
   # Set maximum transimission size as 1GB and block-on-fault
   sudo accel-config config-wq dsa0/wq0.0 --group-id=0 --wq-size=32 --priority=1 --block-on-fault=1 --threshold=4 --type=user --name=swq --mode=shared --max-batch-size=32 --max-transfer-size=1073741824
   sudo accel-config enable-device dsa0
   sudo accel-config enable-wq dsa0/wq0.0
   ```

   Or config via file:
   ```sh
   sudo accel-config load-config -c net_profile.conf -e
   ```

## Execute Experiment
### Memory Integrity Experiment
```sh
make test LDLIBS=-laccel-config
sudo ./test
```

### Confidentiality Experiment
#### Run Experiment
```sh
make side_channel LDLIBS=-laccel-config
sudo ./side_channel > side_channel.log
```
#### Scatter graph
```sh
python3 output.py
```
