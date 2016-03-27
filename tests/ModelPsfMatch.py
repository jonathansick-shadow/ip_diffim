#!/usr/bin/env python
import unittest
import lsst.utils.tests as tests
import lsst.afw.geom as afwGeom
import lsst.afw.image as afwImage
import lsst.ip.diffim as ipDiffim
import lsst.meas.algorithms as measAlg

import lsst.pex.logging as pexLog
pexLog.Trace_setVerbosity('lsst.ip.diffim', 5)


class PsfMatchTestCases(unittest.TestCase):

    def setUp(self):
        self.config = ipDiffim.ModelPsfMatchTask.ConfigClass()
        self.subconfig = self.config.kernel.active
        self.subconfig.scaleByFwhm = False
        self.subconfig.kernelSize = 9
        self.subconfig.kernelSizeMin = 9

        self.imsize = 2 * self.subconfig.sizeCellX
        self.ksize = 11
        self.sigma1 = 2.0
        self.sigma2 = 3.7
        self.exp = afwImage.ExposureF(afwGeom.Extent2I(self.imsize, self.imsize))
        self.exp.setPsf(measAlg.DoubleGaussianPsf(self.ksize, self.ksize, self.sigma1))

    def testTooBig(self):
        self.subconfig.kernelSize = self.ksize
        psf = measAlg.DoubleGaussianPsf(self.ksize, self.ksize, self.sigma2)
        psfMatch = ipDiffim.ModelPsfMatchTask(config=self.config)
        try:
            psfMatch.run(self.exp, psf)
        except Exception:
            pass
        else:
            self.fail()

    def testMatch(self):
        for order in (0, 1):
            for ksum in (0.5, 1.0, 2.7):
                self.runMatch(kOrder = order, kSumIn = ksum)

    def runMatch(self, kOrder = 0, kSumIn = 3.7):
        self.subconfig.spatialKernelOrder = kOrder

        psf = measAlg.DoubleGaussianPsf(self.ksize, self.ksize, self.sigma2)
        psfMatch = ipDiffim.ModelPsfMatchTask(config=self.config)
        results = psfMatch.run(self.exp, psf, kernelSum = kSumIn)

        matchingKernel = results.psfMatchingKernel

        kImage = afwImage.ImageD(matchingKernel.getDimensions())
        kSumOut = matchingKernel.computeImage(kImage, False)

        self.assertAlmostEqual(kSumIn, kSumOut)

    def tearDown(self):
        del self.exp
        del self.subconfig


def suite():
    """Returns a suite containing all the test cases in this module."""
    tests.init()

    suites = []
    suites += unittest.makeSuite(PsfMatchTestCases)
    suites += unittest.makeSuite(tests.MemoryTestCase)
    return unittest.TestSuite(suites)


def run(doExit=False):
    """Run the tests"""
    tests.run(suite(), doExit)

if __name__ == "__main__":
    run(True)
