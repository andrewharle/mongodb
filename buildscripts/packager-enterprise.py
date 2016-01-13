#!/usr/bin/python

# This program makes Debian and RPM repositories for MongoDB, by
# downloading our tarballs of statically linked executables and
# insinuating them into Linux packages.  It must be run on a
# Debianoid, since Debian provides tools to make RPMs, but RPM-based
# systems don't provide debian packaging crud.

# Notes:
#
# * Almost anything that you want to be able to influence about how a
# package construction must be embedded in some file that the
# packaging tool uses for input (e.g., debian/rules, debian/control,
# debian/changelog; or the RPM specfile), and the precise details are
# arbitrary and silly.  So this program generates all the relevant
# inputs to the packaging tools.
#
# * Once a .deb or .rpm package is made, there's a separate layer of
# tools that makes a "repository" for use by the apt/yum layers of
# package tools.  The layouts of these repositories are arbitrary and
# silly, too.
#
# * Before you run the program on a new host, these are the
# prerequisites:
#
# apt-get install dpkg-dev rpm debhelper fakeroot ia32-libs createrepo git-core libsnmp15
# echo "Now put the dist gnupg signing keys in ~root/.gnupg"

import errno
import getopt
from glob import glob
from packager import httpget
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import urlparse

# For the moment, this program runs on the host that also serves our
# repositories to the world, so the last thing the program does is
# move the repositories into place.  Make this be the path where the
# web server will look for repositories.
REPOPATH="/var/www/repo"

# The MongoDB names for the architectures we support.
ARCHES=["x86_64"]

# Made up names for the flavors of distribution we package for.
DISTROS=["suse","debian","redhat","ubuntu"]


class Spec(object):
    def __init__(self, specstr):
        tup = specstr.split(":")
        self.ver = tup[0]
        # Hack: the second item in the tuple is treated as a suffix if
        # it lacks an equals sign; otherwise it's the start of named
        # parameters.
        self.suf = None
        if len(tup) > 1 and tup[1].find("=") == -1:
            self.suf = tup[1]
        # Catch-all for any other parameters to the packaging.
        i = 2 if self.suf else 1
        self.params = dict([s.split("=") for s in tup[i:]])
        for key in self.params.keys():
            assert(key in ["suffix", "revision"])

    def version(self):
        return self.ver

    def version_better_than(self, version_string):
        # FIXME: this is wrong, but I'm in a hurry.
        # e.g., "1.8.2" < "1.8.10", "1.8.2" < "1.8.2-rc1"
        return self.ver > version_string

    def suffix(self):
        # suffix is what we tack on after pkgbase.
        if self.suf:
            return self.suf
        elif "suffix" in self.params:
            return self.params["suffix"]
        else:
            return "-enterprise" if int(self.ver.split(".")[1])%2==0 else "-enterprise-unstable"


    def pversion(self, distro):
        # Note: Debian packages have funny rules about dashes in
        # version numbers, and RPM simply forbids dashes.  pversion
        # will be the package's version number (but we need to know
        # our upstream version too).
        if re.search("^(debian|ubuntu)", distro.name()):
            return re.sub("-", "~", self.ver)
        elif re.search("(suse|redhat|fedora|centos)", distro.name()):
            return re.sub("\\d+-", "", self.ver)
        else:
            raise Exception("BUG: unsupported platform?")

    def param(self, param):
        if param in self.params:
            return self.params[param]
        return None

    def branch(self):
        """Return the major and minor portions of the specificed version.
        For example, if the version is "2.5.5" the branch would be "2.5"
        """
        return ".".join(self.ver.split(".")[0:2])

