#!/usr/bin/env python

# makedist.py: make a distro package (on an EC2 instance)

# For ease of use, put a file called settings.py someplace in your
# sys.path, containing something like the following:

# makedist = {
#     # ec2-api-tools needs the following two set in the process
#     # environment.
#     "EC2_HOME": "/path/to/ec2-api-tools",
#     # The EC2 tools won't run at all unless this variable is set to a directory
#     # relative to which a "bin/java" exists.
#     "JAVA_HOME" : "/usr",
#     # All the ec2-api-tools take these two as arguments.
#     # Alternatively, you can set the environment variables EC2_PRIVATE_KEY and EC2_CERT
#     # respectively, leave these two out of settings.py, and let the ec2 tools default.
#     "ec2_pkey": "/path/to/pk-file.pem"
#     "ec2_cert" : "/path/to/cert-file.pem"
#     # This gets supplied to ec2-run-instances to rig up an ssh key for
#     # the remote user.
#     "ec2_sshkey" : "key-id",
#     # And so we need to tell our ssh processes where to find the
#     # appropriate public key file.
#     "ssh_keyfile" : "/path/to/key-id-file"
#     }

# Notes: although there is a Python library for accessing EC2 as a web
# service, it seemed as if it would be less work to just shell out to
# the three EC2 management tools we use.

# To make a distribution we must:

# 1. Fire up an EC2 AMI suitable for building.
# 2. Get any build-dependencies and configurations onto the remote host.
# 3. Fetch the mongodb source.
# 4. Run the package building tools.
# 5. Save the package archives someplace permanent (eventually we
#    ought to install them into a public repository for the distro).
# Unimplemented:
# 6. Fire up an EC2 AMI suitable for testing whether the packages
#    install.
# 7. Check whether the packages install and run.

# The implementations of steps 1, 2, 4, 5, 6, and 7 will depend on the
# distro of host we're talking to (Ubuntu, CentOS, Debian, etc.).

from __future__ import with_statement
import subprocess
import sys
import signal
import getopt
import socket
import time
import os.path
import tempfile

# For the moment, we don't handle any of the errors we raise, so it
# suffices to have a simple subclass of Exception that just
# stringifies according to a desired format.
class SimpleError(Exception):
    def __init__(self, *args):
        self.args = args
    def __str__(self):
        return self.args[0] % self.args[1:]

class SubcommandError(SimpleError):
    def __init__(self, *args):
        self.status = args[2]
        super(SubcommandError, self).__init__(*args)

class BaseConfigurator (object):
    def __init__ (self, **kwargs):
        self.configuration = []
        self.arch=kwargs["arch"]
        self.distro_name=kwargs["distro_name"]
        self.distro_version=kwargs["distro_version"]
        
    def lookup(self,  what, dist, vers, arch):
        for (wht, seq) in self.configuration:
            if what == wht:
                for ((dpat, vpat, apat), payload) in seq:
                    # For the moment, our pattern facility is just "*" or exact match.
                    if ((dist == dpat or dpat == "*") and
                        (vers == vpat or vpat == "*") and
                        (arch == apat or apat == "*")):
                        return payload
        if getattr(self, what, False):
            return getattr(self, what)
        else:
            raise SimpleError("couldn't find a%s %s configuration for dist=%s, version=%s, arch=%s",
                              "n" if ("aeiouAEIOU".find(what[0]) > -1) else "",
                              what, dist, vers, arch)

    def default(self, what):
        return self.lookup(what, self.distro_name, self.distro_version, self.arch)
    def findOrDefault(self, dict, what):
        return (dict[what] if what in dict else self.lookup(what, self.distro_name, self.distro_version, self.arch))

class BaseHostConfigurator (BaseConfigurator):
    def __init__(self, **kwargs):
        super(BaseHostConfigurator, self).__init__(**kwargs)
        self.configuration += [("distro_arch",
                               ((("debian", "*", "x86_64"), "amd64"),
                                (("ubuntu", "*", "x86_64"), "amd64"),
                                (("debian", "*", "x86"), "i386"),
                                (("ubuntu", "*", "x86"), "i386"),
                                (("centos", "*", "x86_64"), "x86_64"),
                                (("fedora", "*", "x86_64"), "x86_64"),
                                (("centos", "*", "x86"), "i386"),
                                (("fedora", "*", "x86"), "i386"),
                                (("*", "*", "x86_64"), "x86_64"),
                                (("*", "*", "x86"), "x86"))) ,
                              ]

class LocalHost(object):
    @classmethod
    def runLocally(cls, argv):
        print "running %s" % argv
        r = subprocess.Popen(argv).wait()
        if r != 0:
            raise SubcommandError("subcommand %s exited %d", argv, r)

