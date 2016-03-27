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


import unittest
import lsst.utils.tests as tests

import lsst.afw.geom as afwGeom
import lsst.afw.image as afwImage
import lsst.ip.diffim as ipDiffim
import lsst.pex.logging as logging

verbosity = 1
logging.Trace_setVerbosity('lsst.ip.diffim', verbosity)


class DiffimTestCases(unittest.TestCase):

    def setUp(self):
        pass

    def tearDown(self):
        pass

    def testNoMask(self):
        mask = afwImage.MaskU(afwGeom.Extent2I(20, 20))
        mask.set(0)
        fsb = ipDiffim.FindSetBitsU()

        bbox = afwGeom.Box2I(afwGeom.Point2I(0, 10),
                             afwGeom.Point2I(9, 12))
        fsb.apply(afwImage.MaskU(mask, bbox, afwImage.LOCAL))

        self.assertEqual(fsb.getBits(), 0)

    def testOneMask(self):
        mask = afwImage.MaskU(afwGeom.Extent2I(20, 20))
        mask.set(0)
        bitmaskBad = mask.getPlaneBitMask('BAD')
        fsb = ipDiffim.FindSetBitsU()

        bbox = afwGeom.Box2I(afwGeom.Point2I(9, 10),
                             afwGeom.Point2I(11, 12))
        submask = afwImage.MaskU(mask, bbox, afwImage.LOCAL)
        submask |= bitmaskBad

        bbox2 = afwGeom.Box2I(afwGeom.Point2I(8, 8),
                              afwGeom.Point2I(19, 19))
        fsb.apply(afwImage.MaskU(mask, bbox2, afwImage.LOCAL))

        self.assertEqual(fsb.getBits(), bitmaskBad)

    def testManyMask(self):
        mask = afwImage.MaskU(afwGeom.Extent2I(20, 20))
        mask.set(0)
        bitmaskBad = mask.getPlaneBitMask('BAD')
        bitmaskSat = mask.getPlaneBitMask('SAT')
        fsb = ipDiffim.FindSetBitsU()

        bbox = afwGeom.Box2I(afwGeom.Point2I(9, 10),
                             afwGeom.Point2I(11, 12))
        submask = afwImage.MaskU(mask, bbox, afwImage.LOCAL)
        submask |= bitmaskBad

        bbox2 = afwGeom.Box2I(afwGeom.Point2I(8, 8),
                              afwGeom.Point2I(19, 19))
        submask2 = afwImage.MaskU(mask, bbox2, afwImage.LOCAL)
        submask2 |= bitmaskSat

        bbox3 = afwGeom.Box2I(afwGeom.Point2I(0, 0),
                              afwGeom.Point2I(19, 19))
        fsb.apply(afwImage.MaskU(mask, bbox3, afwImage.LOCAL))

        self.assertEqual(fsb.getBits(), bitmaskBad | bitmaskSat)

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