class Distro(object):
    def __init__(self, string):
        self.n=string

    def name(self):
        return self.n

    def pkgbase(self):
        return "mongodb"

    def archname(self, arch):
        if re.search("^(debian|ubuntu)", self.n):
            return "i386" if arch.endswith("86") else "amd64"
        elif re.search("^(suse|centos|redhat|fedora)", self.n):
            return "i686" if arch.endswith("86") else "x86_64"
        else:
            raise Exception("BUG: unsupported platform?")

    def repodir(self, arch, build_os, spec):
        """Return the directory where we'll place the package files for
        (distro, distro_version) in that distro's preferred repository
        layout (as distinct from where that distro's packaging building
        tools place the package files).

        Examples:

        repo/apt/ubuntu/dists/precise/mongodb-enterprise/2.5/multiverse/binary-amd64
        repo/apt/ubuntu/dists/precise/mongodb-enterprise/2.5/multiverse/binary-i386

        repo/apt/ubuntu/dists/trusty/mongodb-enterprise/2.5/multiverse/binary-amd64
        repo/apt/ubuntu/dists/trusty/mongodb-enterprise/2.5/multiverse/binary-i386

        repo/apt/debian/dists/wheezy/mongodb-enterprise/2.5/main/binary-amd64
        repo/apt/debian/dists/wheezy/mongodb-enterprise/2.5/main/binary-i386

        repo/yum/redhat/6/mongodb-enterprise/2.5/x86_64
        yum/redhat/6/mongodb-enterprise/2.5/i386

        repo/zypper/suse/11/mongodb-enterprise/2.5/x86_64
        zypper/suse/11/mongodb-enterprise/2.5/i386

        """

        if re.search("^(debian|ubuntu)", self.n):
            return "repo/apt/%s/dists/%s/mongodb-enterprise/%s/%s/binary-%s/" % (self.n, self.repo_os_version(build_os), spec.branch(), self.repo_component(), self.archname(arch))
        elif re.search("(redhat|fedora|centos)", self.n):
            return "repo/yum/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (self.n, self.repo_os_version(build_os), spec.branch(), self.archname(arch))
        elif re.search("(suse)", self.n):
            return "repo/zypper/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (self.n, self.repo_os_version(build_os), spec.branch(), self.archname(arch))
        else:
            raise Exception("BUG: unsupported platform?")

    def repo_component(self):
        """Return the name of the section/component/pool we are publishing into -
        e.g. "multiverse" for Ubuntu, "main" for debian."""
        if self.n == 'ubuntu':
          return "multiverse"
        elif self.n == 'debian':
          return "main"
        else:
            raise Exception("unsupported distro: %s" % self.n)

    def repo_os_version(self, build_os):
        """Return an OS version suitable for package repo directory
        naming - e.g. 5, 6 or 7 for redhat/centos, "precise," "wheezy," etc.
        for Ubuntu/Debian, 11 for suse"""
        if self.n == 'suse':
            return re.sub(r'^suse(\d+)$', r'\1', build_os)
        if self.n == 'redhat':
            return re.sub(r'^rhel(\d).*$', r'\1', build_os)
        elif self.n == 'ubuntu':
            if build_os == 'ubuntu1204':
                return "precise"
            elif build_os == 'ubuntu1404':
                return "trusty"
            else:
                raise Exception("unsupported build_os: %s" % build_os)
        elif self.n == 'debian':
            if build_os == 'debian71':
                return 'wheezy'
            else:
                raise Exception("unsupported build_os: %s" % build_os)
        else:
            raise Exception("unsupported distro: %s" % self.n)

    def make_pkg(self, build_os, arch, spec, srcdir):
        if re.search("^(debian|ubuntu)", self.n):
            return make_deb(self, build_os, arch, spec, srcdir)
        elif re.search("^(suse|centos|redhat|fedora)", self.n):
            return make_rpm(self, build_os, arch, spec, srcdir)
        else:
            raise Exception("BUG: unsupported platform?")

    def build_os(self):
        """Return the build os label in the binary package to download ("rhel57", "rhel62" and "rhel70"
        for redhat, "ubuntu1204" and "ubuntu1404" for Ubuntu, "debian71" for Debian, and "suse11" for SUSE)"""

        if re.search("(suse)", self.n):
            return [ "suse11" ]
        if re.search("(redhat|fedora|centos)", self.n):
            return [ "rhel70", "rhel62", "rhel57" ]
        elif self.n == 'ubuntu':
            return [ "ubuntu1204", "ubuntu1404" ]
        elif self.n == 'debian':
            return [ "debian71" ]
        else:
            raise Exception("BUG: unsupported platform?")

    def release_dist(self, build_os):
        """Return the release distribution to use in the rpm - "el5" for rhel 5.x,
        "el6" for rhel 6.x, return anything else unchanged"""

        return re.sub(r'^rh(el\d).*$', r'\1', build_os)
