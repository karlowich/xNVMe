#!/usr/bin/env python3
"""
    Produce a binary Debian package from the install-plan in meson builddir

    This is implemented as a quick'n'dirty replacement for the CMake/CPack
    feature.
"""
import subprocess
import argparse
import shutil
import pprint
import pathlib
import stat
import json
import sys
import os


def expand_path(path):
    """Expands variables from the given path and turns it into absolute path"""

    return os.path.abspath(os.path.expanduser(os.path.expandvars(path)))


def parse_args():
    """Parse arguments for archiver"""

    prsr = argparse.ArgumentParser(
        description='Create a binary Debian package from meson install-plan',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    prsr.add_argument(
        '--builddir',
        help='Path to meson builddir',
        required=True,
        type=str
    )
    prsr.add_argument(
        '--workdir',
        help='Path to workdir',
        required=True,
        type=str
    )
    prsr.add_argument(
        '--output',
        help='Path to directory in which to store deb-package',
        type=str,
        default=os.path.join(expand_path('.'))
    )

    prsr.add_argument(
        '--deb-package',
        default='noname',
        help='Debian control-file property',
    )
    prsr.add_argument(
        '--deb-version',
        default='0.0.1',
        help='Debian control-file property',
    )
    prsr.add_argument(
        '--deb-architecture',
        default='amd64',
        help='Debian control-file property',
    )
    prsr.add_argument(
        '--deb-maintainer',
        default='Mr. Robot',
        help='Debian control-file property',
    )
    prsr.add_argument(
        '--deb-description',
        default='Something great',
        help='Debian control-file property',
    )

    return prsr.parse_args()


def main(args):
    """Produce library bundle"""

    args.builddir = expand_path(args.builddir)
    args.workdir = expand_path(args.workdir)
    args.output = expand_path(args.output)

    deb_dir = os.path.join(args.workdir, 'DEBIAN')
    control_file = os.path.join(deb_dir, 'control')
    os.makedirs(deb_dir)

    meson_install_plan_path = os.path.join(
        args.builddir, 'meson-info', 'intro-install_plan.json'
    )

    with open(meson_install_plan_path) as pfd:
        plan = json.load(pfd)

    prefix = 'usr'
    layout = {
        'includedir': os.path.join(args.workdir, prefix, 'include'),
        'libdir_shared': os.path.join(args.workdir, prefix, 'lib'),
        'libdir_static': os.path.join(args.workdir, prefix, 'lib'),
        'libdir': os.path.join(args.workdir, prefix, 'lib'),
        'bindir': os.path.join(args.workdir, prefix, 'bin'),
        'datadir': os.path.join(args.workdir, prefix),
        'prefix': os.path.join(args.workdir, prefix),
    }

    # Emit the control file
    adict = vars(args)
    with open(control_file, 'w') as cfd:
        cfd.write("Package: {deb_package}\n".format(**adict))
        cfd.write("Version: {deb_version}\n".format(**adict))
        cfd.write("Architecture: {deb_architecture}\n".format(**adict))
        cfd.write("Maintainer: {deb_maintainer}\n".format(**adict))
        cfd.write("Description: {deb_description}\n".format(**adict))

    # Populate bin-deb according to install-plan
    for section, stuff in sorted(plan.items()):
        for src_path, spec in stuff.items():
            dst_fmt = spec.get("destination", None)
            if dst_fmt is None:
                continue

            dst_path = dst_fmt.format(**layout)
            if not dst_path.startswith(args.workdir):
                print("Malformat: %s" % dst_path)
                continue

            dst_dir = os.path.dirname(dst_path)
            if not os.path.exists(dst_dir):
                os.makedirs(dst_dir)
            shutil.copyfile(src_path, dst_path)

            if '{bindir}' in dst_fmt or '/bin' in dst_fmt:
                f = pathlib.Path(dst_path)
                f.chmod(f.stat().st_mode | stat.S_IEXEC)

    # Run dpkg-deb to create the package
    cmd = [
        'dpkg-deb',
        '--build',
        '--root-owner-group',
        args.workdir,
        args.output
    ]
    proc = subprocess.Popen(
        cmd, stdout = subprocess.PIPE, stderr = subprocess.PIPE
    )
    out, err = proc.communicate()
    ret = proc.returncode

    if ret:
        print(out, err)

    return ret


if __name__ == "__main__":
    sys.exit(main(parse_args()))