class EC2InstanceConfigurator(BaseConfigurator):
    def __init__(self, **kwargs):
        super(EC2InstanceConfigurator, self).__init__(**kwargs)
        self.configuration += [("ec2_ami",
                                ((("ubuntu", "10.4", "x86_64"), "ami-bf07ead6"),
                                 (("ubuntu", "10.4", "x86"), "ami-f707ea9e"),
                                 (("ubuntu", "9.10", "x86_64"), "ami-55739e3c"),
                                 (("ubuntu", "9.10", "x86"), "ami-bb709dd2"),
                                 (("ubuntu", "9.4", "x86_64"), "ami-eef61587"),
                                 (("ubuntu", "9.4", "x86"), "ami-ccf615a5"),
                                 (("ubuntu", "8.10", "x86"), "ami-c0f615a9"),
                                 (("ubuntu", "8.10", "x86_64"), "ami-e2f6158b"),
                                 (("ubuntu", "8.4", "x86"), "ami59b35f30"),
                                 (("ubuntu", "8.4", "x86_64"), "ami-27b35f4e"),
                                 (("debian", "5.0", "x86"), "ami-dcf615b5"),
                                 (("debian", "5.0", "x86_64"), "ami-f0f61599"),
                                 (("centos", "5.4", "x86"), "ami-f8b35e91"),
                                 (("centos", "5.4", "x86_64"), "ami-ccb35ea5"),
                                 (("fedora", "8", "x86_64"), "ami-2547a34c"),
                                 (("fedora", "8", "x86"), "ami-5647a33f"))),
                               ("ec2_mtype",
                                ((("*", "*", "x86"), "m1.small"),
                                 (("*", "*", "x86_64"), "m1.large"))),
                               ]
    

class EC2Instance (object):
    def __init__(self, configurator, **kwargs):
        # Stuff we need to start an instance: AMI name, key and cert
        # files.  AMI and mtype default to configuration in this file,
        # but can be overridden.
        self.ec2_ami   = configurator.findOrDefault(kwargs, "ec2_ami")
        self.ec2_mtype = configurator.findOrDefault(kwargs, "ec2_mtype")

        self.use_internal_name = True if "use_internal_name" in kwargs else False

        # Authentication stuff defaults according to the conventions
        # of the ec2-api-tools.
        self.ec2_cert=kwargs["ec2_cert"]
        self.ec2_pkey=kwargs["ec2_pkey"]
        self.ec2_sshkey=kwargs["ec2_sshkey"]

        # FIXME: this needs to be a commandline option
        self.ec2_groups = ["default", "buildbot-slave", "dist-slave"]
        self.terminate = False if "no_terminate" in kwargs else True

    def parsedesc (self, hdl):
        line1=hdl.readline()
        splitline1=line1.split()
        (_, reservation, unknown1, groupstr) = splitline1[:4]
        groups = groupstr.split(',')
        self.ec2_reservation = reservation
        self.ec2_unknown1 = unknown1
        self.ec2_groups = groups
        # I haven't seen more than 4 data fields in one of these
        # descriptions, but what do I know?
        if len(splitline1)>4:
            print >> sys.stderr, "more than 4 fields in description line 1\n%s\n" % line1
            self.ec2_extras1 = splitline1[4:]
        line2=hdl.readline()
        splitline2=line2.split()
        # The jerks make it tricky to parse line 2: the fields are
        # dependent on the instance's state.
        (_, instance, ami, status_or_hostname) = splitline2[:4]
        self.ec2_instance = instance
        if ami != self.ec2_ami:
            print >> sys.stderr, "warning: AMI in description isn't AMI we invoked\nwe started %s, but got\n%s", (self.ec2_ami, line2)
        # FIXME: are there other non-running statuses?
        if status_or_hostname in ["pending", "terminated"]:
            self.ec2_status = status_or_hostname
            self.ec2_running = False
            index = 4
            self.ec2_storage = splitline2[index+8]
        else:
            self.ec2_running = True
            index = 6
            self.ec2_status = splitline2[5]
            self.ec2_external_hostname = splitline2[3]
            self.ec2_internal_hostname = splitline2[4]
            self.ec2_external_ipaddr = splitline2[index+8]
            self.ec2_internal_ipaddr = splitline2[index+9]
            self.ec2_storage = splitline2[index+10]
        (sshkey, unknown2, mtype, starttime, zone, unknown3, unknown4, monitoring) = splitline2[index:index+8]
        # FIXME: potential disagreement with the supplied sshkey?
        self.ec2_sshkey = sshkey
        self.ec2_unknown2 = unknown2
        # FIXME: potential disagreement with the supplied mtype?
        self.ec2_mtype = mtype
        self.ec2_starttime = starttime
        self.ec2_zone = zone
        self.ec2_unknown3 = unknown3
        self.ec2_unknown4 = unknown4
        self.ec2_monitoring = monitoring

    def start(self):
        "Fire up a fresh EC2 instance."
        groups = reduce(lambda x, y : x+y, [["-g", i] for i in self.ec2_groups], [])
        argv = ["ec2-run-instances",
                self.ec2_ami, "-K", self.ec2_pkey,  "-C", self.ec2_cert,
                "-k", self.ec2_sshkey, "-t", self.ec2_mtype] + groups
        self.ec2_running = False
        print "running %s" % argv
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE)
        try:
            self.parsedesc(proc.stdout)
            if self.ec2_instance == "":
                raise SimpleError("instance id is empty")
            else:
                print "Instance id: %s" % self.ec2_instance
        finally:
            r = proc.wait()
            if r != 0:
                raise SimpleError("ec2-run-instances exited %d", r)

    def initwait(self):
        # poll the instance description until we get a hostname.
        # Note: it seems there can be a time interval after
        # ec2-run-instance finishes during which EC2 will tell us that
        # the instance ID doesn't exist.  This is sort of bad.
        state = "pending"
        numtries = 0
        giveup = 5

        while not self.ec2_running:
            time.sleep(15) # arbitrary
            argv = ["ec2-describe-instances", "-K", self.ec2_pkey, "-C", self.ec2_cert, self.ec2_instance]
            proc = subprocess.Popen(argv, stdout=subprocess.PIPE)
            try:
                self.parsedesc(proc.stdout)
            except Exception, e:
                r = proc.wait()
                if r < giveup:
                    print sys.stderr, str(e)
                    continue
                else:
                    raise SimpleError("ec2-describe-instances exited %d", r)
                numtries+=1

    def stop(self):
        if self.terminate:
            LocalHost.runLocally(["ec2-terminate-instances", "-K", self.ec2_pkey, "-C", self.ec2_cert, self.ec2_instance])
        else:
            print "Not terminating EC2 instance %s." % self.ec2_instance

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, type, value, traceback):
        self.stop()

    def getHostname(self):
        return self.ec2_internal_hostname if self.use_internal_name else self.ec2_external_hostname
        