def main(argv):
    (flags, specs) = parse_args(argv[1:])
    distros=[Distro(distro) for distro in DISTROS]

    oldcwd=os.getcwd()
    srcdir=oldcwd+"/../"

    # We do all our work in a randomly-created directory. You can set
    # TEMPDIR to influence where this program will do stuff.
    prefix=tempfile.mkdtemp()
    print "Working in directory %s" % prefix

    # This will be a list of directories where we put packages in
    # "repository layout".
    repos=[]

    os.chdir(prefix)
    try:
        # Download the binaries.
        urlfmt="http://downloads.mongodb.com/linux/mongodb-linux-%s-enterprise-%s-%s.tgz"

        # Build a pacakge for each distro/spec/arch tuple, and
        # accumulate the repository-layout directories.
        for (distro, spec, arch) in crossproduct(distros, specs, ARCHES):

          for build_os in distro.build_os():

            httpget(urlfmt % (arch, build_os, spec.version()), ensure_dir(tarfile(build_os, arch, spec)))

            repo = make_package(distro, build_os, arch, spec, srcdir)
            make_repo(repo, distro, build_os, spec)

    finally:
        os.chdir(oldcwd)
    if "-n" not in flags:
        move_repos_into_place(prefix+"/repo", REPOPATH)
        # FIXME: try shutil.rmtree some day.
        sysassert(["rm", "-rv", prefix])


def parse_args(args):
    if len(args) == 0:
        print """Usage: packager.py [OPTS] SPEC1 SPEC2 ... SPECn

Options:

  -n:  Just build the packages, don't publish them as a repo
       or clean out the working directory

Each SPEC is a mongodb version string optionally followed by a colon
and some parameters, of the form <paramname>=<value>.  Supported
parameters:

  suffix -- suffix to append to the package's base name.  (If
            unsupplied, suffixes default based on the parity of the
            middle number in the version.)

  revision -- least-order version number to packaging systems
"""
        sys.exit(0)

    try:
        (flags, args) = getopt.getopt(args, "n")
    except getopt.GetoptError, err:
        print str(err)
        sys.exit(2)
    flags=dict(flags)
    specs=[Spec(arg) for arg in args]
    return (flags, specs)

def crossproduct(*seqs):
    """A generator for iterating all the tuples consisting of elements
    of seqs."""
    l = len(seqs)
    if l == 0:
        pass
    elif l == 1:
        for i in seqs[0]:
            yield [i]
    else:
        for lst in crossproduct(*seqs[:-1]):
            for i in seqs[-1]:
                lst2=list(lst)
                lst2.append(i)
                yield lst2

def sysassert(argv):
    """Run argv and assert that it exited with status 0."""
    print "In %s, running %s" % (os.getcwd(), " ".join(argv))
    sys.stdout.flush()
    sys.stderr.flush()
    assert(subprocess.Popen(argv).wait()==0)

def backtick(argv):
    """Run argv and return its output string."""
    print "In %s, running %s" % (os.getcwd(), " ".join(argv))
    sys.stdout.flush()
    sys.stderr.flush()
    return subprocess.Popen(argv, stdout=subprocess.PIPE).communicate()[0]

