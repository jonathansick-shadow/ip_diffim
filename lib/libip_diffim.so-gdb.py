import os.path
import sys
import lsst.utils
import gdb
#
# Adjust the load path to include lsst.gdb, bypassing the regular lsstimport mechanism as
# the version of python running within gdb may not be the same as we are using for lsst processing
#
ipDiffimDir = lsst.utils.getPackageDir('ip_diffim')
printerDir = os.path.join(ipDiffimDir, "python", "lsst", "gdb")
if printerDir not in sys.path:
    sys.path.append(printerDir)

import ip.diffim.printers

ip.diffim.printers.register(gdb.current_objfile())