class SshConnectionConfigurator (BaseConfigurator):
    def __init__(self, **kwargs):
        super(SshConnectionConfigurator, self).__init__(**kwargs)
        self.configuration += [("ssh_login",
                                # FLAW: this actually depends more on the AMI
                                # than the triple.
                                ((("debian", "*", "*"), "root"),
                                 (("ubuntu", "10.4", "*"), "ubuntu"),
                                 (("ubuntu", "9.10", "*"), "ubuntu"),
                                 (("ubuntu", "9.4", "*"), "root"),
                                 (("ubuntu", "8.10", "*"), "root"),
                                 (("ubuntu", "8.4", "*"), "ubuntu"),
                                 (("centos", "*", "*"), "root"))),
                               ]

class SshConnection (object):
    def __init__(self, configurator, **kwargs):
        # Stuff we need to talk to the thing properly
        self.ssh_login = configurator.findOrDefault(kwargs, "ssh_login")

        self.ssh_host = kwargs["ssh_host"]
        self.ssh_keyfile=kwargs["ssh_keyfile"]
        # Gets set to False when we think we can ssh in.
        self.sshwait = True

    def sshWait(self):
        "Poll until somebody's listening on port 22"

        if self.sshwait == False:
            return
        while self.sshwait:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                try:
                    s.connect((self.ssh_host, 22))
                    self.sshwait = False
                    print "connected on port 22 (ssh)"
                    time.sleep(15) # arbitrary timeout, in case the
                                  # remote sshd is slow.
                except socket.error, err:
                    pass
            finally:
                s.close()
                time.sleep(3) # arbitrary timeout

    def initSsh(self):
        self.sshWait()
        ctlpath="/tmp/ec2-ssh-%s-%s-%s" % (self.ssh_host, self.ssh_login, os.getpid())
        argv = ["ssh", "-o", "StrictHostKeyChecking no",
                "-M", "-o", "ControlPath %s" % ctlpath,
                "-v", "-l", self.ssh_login, "-i",  self.ssh_keyfile,
                self.ssh_host]
        print "Setting up ssh master connection with %s" % argv
        self.sshproc = subprocess.Popen(argv)
        self.ctlpath = ctlpath


    def __enter__(self):
        self.initSsh()
        return self
        
    def __exit__(self, type, value, traceback):
        os.kill(self.sshproc.pid, signal.SIGTERM)
        self.sshproc.wait()
        
    def runRemotely(self, argv):
        """Run a command on the host."""
        LocalHost.runLocally(["ssh", "-o", "StrictHostKeyChecking no",
                         "-S", self.ctlpath,
                         "-l", self.ssh_login,
                         "-i",  self.ssh_keyfile,
                         self.ssh_host] + argv)

    def sendFiles(self, files):
        self.sshWait()
        for (localfile, remotefile) in files:
            LocalHost.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-o", "ControlMaster auto",
                             "-o", "ControlPath %s" % self.ctlpath,
                             "-i",  self.ssh_keyfile,
                             "-rv", localfile,
                             self.ssh_login + "@" + self.ssh_host + ":" +
                             ("" if remotefile is None else remotefile) ])

    def recvFiles(self, files):
        self.sshWait()
        print files
        for (remotefile, localfile) in files:
            LocalHost.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-o", "ControlMaster auto",
                             "-o", "ControlPath %s" % self.ctlpath,
                             "-i",  self.ssh_keyfile,
                             "-rv", 
                             self.ssh_login + "@" + self.ssh_host +
                             ":" + remotefile,
                             "." if localfile is None else localfile ])