def ensure_dir(filename):
    """Make sure that the directory that's the dirname part of
    filename exists, and return filename."""
    dirpart = os.path.dirname(filename)
    try:
        os.makedirs(dirpart)
    except OSError: # as exc: # Python >2.5
        exc=sys.exc_value
        if exc.errno == errno.EEXIST:
            pass
        else:
            raise exc
    return filename


def tarfile(build_os, arch, spec):
    """Return the location where we store the downloaded tarball for
    this package"""
    return "dl/mongodb-linux-%s-enterprise-%s-%s.tar.gz" % (spec.version(), build_os, arch)

def setupdir(distro, build_os, arch, spec):
    # The setupdir will be a directory containing all inputs to the
    # distro's packaging tools (e.g., package metadata files, init
    # scripts, etc), along with the already-built binaries).  In case
    # the following format string is unclear, an example setupdir
    # would be dst/x86_64/debian-sysvinit/wheezy/mongodb-org-unstable/
    # or dst/x86_64/redhat/rhel57/mongodb-org-unstable/
    return "dst/%s/%s/%s/%s%s-%s/" % (arch, distro.name(), build_os, distro.pkgbase(), spec.suffix(), spec.pversion(distro))

def unpack_binaries_into(build_os, arch, spec, where):
    """Unpack the tarfile for (build_os, arch, spec) into directory where."""
    rootdir=os.getcwd()
    ensure_dir(where)
    # Note: POSIX tar doesn't require support for gtar's "-C" option,
    # and Python's tarfile module prior to Python 2.7 doesn't have the
    # features to make this detail easy.  So we'll just do the dumb
    # thing and chdir into where and run tar there.
    os.chdir(where)
    try:
	sysassert(["tar", "xvzf", rootdir+"/"+tarfile(build_os, arch, spec)])
    	release_dir = glob('mongodb-linux-*')[0]
        for releasefile in "bin", "snmp", "LICENSE.txt", "README", "THIRD-PARTY-NOTICES":
            os.rename("%s/%s" % (release_dir, releasefile), releasefile)
        os.rmdir(release_dir)
    except Exception:
        exc=sys.exc_value
        os.chdir(rootdir)
        raise exc
    os.chdir(rootdir)

def make_package(distro, build_os, arch, spec, srcdir):
    """Construct the package for (arch, distro, spec), getting
    packaging files from srcdir and any user-specified suffix from
    suffixes"""

    sdir=setupdir(distro, build_os, arch, spec)
    ensure_dir(sdir)
    # Note that the RPM packages get their man pages from the debian
    # directory, so the debian directory is needed in all cases (and
    # innocuous in the debianoids' sdirs).
    for pkgdir in ["debian", "rpm"]:
        print "Copying packaging files from %s to %s" % ("%s/%s" % (srcdir, pkgdir), sdir)
        # FIXME: sh-dash-cee is bad. See if tarfile can do this.
        sysassert(["sh", "-c", "(cd \"%s\" && git archive r%s %s/ ) | (cd \"%s\" && tar xvf -)" % (srcdir, spec.version(), pkgdir, sdir)])
    # Splat the binaries and snmp files under sdir.  The "build" stages of the
    # packaging infrastructure will move the files to wherever they
    # need to go.
    unpack_binaries_into(build_os, arch, spec, sdir)
    # Remove the mongosniff binary due to libpcap dynamic
    # linkage.  FIXME: this removal should go away
    # eventually.
    if os.path.exists(sdir + "bin/mongosniff"):
      os.unlink(sdir + "bin/mongosniff")
    return distro.make_pkg(build_os, arch, spec, srcdir)

def make_repo(repodir, distro, build_os, spec):
    if re.search("(debian|ubuntu)", repodir):
        make_deb_repo(repodir, distro, build_os, spec)
    elif re.search("(suse|centos|redhat|fedora)", repodir):
        make_rpm_repo(repodir)
    else:
        raise Exception("BUG: unsupported platform?")

