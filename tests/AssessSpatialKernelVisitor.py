#!/usr/bin/env python
import unittest
import numpy as num

import lsst.utils.tests as tests

import lsst.afw.geom as afwGeom
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.pex.logging as pexLog
import lsst.pex.config as pexConfig

#import lsst.afw.display.ds9 as ds9

pexLog.Trace_setVerbosity('lsst.ip.diffim', 5)


class DiffimTestCases(unittest.TestCase):

    def setUp(self):
        self.config = ipDiffim.ImagePsfMatchTask.ConfigClass()
        self.config.kernel.name = "AL"
        self.subconfig = self.config.kernel.active

        self.policy = pexConfig.makePolicy(self.subconfig)
        self.kList = ipDiffim.makeKernelBasisList(self.subconfig)

        self.ksize = self.policy.get('kernelSize')

    def makeSpatialKernel(self, order):
        basicGaussian1 = afwMath.GaussianFunction2D(2., 2., 0.)
        basicKernel1 = afwMath.AnalyticKernel(self.ksize, self.ksize, basicGaussian1)

        basicGaussian2 = afwMath.GaussianFunction2D(5., 3., 0.5 * num.pi)
        basicKernel2 = afwMath.AnalyticKernel(self.ksize, self.ksize, basicGaussian2)

        basisList = afwMath.KernelList()
        basisList.append(basicKernel1)
        basisList.append(basicKernel2)
        basisList = afwMath.KernelList(ipDiffim.renormalizeKernelList(basisList))

        spatialKernelFunction = afwMath.PolynomialFunction2D(order)
        spatialKernel = afwMath.LinearCombinationKernel(basisList, spatialKernelFunction)
        kCoeffs = [[0.0 for x in range(1, spatialKernelFunction.getNParameters()+1)],
                   [0.01 * x for x in range(1, spatialKernelFunction.getNParameters()+1)]]
        kCoeffs[0][0] = 1.0  # it does not vary spatially; constant across image
        spatialKernel.setSpatialParameters(kCoeffs)
        return spatialKernel

    def tearDown(self):
        del self.policy
        del self.kList

    def testGood(self):
        ti = afwImage.MaskedImageF(afwGeom.Extent2I(100, 100))
        ti.getVariance().set(0.1)
        ti.set(50, 50, (1., 0x0, 1.))
        sKernel = self.makeSpatialKernel(2)
        si = afwImage.MaskedImageF(ti.getDimensions())
        afwMath.convolve(si, ti, sKernel, True)

        bbox = afwGeom.Box2I(afwGeom.Point2I(25, 25),
                             afwGeom.Point2I(75, 75))
        si = afwImage.MaskedImageF(si, bbox, afwImage.LOCAL)
        ti = afwImage.MaskedImageF(ti, bbox, afwImage.LOCAL)
        kc = ipDiffim.KernelCandidateF(50., 50., ti, si, self.policy)

        sBg = afwMath.PolynomialFunction2D(1)
        bgCoeffs = [0., 0., 0.]
        sBg.setParameters(bgCoeffs)

        # must be initialized
        bskv = ipDiffim.BuildSingleKernelVisitorF(self.kList, self.policy)
        bskv.processCandidate(kc)
        self.assertEqual(kc.isInitialized(), True)
        #ds9.mtv(kc.getTemplateMaskedImage(), frame=1)
        #ds9.mtv(kc.getScienceMaskedImage(), frame=2)
        #ds9.mtv(kc.getKernelImage(ipDiffim.KernelCandidateF.RECENT), frame=3)
        #ds9.mtv(kc.getDifferenceImage(ipDiffim.KernelCandidateF.RECENT), frame=4)

        askv = ipDiffim.AssessSpatialKernelVisitorF(sKernel, sBg, self.policy)
        askv.processCandidate(kc)

        self.assertEqual(askv.getNProcessed(), 1)
        self.assertEqual(askv.getNRejected(), 0)
        self.assertEqual(kc.getStatus(), afwMath.SpatialCellCandidate.GOOD)

    def testBad(self):
        ti = afwImage.MaskedImageF(afwGeom.Extent2I(100, 100))
        ti.getVariance().set(0.1)
        ti.set(50, 50, (1., 0x0, 1.))
        sKernel = self.makeSpatialKernel(2)
        si = afwImage.MaskedImageF(ti.getDimensions())
        afwMath.convolve(si, ti, sKernel, True)

        bbox = afwGeom.Box2I(afwGeom.Point2I(25, 25),
                             afwGeom.Point2I(75, 75))
        si = afwImage.MaskedImageF(si, bbox, afwImage.LOCAL)
        ti = afwImage.MaskedImageF(ti, bbox, afwImage.LOCAL)
        kc = ipDiffim.KernelCandidateF(50., 50., ti, si, self.policy)

        badGaussian = afwMath.GaussianFunction2D(1., 1., 0.)
        badKernel = afwMath.AnalyticKernel(self.ksize, self.ksize, badGaussian)
        basisList = afwMath.KernelList()
        basisList.append(badKernel)
        badSpatialKernelFunction = afwMath.PolynomialFunction2D(0)
        badSpatialKernel = afwMath.LinearCombinationKernel(basisList, badSpatialKernelFunction)
        badSpatialKernel.setSpatialParameters([[1, ]])

        sBg = afwMath.PolynomialFunction2D(1)
        bgCoeffs = [10., 10., 10.]
        sBg.setParameters(bgCoeffs)

        # must be initialized
        bskv = ipDiffim.BuildSingleKernelVisitorF(self.kList, self.policy)
        bskv.processCandidate(kc)
        self.assertEqual(kc.isInitialized(), True)

        askv = ipDiffim.AssessSpatialKernelVisitorF(badSpatialKernel, sBg, self.policy)
        askv.processCandidate(kc)

        self.assertEqual(askv.getNProcessed(), 1)
        self.assertEqual(askv.getNRejected(), 1)
        self.assertEqual(kc.getStatus(), afwMath.SpatialCellCandidate.BAD)


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
