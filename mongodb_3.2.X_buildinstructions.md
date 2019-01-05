# 1. Summary

These instructions are for building mongodb 3.2.X for 32-bit arm architecture (*armhf* in debian).

It is recommended to cross-compile for speed and to avoid various memory related issues on single board computers. The unit tests are cross-compiled, transferred and ran on the target machine to verify the build.

# 2. Setup Schroot environment for builds

Source: https://wiki.debian.org/CrossCompiling#Building_manually_outside_of_packages

Setup an schroot for compiling with a blank slate.

```bash
sudo apt-get install -y sbuild schroot debootstrap
sudo sbuild-createchroot --make-sbuild-tarball=/srv/chroots/stretch-sbuild.tgz stretch /srv/chroots/stretch http://httpredir.debian.org/debian/
```

If you have not used sbuild before on this machine it needs setting up on your machine:

```bash
sudo sbuild-adduser <your-username>
sbuild-update --keygen
```

Get list of schroots. (Check that the environment that you just created is present).
```bash
schroot -l
```

Add sudo to the schroot that was just created.
```bash
sudo sbuild-apt stretch-<arch>-sbuild apt-get install sudo
```

To make your home directory available to the schroot environment set in the file **`/etc/schroot/chroot.d/stretch-<arch>-sbuild-<uniqueID>`**

```bash
profile=default

```

# 3. Configure useful debian build helper script

Source: https://www.debian.org/doc/manuals/maint-guide/update.en.html

Append (use nano for example) to ~/.bashrc

```bash
#Source: https://www.debian.org/doc/manuals/maint-guide/modify.en.html#quiltrc

alias dquilt="quilt --quiltrc=${HOME}/.quiltrc-dpkg"
complete -F _quilt_completion -o filenames dquilt
```

Then let's create ~/.quiltrc-dpkg as follows: 

```bash
#Source: https://www.debian.org/doc/manuals/maint-guide/modify.en.html#quiltrc

d=. ; while [ ! -d $d/debian -a $(readlink -e $d) != / ]; do d=$d/..; done
if [ -d $d/debian ] && [ -z $QUILT_PATCHES ]; then
    # if in Debian packaging tree with unset $QUILT_PATCHES
    QUILT_PATCHES="debian/patches"
    QUILT_PATCH_OPTS="--reject-format=unified"
    QUILT_DIFF_ARGS="-p ab --no-timestamps --no-index --color=auto"
    QUILT_REFRESH_ARGS="-p ab --no-timestamps --no-index"
    QUILT_COLORS="diff_hdr=1;32:diff_add=1;34:diff_rem=1;31:diff_hunk=1;33:diff_ctx=35:diff_cctx=33"
    if ! [ -d $d/debian/patches ]; then mkdir $d/debian/patches; fi
fi
```

# 4. Configure the Schroot environment for building mongodb 3.2.X

* **Follow 4.1 if building on amd64 machine**
* **Follow 4.2 if building on tinkerboard etc**

## 4.1. Cross-building with *armhf* target
```bash
#Enter the chroot environment
schroot -c stretch-<arch>-sbuild

sudo dpkg --add-architecture armhf 

#Get packages
sudo apt-get update && sudo apt-get upgrade -y

sudo apt install -y build-essential crossbuild-essential-armhf scons debhelper dh-systemd
sudo apt install -y nano scons build-essential wget autoconf automake autotools-dev debhelper dh-make devscripts fakeroot lintian patch patchutils perl python quilt dh-systemd

sudo apt install -y libboost-filesystem-dev:armhf libboost-program-options-dev:armhf libboost-system-dev:armhf libboost-thread-dev:armhf 
sudo apt install libboost-date-time-dev:armhf libboost-dev:armhf libboost-filesystem-dev:armhf libboost-program-options-dev:armhf libboost-thread-dev:armhf libboost-regex-dev:armhf libgoogle-perftools-dev:armhf libyaml-cpp-dev:armhf libpcap0.8-dev:armhf libpcre3-dev:armhf libreadline-dev:armhf libsnappy-dev:armhf libstemmer-dev:armhf libssl1.0-dev:armhf zlib1g-dev:armhf python-pymongo python-subprocess32 python-yaml python valgrind:armhf
```


## 4.2. Building directly on target architecture

```bash
#Enter the chroot environment
schroot -c stretch-<arch>-sbuild

#Get packages
sudo apt-get update && sudo apt-get upgrade -y
sudo apt install -y nano scons build-essential wget autoconf automake autotools-dev debhelper dh-make devscripts fakeroot lintian patch patchutils perl python quilt dh-systemd
sudo apt install -y libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev 
sudo apt install -y libboost-date-time-dev libboost-dev libboost-regex-dev libyaml-cpp-dev libpcap-dev libpcre3-dev libreadline-dev libsnappy-dev libstemmer-dev libssl1.0-dev zlib1g-dev python-pymongo python-subprocess32 python-yaml valgrind libgoogle-perftools-dev
```

# 5. Instructions for build using debian packaging framework
## 5.1. Obtain the sources.
### 5.1.1. From git

Clone the repository. `git clone git://github.com/andrewharle/mongodb.git && cd mongodb/`