def make_deb(distro, build_os, arch, spec, srcdir):
    # I can't remember the details anymore, but the initscript/upstart
    # job files' names must match the package name in some way; and
    # see also the --name flag to dh_installinit in the generated
    # debian/rules file.
    suffix=spec.suffix()
    sdir=setupdir(distro, build_os, arch, spec)
    if re.search("debian", distro.name()):
        os.link(sdir+"debian/init.d", sdir+"debian/%s%s-server.mongod.init" % (distro.pkgbase(), suffix))
        os.unlink(sdir+"debian/mongod.upstart")
    elif re.search("ubuntu", distro.name()):
        os.link(sdir+"debian/mongod.upstart", sdir+"debian/%s%s-server.mongod.upstart" % (distro.pkgbase(), suffix))
        os.unlink(sdir+"debian/init.d")
    else:
        raise Exception("unknown debianoid flavor: not debian or ubuntu?")
    # Rewrite the control and rules files
    write_debian_changelog(sdir+"debian/changelog", spec, srcdir)
    distro_arch=distro.archname(arch)
    sysassert(["cp", "-v", srcdir+"debian/%s%s.control" % (distro.pkgbase(), suffix), sdir+"debian/control"])
    sysassert(["cp", "-v", srcdir+"debian/%s%s.rules" % (distro.pkgbase(), suffix), sdir+"debian/rules"])


    # old non-server-package postinst will be hanging around for old versions
    #
    if os.path.exists(sdir+"debian/postinst"):
      os.unlink(sdir+"debian/postinst")

    # copy our postinst files
    #
    sysassert(["sh", "-c", "cp -v \"%sdebian/\"*.postinst \"%sdebian/\""%(srcdir, sdir)])

    # Do the packaging.
    oldcwd=os.getcwd()
    try:
        os.chdir(sdir)
        sysassert(["dpkg-buildpackage", "-a"+distro_arch, "-k Richard Kreuter <richard@10gen.com>"])
    finally:
        os.chdir(oldcwd)
    r=distro.repodir(arch, build_os, spec)
    ensure_dir(r)
    # FIXME: see if shutil.copyfile or something can do this without
    # much pain.
    #sysassert(["cp", "-v", sdir+"../%s%s_%s%s_%s.deb"%(distro.pkgbase(), suffix, spec.pversion(distro), "-"+spec.param("revision") if spec.param("revision") else"", distro_arch), r])
    sysassert(["sh", "-c", "cp -v \"%s/../\"*.deb \"%s\""%(sdir, r)])
    return r

def make_deb_repo(repo, distro, build_os, spec):
    # Note: the Debian repository Packages files must be generated
    # very carefully in order to be usable.
    oldpwd=os.getcwd()
    os.chdir(repo+"../../../../../../")
    try:
        dirs=set([os.path.dirname(deb)[2:] for deb in backtick(["find", ".", "-name", "*.deb"]).split()])
        for d in dirs:
            s=backtick(["dpkg-scanpackages", d, "/dev/null"])
            f=open(d+"/Packages", "w")
            try:
                f.write(s)
            finally:
                f.close()
            b=backtick(["gzip", "-9c", d+"/Packages"])
            f=open(d+"/Packages.gz", "wb")
            try:
                f.write(b)
            finally:
                f.close()
    finally:
        os.chdir(oldpwd)
    # Notes: the Release{,.gpg} files must live in a special place,
    # and must be created after all the Packages.gz files have been
    # done.
    s="""Origin: mongodb
Label: mongodb
Suite: mongodb
Codename: %s/mongodb-enterprise
Architectures: amd64
Components: %s
Description: MongoDB packages
""" % (distro.repo_os_version(build_os), distro.repo_component())
    if os.path.exists(repo+"../../Release"):
        os.unlink(repo+"../../Release")
    if os.path.exists(repo+"../../Release.gpg"):
        os.unlink(repo+"../../Release.gpg")
    oldpwd=os.getcwd()
    os.chdir(repo+"../../")
    s2=backtick(["apt-ftparchive", "release", "."])
    try:
        f=open("Release", 'w')
        try:
            f.write(s)
            f.write(s2)
        finally:
            f.close()

        arg=None
        for line in backtick(["gpg", "--list-keys"]).split("\n"):
            tokens=line.split()
            if len(tokens)>0 and tokens[0] == "uid":
                arg=tokens[-1]
                break
        # Note: for some reason, I think --no-tty might be needed
        # here, but maybe not.
        sysassert(["gpg", "-r", arg, "--no-secmem-warning", "-abs", "--output", "Release.gpg", "Release"])
    finally:
        os.chdir(oldpwd)


