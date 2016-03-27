#!/usr/bin/env python

#
# LSST Data Management System
# Copyright 2008, 2009, 2010 LSST Corporation.
#
# This product includes software developed by the
# LSST Project (http://www.lsst.org/).
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the LSST License Statement and
# the GNU General Public License along with this program.  If not,
# see <http://www.lsstcorp.org/LegalNotices/>.
#

import os
import sys
import unittest
import lsst.utils.tests as tests

import lsst.utils
import lsst.afw.geom as afwGeom
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.pex.logging as logging

verbosity = 4
logging.Trace_setVerbosity('lsst.ip.diffim', verbosity)

# This one tests convolve and subtract


class DiffimTestCases(unittest.TestCase):

    # D = I - (K.x.T + bg)

    def setUp(self):
        self.config = ipDiffim.ImagePsfMatchTask.ConfigClass()
        self.config.kernel.name = "AL"
        self.subconfig = self.config.kernel.active
        self.kSize = self.subconfig.kernelSize

        # gaussian reference kernel
        self.gSize = self.kSize
        self.gaussFunction = afwMath.GaussianFunction2D(2, 3)
        self.gaussKernel = afwMath.AnalyticKernel(self.gSize, self.gSize, self.gaussFunction)

        # known input images
        try:
            self.defDataDir = lsst.utils.getPackageDir('afwdata')
        except Exception:
            self.defDataDir = None

        if self.defDataDir:
            defImagePath = os.path.join(self.defDataDir, "DC3a-Sim", "sci", "v5-e0",
                                        "v5-e0-c011-a00.sci.fits")
            self.templateImage = afwImage.MaskedImageF(defImagePath)
            self.scienceImage = self.templateImage.Factory(self.templateImage.getDimensions())

            afwMath.convolve(self.scienceImage, self.templateImage, self.gaussKernel, False)

    def tearDown(self):
        del self.subconfig
        del self.gaussFunction
        del self.gaussKernel
        if self.defDataDir:
            del self.templateImage
            del self.scienceImage

    def runConvolveAndSubtract1(self, bgVal = 0, xloc = 408, yloc = 580):
        imsize = int(5 * self.kSize)

        p0 = afwGeom.Point2I(xloc - imsize/2, yloc - imsize/2)
        p1 = afwGeom.Point2I(xloc + imsize/2, yloc + imsize/2)
        bbox = afwGeom.Box2I(p0, p1)

        tmi = afwImage.MaskedImageF(self.templateImage, bbox, afwImage.LOCAL)
        smi = afwImage.MaskedImageF(self.scienceImage, bbox, afwImage.LOCAL)
        diffIm = ipDiffim.convolveAndSubtract(tmi, smi, self.gaussKernel, bgVal)

        bbox = self.gaussKernel.shrinkBBox(diffIm.getBBox(afwImage.LOCAL))
        diffIm2 = afwImage.MaskedImageF(diffIm, bbox, afwImage.LOCAL)

        # image is empty (or the additional background you subtracted off)
        for j in range(diffIm2.getHeight()):
            for i in range(diffIm2.getWidth()):
                self.assertAlmostEqual(diffIm2.getImage().get(i, j), -1.*bgVal, 3)

    def runConvolveAndSubtract2(self, bgOrder=0, xloc = 408, yloc = 580):
        imsize = int(5 * self.kSize)

        p0 = afwGeom.Point2I(xloc - imsize/2, yloc - imsize/2)
        p1 = afwGeom.Point2I(xloc + imsize/2, yloc + imsize/2)
        bbox = afwGeom.Box2I(p0, p1)

        tmi = afwImage.MaskedImageF(self.templateImage, bbox, afwImage.LOCAL)
        smi = afwImage.MaskedImageF(self.scienceImage, bbox, afwImage.LOCAL)
        bgFunc = afwMath.PolynomialFunction2D(bgOrder)  # coeffs are 0. by default
        diffIm = ipDiffim.convolveAndSubtract(tmi, smi, self.gaussKernel, bgFunc)

        bbox = self.gaussKernel.shrinkBBox(diffIm.getBBox(afwImage.LOCAL))
        diffIm2 = afwImage.MaskedImageF(diffIm, bbox, afwImage.LOCAL)
        for j in range(diffIm2.getHeight()):
            for i in range(diffIm2.getWidth()):
                self.assertAlmostEqual(diffIm2.getImage().get(i, j), 0., 4)

    def testConvolveAndSubtract(self):
        if not self.defDataDir:
            print >> sys.stderr, "Warning: afwdata is not set up"
            return

        self.runConvolveAndSubtract1(bgVal=0)
        self.runConvolveAndSubtract1(bgVal=10)
        # this one uses a function
        self.runConvolveAndSubtract2(bgOrder=0)
        self.runConvolveAndSubtract2(bgOrder=2)

#####


def suite():
    """Returns a suite containing all the test cases in this module."""
    tests.init()

    suites = []
    suites += unittest.makeSuite(DiffimTestCases)
    suites += unittest.makeSuite(tests.MemoryTestCase)
    return unittest.TestSuite(suites)


def run(doExit=False):
    """Run the tests"""
    tests.run(suite(), doExit)

if __name__ == "__main__":
    run(True)
