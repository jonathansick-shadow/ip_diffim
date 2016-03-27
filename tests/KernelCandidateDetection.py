#!/usr/bin/env python
import os
import sys
import unittest
import lsst.utils.tests as tests

import lsst.utils
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.ip.diffim.diffimTools as diffimTools
import lsst.pex.logging as pexLog
import lsst.pex.config as pexConfig

pexLog.Trace_setVerbosity('lsst.ip.diffim', 3)


class DiffimTestCases(unittest.TestCase):

    def setUp(self):
        self.config = ipDiffim.ImagePsfMatchTask.ConfigClass()
        self.subconfig = self.config.kernel.active
        self.policy = pexConfig.makePolicy(self.subconfig)
        self.kSize = self.policy.getInt('kernelSize')

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
        del self.config
        del self.policy
        del self.gaussFunction
        del self.gaussKernel
        if self.defDataDir:
            del self.templateImage
            del self.scienceImage

    def testGetCollection(self):
        if not self.defDataDir:
            print >> sys.stderr, "Warning: afwdata is not set up; not running KernelCandidateDetection.py"
            return

        # NOTE - you need to subtract off background from the image
        # you run detection on.  Here it is the template.
        bgConfig = self.subconfig.afwBackgroundConfig
        diffimTools.backgroundSubtract(bgConfig, [self.templateImage, ])

        detConfig = self.subconfig.detectionConfig
        maskPlane = detConfig.badMaskPlanes[0]
        maskVal = afwImage.MaskU.getPlaneBitMask(maskPlane)

        kcDetect = ipDiffim.KernelCandidateDetectionF(pexConfig.makePolicy(detConfig))
        kcDetect.apply(self.templateImage, self.scienceImage)
        fpList1 = kcDetect.getFootprints()

        self.assertTrue(len(fpList1) != 0)

        for fp in fpList1:
            bbox = fp.getBBox()
            tmi = afwImage.MaskedImageF(self.templateImage, bbox, afwImage.LOCAL)
            smi = afwImage.MaskedImageF(self.scienceImage, bbox, afwImage.LOCAL)
            tmask = tmi.getMask()
            smask = smi.getMask()

            for j in range(tmask.getHeight()):
                for i in range(tmask.getWidth()):
                    # No masked pixels in either image
                    self.assertEqual(tmask.get(i, j), 0)
                    self.assertEqual(smask.get(i, j), 0)

        # add a masked pixel to the template image and make sure you don't get it
        afwImage.MaskedImageF(self.templateImage, fpList1[0].getBBox(), afwImage.LOCAL).getMask().set(
            tmask.getWidth()//2, tmask.getHeight()//2, maskVal)
        kcDetect.apply(self.templateImage, self.scienceImage)
        fpList2 = kcDetect.getFootprints()
        self.assertTrue(len(fpList2) == (len(fpList1)-1))

        # add a masked pixel to the science image and make sure you don't get it
        afwImage.MaskedImageF(self.scienceImage, fpList1[1].getBBox(), afwImage.LOCAL).getMask().set(
            smask.getWidth()//2, smask.getHeight()//2, maskVal)
        afwImage.MaskedImageF(self.scienceImage, fpList1[2].getBBox(), afwImage.LOCAL).getMask().set(
            smask.getWidth()//2, smask.getHeight()//2, maskVal)
        kcDetect.apply(self.templateImage, self.scienceImage)
        fpList3 = kcDetect.getFootprints()
        self.assertTrue(len(fpList3) == (len(fpList1)-3))

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