def move_repos_into_place(src, dst):
    # Find all the stuff in src/*, move it to a freshly-created
    # directory beside dst, then play some games with symlinks so that
    # dst is a name the new stuff and dst+".old" names the previous
    # one.  This feels like a lot of hooey for something so trivial.

    # First, make a crispy fresh new directory to put the stuff in.
    i=0
    while True:
        date_suffix=time.strftime("%Y-%m-%d")
        dname=dst+".%s.%d" % (date_suffix, i)
        try:
            os.mkdir(dname)
            break
        except OSError:
            exc=sys.exc_value
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise exc
        i=i+1

    # Put the stuff in our new directory.
    for r in os.listdir(src):
        sysassert(["cp", "-rv", src + "/" + r, dname])

    # Make a symlink to the new directory; the symlink will be renamed
    # to dst shortly.
    i=0
    while True:
        tmpnam=dst+".TMP.%d" % i
        try:
            os.symlink(dname, tmpnam)
            break
        except OSError: # as exc: # Python >2.5
            exc=sys.exc_value
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise exc
        i=i+1

    # Make a symlink to the old directory; this symlink will be
    # renamed shortly, too.
    oldnam=None
    if os.path.exists(dst):
       i=0
       while True:
           oldnam=dst+".old.%d" % i
           try:
               os.symlink(os.readlink(dst), oldnam)
               break
           except OSError: # as exc: # Python >2.5
               exc=sys.exc_value
               if exc.errno == errno.EEXIST:
                   pass
               else:
                   raise exc

    os.rename(tmpnam, dst)
    if oldnam:
        os.rename(oldnam, dst+".old")


def write_debian_changelog(path, spec, srcdir):
    oldcwd=os.getcwd()
    os.chdir(srcdir)
    preamble=""
    if spec.param("revision"):
        preamble="""mongodb%s (%s-%s) unstable; urgency=low

  * Bump revision number

 -- Richard Kreuter <richard@10gen.com>  %s

""" % (spec.suffix(), spec.pversion(Distro("debian")), spec.param("revision"), time.strftime("%a, %d %b %Y %H:%m:%S %z"))
    try:
        s=preamble+backtick(["sh", "-c", "git archive r%s debian/changelog | tar xOf -" % spec.version()])
    finally:
        os.chdir(oldcwd)
    f=open(path, 'w')
    lines=s.split("\n")
    # If the first line starts with "mongodb", it's not a revision
    # preamble, and so frob the version number.
    lines[0]=re.sub("^mongodb \\(.*\\)", "mongodb (%s)" % (spec.pversion(Distro("debian"))), lines[0])
    # Rewrite every changelog entry starting in mongodb<space>
    lines=[re.sub("^mongodb ", "mongodb%s " % (spec.suffix()), l) for l in lines]
    lines=[re.sub("^  --", " --", l) for l in lines]
    s="\n".join(lines)
    try:
        f.write(s)
    finally:
        f.close()