class ScriptFileConfigurator (BaseConfigurator):
    deb_productdir = "dists"
    rpm_productdir = "/usr/src/redhat/RPMS" # FIXME: this could be
                                            # ~/redhat/RPMS or
                                            # something elsewhere

    preamble_commands = """
set -x # verbose execution, for debugging
set -e # errexit, stop on errors
"""
    # Strictly speaking, we don't need to mangle debian files on rpm
    # systems (and vice versa), but (a) it doesn't hurt anything to do
    # so, and (b) mangling files the same way everywhere could
    # conceivably help uncover bugs in the hideous hideous sed
    # programs we're running here.  (N.B., for POSIX wonks: POSIX sed
    # doesn't support either in-place file editing, which we use
    # below.  So if we end up wanting to run these mangling commands
    # e.g., on a BSD, we'll need to make them fancier.)
    mangle_files_commands ="""
# On debianoids, the package names in the changelog and control file
# must agree, and only files in a subdirectory of debian/ matching the
# package name will get included in the .deb, so we also have to mangle
# the rules file.
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '1s/.*([^)]*)/{pkg_name}{pkg_name_suffix} ({pkg_version})/' debian/changelog ) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's/^Source:.*/Source: {pkg_name}{pkg_name_suffix}/;
s/^Package:.*mongodb/Package: {pkg_name}{pkg_name_suffix}\\
Conflicts: {pkg_name_conflicts}/' debian/control; ) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|$(CURDIR)/debian/mongodb/|$(CURDIR)/debian/{pkg_name}{pkg_name_suffix}/|g' debian/rules) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|debian/mongodb.manpages|debian/{pkg_name}{pkg_name_suffix}.manpages|g' debian/rules) || exit 1
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '/^Name:/s/.*/Name: {pkg_name}{pkg_name_suffix}/; /^Version:/s/.*/Version: {pkg_version}/;' rpm/mongo.spec )
# Debian systems require some ridiculous workarounds to get an init
# script at /etc/init.d/mongodb when the packge name isn't the init
# script name.  Note: dh_installinit --name won't work, because that
# option would require the init script under debian/ to be named
# mongodb.
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" &&
ln debian/init.d debian/{pkg_name}{pkg_name_suffix}.mongodb.init &&
ln debian/mongodb.upstart debian/{pkg_name}{pkg_name_suffix}.mongodb.upstart &&
sed -i 's/dh_installinit/dh_installinit --name=mongodb/' debian/rules) || exit 1
"""

    mangle_files_for_ancient_redhat_commands = """
# Ancient RedHats ship with very old boosts and non-UTF8-aware js
# libraries, so we need to link statically to those.
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|^scons.*((inst)all)|scons --prefix=$RPM_BUILD_ROOT/usr --extralib=nspr4 --staticlib=boost_system-mt,boost_thread-mt,boost_filesystem-mt,boost_program_options-mt,js $1|' rpm/mongo.spec )
"""

    deb_prereq_commands = """
# Configure debconf to never prompt us for input.
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y {pkg_prereq_str}
"""

    deb_build_commands="""
mkdir -p "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}"
mkdir -p "{pkg_product_dir}/{distro_version}/10gen/source"
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}"; debuild ) || exit 1
# Try installing it
dpkg -i *.deb
ps ax | grep mongo || {{ echo "no running mongo" >/dev/stderr; exit 1; }}
cp {pkg_name}{pkg_name_suffix}*.deb "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}"
cp {pkg_name}{pkg_name_suffix}*.dsc "{pkg_product_dir}/{distro_version}/10gen/source"
cp {pkg_name}{pkg_name_suffix}*.tar.gz "{pkg_product_dir}/{distro_version}/10gen/source"
dpkg-scanpackages "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}" /dev/null | gzip -9c > "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}/Packages.gz"
dpkg-scansources "{pkg_product_dir}/{distro_version}/10gen/source" /dev/null | gzip -9c > "{pkg_product_dir}/{distro_version}/10gen/source/Sources.gz"
"""
    rpm_prereq_commands = """
rpm -Uvh http://download.fedora.redhat.com/pub/epel/5/{distro_arch}/epel-release-5-3.noarch.rpm
yum -y install {pkg_prereq_str}
"""
    rpm_build_commands="""
for d in BUILD  BUILDROOT  RPMS  SOURCES  SPECS  SRPMS; do mkdir -p /usr/src/redhat/$d; done
cp -v "{pkg_name}{pkg_name_suffix}-{pkg_version}/rpm/mongo.spec" /usr/src/redhat/SPECS
tar -cpzf /usr/src/redhat/SOURCES/"{pkg_name}{pkg_name_suffix}-{pkg_version}".tar.gz "{pkg_name}{pkg_name_suffix}-{pkg_version}"
rpmbuild -ba /usr/src/redhat/SPECS/mongo.spec
""" 
    # FIXME: this is clean, but adds 40 minutes or so to the build process.
    old_rpm_precommands = """
yum install -y bzip2-devel python-devel libicu-devel chrpath zlib-devel nspr-devel readline-devel ncurses-devel
# FIXME: this is just some random URL found on rpmfind some day in 01/2010.
wget ftp://194.199.20.114/linux/EPEL/5Client/SRPMS/js-1.70-8.el5.src.rpm
rpm -ivh js-1.70-8.el5.src.rpm
sed -i 's/XCFLAGS.*$/XCFLAGS=\"%{{optflags}} -fPIC -DJS_C_STRINGS_ARE_UTF8\" \\\\/' /usr/src/redhat/SPECS/js.spec
rpmbuild -ba /usr/src/redhat/SPECS/js.spec
rpm -Uvh /usr/src/redhat/RPMS/{distro_arch}/js-1.70-8.{distro_arch}.rpm
rpm -Uvh /usr/src/redhat/RPMS/{distro_arch}/js-devel-1.70-8.{distro_arch}.rpm
# FIXME: this is just some random URL found on rpmfind some day in 01/2010.
wget ftp://195.220.108.108/linux/sourceforge/g/project/gr/gridiron2/support-files/FC10%20source%20RPMs/boost-1.38.0-1.fc10.src.rpm
rpm -ivh boost-1.38.0-1.fc10.src.rpm
rpmbuild -ba /usr/src/redhat/SPECS/boost.spec
rpm -ivh /usr/src/redhat/RPMS/{distro_arch}/boost-1.38.0-1.{distro_arch}.rpm
rpm -ivh /usr/src/redhat/RPMS/{distro_arch}/boost-devel-1.38.0-1.{distro_arch}.rpm
"""

    # This horribleness is an attempt to work around ways that you're
    # not really meant to package things for Debian unless you are
    # Debian.

    # On very old Debianoids, libboost-<foo>-dev will be some old
    # boost that's not as thready as we want, but which Eliot says
    # will work.
    very_old_deb_prereqs =  ["libboost-thread-dev", "libboost-filesystem-dev", "libboost-program-options-dev", "libboost-date-time-dev", "libboost-dev", "xulrunner1.9-dev"]

    # On less old (but still old!) Debianoids, libboost-<foo>-dev is
    # still a 1.34, but 1.35 packages are available, so we want those.
    old_deb_prereqs =  ["libboost-thread1.35-dev", "libboost-filesystem1.35-dev", "libboost-program-options1.35-dev", "libboost-date-time1.35-dev", "libboost1.35-dev", "xulrunner-dev"]

    # On newer Debianoids, libbost-<foo>-dev is some sufficiently new
    # thing.
    new_deb_prereqs = [ "libboost-thread-dev", "libboost-filesystem-dev", "libboost-program-options-dev", "libboost-date-time-dev", "libboost-dev", "xulrunner-dev" ]

    common_deb_prereqs = [ "build-essential", "dpkg-dev", "libreadline-dev", "libpcap-dev", "libpcre3-dev", "git-core", "scons", "debhelper", "devscripts", "git-core" ]

    centos_preqres = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git" ]
    fedora_prereqs = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git" ]

    def __init__(self, **kwargs):
        super(ScriptFileConfigurator, self).__init__(**kwargs)
        if kwargs["mongo_version"][0] == 'r':
            self.get_mongo_commands = """
wget -Otarball.tgz "http://github.com/mongodb/mongo/tarball/{mongo_version}";
tar xzf tarball.tgz
mv "`tar tzf tarball.tgz | sed 's|/.*||' | sort -u | head -n1`" "{pkg_name}{pkg_name_suffix}-{pkg_version}"
"""
        else: 
            self.get_mongo_commands = """
git clone git://github.com/mongodb/mongo.git
"""
            if kwargs['mongo_version'][0] == 'v':
                self.get_mongo_commands +="""
( cd mongo && git archive --prefix="{pkg_name}{pkg_name_suffix}-{pkg_version}/" "`git log origin/{mongo_version} | sed -n '1s/^commit //p;q'`" ) | tar xf -
"""
            else:
                self.get_mongo_commands += """
( cd mongo && git archive --prefix="{pkg_name}{pkg_name_suffix}-{pkg_version}/" "{mongo_version}" ) | tar xf -
"""

        if "local_mongo_dir" in kwargs:
            self.mangle_files_commands = """( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && rm -rf debian rpm && cp -pvR ~/pkg/* . )
""" + self.mangle_files_commands

        self.configuration += [("pkg_product_dir",
                                ((("ubuntu", "*", "*"), self.deb_productdir),
                                 (("debian", "*", "*"), self.deb_productdir),
                                 (("fedora", "*", "*"), self.rpm_productdir),
                                 (("centos", "*", "*"), self.rpm_productdir))),
                               ("pkg_prereqs",
                                ((("ubuntu", "9.4", "*"),
                                  self.old_deb_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "9.10", "*"),
                                  self.new_deb_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "10.4", "*"),
                                  self.new_deb_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "8.10", "*"),
                                  self.old_deb_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "8.4", "*"),
                                  self.very_old_deb_prereqs + self.common_deb_prereqs),
                                 (("debian", "5.0", "*"),
                                  self.old_deb_prereqs + self.common_deb_prereqs),
                                 (("fedora", "8", "*"),
                                  self.fedora_prereqs),
                                 (("centos", "5.4", "*"),
                                  self.centos_preqres))),
                               ("commands",
                                ((("debian", "*", "*"),
                                  self.preamble_commands + self.deb_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.deb_build_commands),
                                 (("ubuntu", "*", "*"),
                                  self.preamble_commands + self.deb_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.deb_build_commands),
                                 (("centos", "*", "*"),
                                  self.preamble_commands + self.old_rpm_precommands + self.rpm_prereq_commands + self.get_mongo_commands + self.mangle_files_commands  + self.mangle_files_for_ancient_redhat_commands + self.rpm_build_commands),
                                 (("fedora", "*", "*"),
                                  self.preamble_commands + self.old_rpm_precommands + self.rpm_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.rpm_build_commands))),
                               ("pkg_name",
                                ((("debian", "*", "*"), "mongodb"),
                                 (("ubuntu", "*", "*"), "mongodb"),
                                 (("centos", "*", "*"), "mongo"),

                                 (("fedora", "*", "*"), "mongo")
                                 )),
                               ("pkg_name_conflicts",
                                ((("*", "*", "*"),  ["", "-stable", "-unstable", "-snapshot"]),
                                 ))
                               ]