Access the correct branch, for 3.2.X stable/stretch is used. `git checkout stable/stretch`

### 5.2.1. From debian sources

Get the mongodb source for the distro from the debian repo.

```bash
apt-get source mongodb-server

#load the source directory
cd mongodb-3.2.X
```

## 5.2. Configure the sources

The changes to the source/patches have been committed to the repository and are included here for completeness.

### 5.2.1. Get the latest mongodb 3.2 release.

```bash
#automatically download the *latest release
uscan

#Prepare the update to the new version.
uupdate -v 3.2.X ../mongodb-src-r3.2.X.tar.gz
cd ../mongodb-3.2.X
rm -R debian.upstream/
```
### 5.2.2. Remove CVE-2016-6494 patch.

For version 3.2.22 remove the patch CVE-2016-6494. It has been implemented in the upstream source.
```bash
dquilt delete CVE-2016-6494.patch
while dquilt push; do dquilt refresh; done
```
### 5.2.3. Obtain the mozjs-38 source for *armhf*.

Navigate to the mozjs sources: `cd src/third_party/mozjs-38/`

Edit the *gen-config.sh* file as follows. Replace the line:

> `PYTHON=python ./configure --without-intl-api --enable-posix-nspr-emulation --disable-trace-logging`

With

> `PYTHON=/usr/bin/python CROSS_COMPILE=1 CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++  ./configure --without-intl-api --enable-posix-nspr-emulation --target=arm --disable-trace-logging`

Then download the sources as follows:

```bash
./get_sources.sh
./gen-config.sh arm linux
rm firefox-38.X.Xesr.source.tar.bz2  
rm -R mozilla-release/ 
cd ../../.. 
```

Create a debian source patch `dpkg-source --commit`

### 5.2.4. Fix alignment exception.

Backport the [[SERVER-22802] - mmapv1 btree key should read and write little endian](https://jira.mongodb.org/browse/SERVER-22802) patch to the 3.2.X builds.

`dpkg-source --commit`

### 5.2.5. Modify the build rules

In *debian/rules*:

* Append `--mmapv1=on` to `COMMON_OPTIONS += --wiredtiger=off`.

To enable build of the test suite without running the tests replace `override_dh_auto_test` with:

```bash
ifeq (,$(filter nocheckbuild,$(DEB_BUILD_OPTIONS)))
override_dh_auto_test:
	scons $(COMMON_OPTIONS) dbtest unittests

ifeq (,$(filter nocheck,$(DEB_BUILD_OPTIONS)))
	python ./buildscripts/resmoke.py --dbpathPrefix="$(CURDIR)/debian/tmp-test" --suites=dbtest,unittests
endif
.PHONY: override_dh_auto_test
endif
```

The **DEB_BUILD_OPTIONS** parameter meaning is then:

* **nocheckbuild** - Test suite not built and compiled binaries not tested.
* **nocheck** - Build test suite, but do not run test of compiled binaries.

### 5.2.6. Update debian packaing metadata

* Create changelog entry => `dch -i`
* Edit the *debian\control* file and update the architectures.
* Ensure that source patches are created for each modification `dpkg-source --commit`.

### 5.2.7. Build Commands
#### 5.2.7.1. Cross-Build command

**Build**

`DEB_SIGN_KEYID=XXXXXXXXXXXXXX CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ CONFIG_SITE=/etc/dpkg-cross/cross-config.armhf  DEB_BUILD_OPTIONS="nodbgsym nocheck" dpkg-buildpackage -aarmhf -b`

> This will probably fail and print a list of packages that weren't found. If the packages are only python related then there probably isn't an issue (it seems to be a limitation of the debian cross-build environment) and `-d` can be appended to the build command.

**Cleanup command**

`CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ CONFIG_SITE=/etc/dpkg-cross/cross-config.armhf  DEB_BUILD_OPTIONS="nodbgsym nocheck" dpkg-buildpackage -aarmhf --rules-target override_dh_auto_clean -rfakeroot -d -us -uc`

**Test**

Transfer the build using *rsync* or similar.

`rsync -avh mongodb-3.2.22/ andrew@192.168.1.180:~/mongodb/mongodb-3.2.22/`

Execute the test. Confirm succesful run before deployment.

`python ./buildscripts/resmoke.py --dbpathPrefix="/home/andrew/mongodb/mongodb-3.2.22/debian/tmp-test" --suites=dbtest,unittests --storageEngine=mmapv1`

#### 5.2.7.2. Native-build command:
**Build and Test**

`DEB_SIGN_KEYID=XXXXXXXXXXXXXX DEB_BUILD_OPTIONS=nodbgsym dpkg-buildpackage -b -j3`

**Cleanup command**

`DEB_BUILD_OPTIONS=nodbgsym dpkg-buildpackage --rules-target override_dh_auto_clean -rfakeroot -d -us -uc`

#### 5.2.7.3. Other useful commands:
**These commands can be used to switch between compilers if required.**

`sudo update-alternatives --config c++`

`sudo update-alternatives --config cc`

**If a different linker is required then install lld and create a symbolic link so that SCONS can find it.**

`sudo apt install lld-4.0`

`sudo ln -s /usr/bin/ld.lld-4.0 /usr/bin/ld.lld`