def make_rpm(distro, build_os, arch, spec, srcdir):
    # Create the specfile.
    suffix=spec.suffix()
    sdir=setupdir(distro, build_os, arch, spec)

    # Use special suse init script if we're building for SUSE 
    #
    if distro.name() == "suse":
        os.unlink(sdir+"rpm/init.d-mongod")
        os.link(sdir+"rpm/init.d-mongod.suse", sdir+"rpm/init.d-mongod")

    specfile=srcdir+"rpm/mongodb%s.spec" % suffix
    topdir=ensure_dir('%s/rpmbuild/%s/' % (os.getcwd(), build_os))
    for subdir in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"]:
        ensure_dir("%s/%s/" % (topdir, subdir))
    distro_arch=distro.archname(arch)
    # RPM tools take these macro files that define variables in
    # RPMland.  Unfortunately, there's no way to tell RPM tools to use
    # a given file *in addition* to the files that it would already
    # load, so we have to figure out what it would normally load,
    # augment that list, and tell RPM to use the augmented list.  To
    # figure out what macrofiles ordinarily get loaded, older RPM
    # versions had a parameter called "macrofiles" that could be
    # extracted from "rpm --showrc".  But newer RPM versions don't
    # have this.  To tell RPM what macros to use, older versions of
    # RPM have a --macros option that doesn't work; on these versions,
    # you can put a "macrofiles" parameter into an rpmrc file.  But
    # that "macrofiles" setting doesn't do anything for newer RPM
    # versions, where you have to use the --macros flag instead.  And
    # all of this is to let us do our work with some guarantee that
    # we're not clobbering anything that doesn't belong to us.  Why is
    # RPM so braindamaged?
    macrofiles=[l for l in backtick(["rpm", "--showrc"]).split("\n") if l.startswith("macrofiles")]
    flags=[]
    macropath=os.getcwd()+"/macros"

    write_rpm_macros_file(macropath, topdir, distro.release_dist(build_os))
    if len(macrofiles)>0:
        macrofiles=macrofiles[0]+":"+macropath
        rcfile=os.getcwd()+"/rpmrc"
        write_rpmrc_file(rcfile, macrofiles)
        flags=["--rpmrc", rcfile]
    else:
        # This hard-coded hooey came from some box running RPM
        # 4.4.2.3.  It may not work over time, but RPM isn't sanely
        # configurable.
        flags=["--macros", "/usr/lib/rpm/macros:/usr/lib/rpm/%s-linux/macros:/etc/rpm/macros.*:/etc/rpm/macros:/etc/rpm/%s-linux/macros:~/.rpmmacros:%s" % (distro_arch, distro_arch, macropath)]
    # Put the specfile and the tar'd up binaries and stuff in
    # place. FIXME: see if shutil.copyfile can do this without too
    # much hassle.
    sysassert(["cp", "-v", specfile, topdir+"SPECS/"])
    oldcwd=os.getcwd()
    os.chdir(sdir+"/../")
    try:
        sysassert(["tar", "-cpzf", topdir+"SOURCES/mongodb%s-%s.tar.gz" % (suffix, spec.pversion(distro)), os.path.basename(os.path.dirname(sdir))])
    finally:
        os.chdir(oldcwd)
    # Do the build.
    sysassert(["rpmbuild", "-ba", "--target", distro_arch] + flags + ["%s/SPECS/mongodb%s.spec" % (topdir, suffix)])
    r=distro.repodir(arch, build_os, spec)
    ensure_dir(r)
    # FIXME: see if some combination of shutil.copy<hoohah> and glob
    # can do this without shelling out.
    sysassert(["sh", "-c", "cp -v \"%s/RPMS/%s/\"*.rpm \"%s\""%(topdir, distro_arch, r)])
    return r

def make_rpm_repo(repo):
    oldpwd=os.getcwd()
    os.chdir(repo+"../")
    try:
        sysassert(["createrepo", "."])
    finally:
        os.chdir(oldpwd)


def write_rpmrc_file(path, string):
    f=open(path, 'w')
    try:
        f.write(string)
    finally:
        f.close()

def write_rpm_macros_file(path, topdir, release_dist):
    f=open(path, 'w')
    try:
        f.write("%%_topdir	%s\n" % topdir)
        f.write("%%dist	.%s\n" % release_dist)
        f.write("%_use_internal_dependency_generator 0\n")
    finally:
        f.close()

if __name__ == "__main__":
    main(sys.argv)