class ScriptFile(object):
    def __init__(self, configurator, **kwargs):
        self.mongo_version       = kwargs["mongo_version"]
        self.pkg_version        = kwargs["pkg_version"]
        self.pkg_name_suffix    = kwargs["pkg_name_suffix"] if "pkg_name_suffix" in kwargs else ""
        self.pkg_prereqs        = configurator.default("pkg_prereqs")
        self.pkg_name           = configurator.default("pkg_name")
        self.pkg_product_dir    = configurator.default("pkg_product_dir")
        self.pkg_name_conflicts = configurator.default("pkg_name_conflicts") if self.pkg_name_suffix else []
        self.pkg_name_conflicts.remove(self.pkg_name_suffix) if self.pkg_name_suffix and self.pkg_name_suffix in self.pkg_name_conflicts else []
        self.formatter          = configurator.default("commands")
        self.distro_name        = configurator.default("distro_name")
        self.distro_version     = configurator.default("distro_version")
        self.distro_arch        = configurator.default("distro_arch")

    def genscript(self):
        return self.formatter.format(mongo_version=self.mongo_version,
                                     distro_name=self.distro_name,
                                     distro_version=self.distro_version,
                                     distro_arch=self.distro_arch,
                                     pkg_prereq_str=" ".join(self.pkg_prereqs),
                                     pkg_name=self.pkg_name,
                                     pkg_name_suffix=self.pkg_name_suffix,
                                     pkg_version=self.pkg_version,
                                     pkg_product_dir=self.pkg_product_dir,
                                     # KLUDGE: rpm specs and deb
                                     # control files use
                                     # comma-separated conflicts,
                                     # but there's no reason to
                                     # suppose this works elsewhere
                                     pkg_name_conflicts = ", ".join([self.pkg_name+conflict for conflict in self.pkg_name_conflicts])
                                     )

    def __enter__(self):
        self.localscript=None
        # One of tempfile or I is very stupid.
        (fh, name) = tempfile.mkstemp('', "makedist.", ".")
        try:
            pass
        finally:
            os.close(fh)
        with open(name, 'w+') as fh:
            fh.write(self.genscript())
        self.localscript=name
        return self
        
    def __exit__(self, type, value, traceback):
        if self.localscript:
            os.unlink(self.localscript)

class Configurator(SshConnectionConfigurator, EC2InstanceConfigurator, ScriptFileConfigurator, BaseHostConfigurator):
    def __init__(self, **kwargs):
        super(Configurator, self).__init__(**kwargs)

def main():
#    checkEnvironment()

    (kwargs, args) = processArguments()
    (rootdir, distro_name, distro_version, arch, mongo_version_spec) = args[:5]
    # FIXME: there are a few other characters that we can't use in
    # file names on Windows, in case this program really needs to run
    # there.
    distro_name = distro_name.replace('/', '-').replace('\\', '-')
    distro_version = distro_version.replace('/', '-').replace('\\', '-')
    arch = arch.replace('/', '-').replace('\\', '-')     
    try:
        import settings
        if "makedist" in dir ( settings ):
            for key in ["EC2_HOME", "JAVA_HOME"]:
                if key in settings.makedist:
                    os.environ[key] = settings.makedist[key]
            for key in ["ec2_pkey", "ec2_cert", "ec2_sshkey", "ssh_keyfile" ]:
                if key not in kwargs and key in settings.makedist:
                    kwargs[key] = settings.makedist[key]
    except Exception, err:
        print "No settings: %s.  Continuing anyway..." % err
        pass

    # Ensure that PATH contains $EC2_HOME/bin
    vars = ["EC2_HOME", "JAVA_HOME"]
    for var in vars:
        if os.getenv(var) == None:
            raise SimpleError("Environment variable %s is unset; did you create a settings.py?", var)

    if len([True for x in os.environ["PATH"].split(":") if x.find(os.environ["EC2_HOME"]) > -1]) == 0:
        os.environ["PATH"]=os.environ["EC2_HOME"]+"/bin:"+os.environ["PATH"]


    kwargs["distro_name"]    = distro_name
    kwargs["distro_version"] = distro_version
    kwargs["arch"]           = arch

    foo = mongo_version_spec.split(":")
    kwargs["mongo_version"] = foo[0] # this can be a commit id, a
                                     # release id "r1.2.2", or a
                                     # branch name starting with v.
    if len(foo) > 1:
        kwargs["pkg_name_suffix"] = foo[1] 
    if len(foo) > 2 and foo[2]:
        kwargs["pkg_version"] = foo[2]
    else:
        kwargs["pkg_version"] = time.strftime("%Y%m%d")

    # FIXME: this should also include the mongo version or something.
    if "subdirs" in kwargs:
        kwargs["localdir"] = "%s/%s/%s/%s" % (rootdir, distro_name, distro_version, arch, kwargs["mongo_version"])
    else:
        kwargs["localdir"] = rootdir

    if "pkg_name_suffix" not in kwargs:
        if kwargs["mongo_version"][0] in ["r", "v"]:
            nums = kwargs["mongo_version"].split(".")
            if int(nums[1]) % 2 == 0:
                kwargs["pkg_name_suffix"] = "-stable"
            else:
                kwargs["pkg_name_suffix"] = "-unstable"
        else:
            kwargs["pkg_name_suffix"] = ""


    kwargs['local_gpg_dir'] = kwargs["local_gpg_dir"] if "local_gpg_dir" in kwargs else os.path.expanduser("~/.gnupg") 
    configurator = Configurator(**kwargs)
    LocalHost.runLocally(["mkdir", "-p", kwargs["localdir"]])
    with ScriptFile(configurator, **kwargs) as script:
        with open(script.localscript) as f:
            print """# Going to run the following on a fresh AMI:"""
            print f.read()
            time.sleep(10)
        with EC2Instance(configurator, **kwargs) as ec2:
            ec2.initwait()
            kwargs["ssh_host"] = ec2.getHostname()
            with SshConnection(configurator, **kwargs) as ssh:
                ssh.runRemotely(["uname -a; ls /"])
                ssh.runRemotely(["mkdir", "pkg"])
                if "local_mongo_dir" in kwargs:
                    ssh.sendFiles([(kwargs["local_mongo_dir"]+'/'+d, "pkg") for d in ["rpm", "debian"]])
                ssh.sendFiles([(kwargs['local_gpg_dir'], ".gnupg")])
                ssh.sendFiles([(script.localscript, "makedist.sh")])
                ssh.runRemotely((["sudo"] if ssh.ssh_login != "root" else [])+ ["sh", "makedist.sh"])
                ssh.recvFiles([(script.pkg_product_dir, kwargs['localdir'])])

def processArguments():
    # flagspec [ (short, long, argument?, description, argname)* ]
    flagspec = [ ("?", "usage", False, "Print a (useless) usage message", None),
                 ("h", "help", False, "Print a help message and exit", None),
                 ("N", "no-terminate", False, "Leave the EC2 instance running at the end of the job", None),
                 ("S", "subdirs", False, "Create subdirectories of the output directory based on distro name, version, and architecture", None),
                 ("I", "use-internal-name", False, "Use the EC2 internal hostname for sshing", None),
                 (None, "local-gpg-dir", True, "Local directory of gpg junk", "STRING"),
                 (None, "local-mongo-dir", True, "Copy packaging files from local mongo checkout", "DIRECTORY"),
                 ]
    shortopts = "".join([t[0] + (":" if t[2] else "") for t in flagspec if t[0] is not None])
    longopts = [t[1] + ("=" if t[2] else "") for t in flagspec]

    try:
        opts, args = getopt.getopt(sys.argv[1:], shortopts, longopts)
    except getopt.GetoptError, err:
        print str(err)
        sys.exit(2)

    # Normalize the getopt-parsed options.
    kwargs = {}
    for (opt, arg) in opts:
        flag = opt
        opt = opt.lstrip("-")
        if flag[:2] == '--': #long opt
            kwargs[opt.replace('-', '_')] = arg
        elif flag[:1] == "-": #short opt 
            ok = False
            for tuple in flagspec:
                if tuple[0] == opt:
                    ok = True
                    kwargs[tuple[1].replace('-', '_')] = arg
                    break
            if not ok:
                raise SimpleError("this shouldn't happen: unrecognized option flag: %s", opt)
        else:
            raise SimpleError("this shouldn't happen: non-option returned from getopt()")
        
    if "help" in kwargs:
        print "Usage: %s [OPTIONS] DIRECTORY DISTRO DISTRO-VERSION ARCHITECTURE MONGO-VERSION-SPEC" % sys.argv[0]
        print """Build some packages on new EC2 AMI instances, leave packages under DIRECTORY.

MONGO-VERSION-SPEC has the syntax
Commit(:Pkg-Name-Suffix(:Pkg-Version)).  If Commit starts with an 'r',
build from a tagged release; if Commit starts with a 'v', build from
the HEAD of a version branch; otherwise, build whatever git commit is
identified by Commit.  Pkg-Name-Suffix gets appended to the package
name, and defaults to "-stable" and "-unstable" if Commit looks like
it designates a stable or unstable release/branch, respectively.
Pkg-Version is used as the package version, and defaults to YYYYMMDD.
Examples:

  HEAD             # build a snapshot of HEAD, name the package
                   # "mongodb", use YYYYMMDD for the version

  HEAD:-snap     # build a snapshot of HEAD, name the package
                 # "mongodb-snap", use YYYYMMDD for the version

  HEAD:-snap:123     # build a snapshot of HEAD, name the package
                     # "mongodb-snap", use 123 for the version

  HEAD:-suffix:1.3 # build a snapshot of HEAD, name the package
                   # "mongodb-snapshot", use "1.3 for the version

  r1.2.3           # build a package of the 1.2.3 release, call it "mongodb-stable",
                   # make the package version YYYYMMDD.

  v1.2:-stable:    # build a package of the HEAD of the 1.2 branch

  decafbad:-foo:123 # build git commit "decafbad", call the package
                    # "mongodb-foo" with package version 123.

Options:"""
        for t in flagspec:
            print "%-20s\t%s." % ("%4s--%s%s:" % ("-%s, " % t[0] if t[0] else "", t[1], ("="+t[4]) if t[4] else ""), t[3])
        print """
Mandatory arguments to long options are also mandatory for short
options.  Some EC2 arguments default to (and override) environment
variables; see the ec2-api-tools documentation."""
        sys.exit(0)

    if "usage" in kwargs:
        print "Usage: %s [OPTIONS] OUTPUT-DIR DISTRO-NAME DISTRO-VERSION ARCHITECTURE MONGO-VERSION-SPEC" % sys.argv[0]
        sys.exit(0)


    return (kwargs, args)


if __name__ == "__main__":
    main()

# Examples:

# ./makedist.py --local-gpg-dir=$HOME/10gen/dst/dist-gnupg /tmp/ubuntu ubuntu 8.10 x86_64 HEAD:-snapshot
