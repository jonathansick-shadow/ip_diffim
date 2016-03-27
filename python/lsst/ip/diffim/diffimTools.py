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

# python
import time
import os
from collections import Counter
import numpy as np

# all the c++ level classes and routines
import diffimLib

# all the other LSST packages
import lsst.afw.geom as afwGeom
import lsst.afw.image as afwImage
import lsst.afw.table as afwTable
import lsst.afw.detection as afwDetect
import lsst.afw.math.mathLib as afwMath
import lsst.pex.logging as pexLog
import lsst.pex.config as pexConfig
from .makeKernelBasisList import makeKernelBasisList

# Helper functions for ipDiffim; mostly viewing of results and writing
# debugging info to disk.

#######
# Add noise
#######


def makeFlatNoiseImage(mi, seedStat = afwMath.MAX):
    img = mi.getImage()
    seed = int(10. * afwMath.makeStatistics(mi.getImage(), seedStat).getValue() + 1)
    rdm = afwMath.Random(afwMath.Random.MT19937, seed)
    rdmImage = img.Factory(img.getDimensions())
    afwMath.randomGaussianImage(rdmImage, rdm)
    return rdmImage


def makePoissonNoiseImage(im):
    """Return a Poisson noise image based on im

    Uses numpy.random; you may wish to call numpy.random.seed first.

    @warning This uses an undocumented numpy API (the documented API
    uses a single float expectation value instead of an array).

    @param[in] im image; the output image has the same dimensions and shape
        and its expectation value is the value of im at each pixel
    """
    import numpy.random as rand
    imArr = im.getArray()
    noiseIm = im.Factory(im.getBBox())
    noiseArr = noiseIm.getArray()

    intNoiseArr = rand.poisson(imArr)
    noiseArr[:, :] = intNoiseArr.astype(noiseArr.dtype)
    return noiseIm

#######
# Make fake images for testing; one is a delta function (or narrow
# gaussian) and the other is a convolution of this with a spatially
# varying kernel.
#######


def fakeCoeffs():
    kCoeffs = ((1.0, 0.0, 0.0),
               (0.005, -0.000001, 0.000001),
               (0.005, 0.000004, 0.000004),
               (-0.001, -0.000030, 0.000030),
               (-0.001, 0.000015, 0.000015),
               (-0.005, -0.000050, 0.000050))
    return kCoeffs


def makeFakeKernelSet(sizeCell = 128, nCell = 3,
                      deltaFunctionCounts = 1.e4, tGaussianWidth = 1.0,
                      addNoise = True, bgValue = 100., display = False):

    from . import imagePsfMatch
    configFake = imagePsfMatch.ImagePsfMatchConfig()
    configFake.kernel.name = "AL"
    subconfigFake = configFake.kernel.active
    subconfigFake.alardNGauss = 1
    subconfigFake.alardSigGauss = [2.5, ]
    subconfigFake.alardDegGauss = [2, ]
    subconfigFake.sizeCellX = sizeCell
    subconfigFake.sizeCellY = sizeCell
    subconfigFake.spatialKernelOrder = 1
    subconfigFake.spatialModelType = "polynomial"
    subconfigFake.singleKernelClipping = False   # variance is a hack
    subconfigFake.spatialKernelClipping = False  # variance is a hack
    if bgValue > 0.0:
        subconfigFake.fitForBackground = True

    policyFake = pexConfig.makePolicy(subconfigFake)

    basisList = makeKernelBasisList(subconfigFake)
    kSize = subconfigFake.kernelSize

    # This sets the final extent of each convolved delta function
    gaussKernelWidth = sizeCell // 2

    # This sets the scale over which pixels are correlated in the
    # spatial convolution; should be at least as big as the kernel you
    # are trying to fit for
    spatialKernelWidth = kSize

    # Number of bad pixels due to convolutions
    border = (gaussKernelWidth + spatialKernelWidth)//2

    # Make a fake image with a matrix of delta functions
    totalSize = nCell * sizeCell + 2 * border
    tim = afwImage.ImageF(afwGeom.Extent2I(totalSize, totalSize))
    for x in range(nCell):
        for y in range(nCell):
            tim.set(x * sizeCell + sizeCell // 2 + border - 1,
                    y * sizeCell + sizeCell // 2 + border - 1,
                    deltaFunctionCounts)

    # Turn this into stars with a narrow width; conserve counts
    gaussFunction = afwMath.GaussianFunction2D(tGaussianWidth, tGaussianWidth)
    gaussKernel = afwMath.AnalyticKernel(gaussKernelWidth, gaussKernelWidth, gaussFunction)
    cim = afwImage.ImageF(tim.getDimensions())
    afwMath.convolve(cim, tim, gaussKernel, True)
    tim = cim

    # Trim off border pixels
    bbox = gaussKernel.shrinkBBox(tim.getBBox(afwImage.LOCAL))
    tim = afwImage.ImageF(tim, bbox, afwImage.LOCAL)

    # Now make a science image which is this convolved with some
    # spatial function.  Use input basis list.
    polyFunc = afwMath.PolynomialFunction2D(1)
    kCoeffs = fakeCoeffs()
    nToUse = min(len(kCoeffs), len(basisList))

    # Make the full convolved science image
    sKernel = afwMath.LinearCombinationKernel(afwMath.KernelList(basisList[:nToUse]), polyFunc)
    sKernel.setSpatialParameters(kCoeffs[:nToUse])
    sim = afwImage.ImageF(tim.getDimensions())
    afwMath.convolve(sim, tim, sKernel, True)

    # Get the good subregion
    bbox = sKernel.shrinkBBox(sim.getBBox(afwImage.LOCAL))

    # Add background
    sim += bgValue

    # Watch out for negative values
    tim += 2 * np.abs(np.min(tim.getArray()))

    # Add noise?
    if addNoise:
        sim = makePoissonNoiseImage(sim)
        tim = makePoissonNoiseImage(tim)

    # And turn into MaskedImages
    sim = afwImage.ImageF(sim, bbox, afwImage.LOCAL)
    svar = afwImage.ImageF(sim, True)
    smask = afwImage.MaskU(sim.getDimensions())
    smask.set(0x0)
    sMi = afwImage.MaskedImageF(sim, smask, svar)

    tim = afwImage.ImageF(tim, bbox, afwImage.LOCAL)
    tvar = afwImage.ImageF(tim, True)
    tmask = afwImage.MaskU(tim.getDimensions())
    tmask.set(0x0)
    tMi = afwImage.MaskedImageF(tim, tmask, tvar)

    if display:
        import lsst.afw.display.ds9 as ds9
        ds9.mtv(tMi, frame=1)
        ds9.mtv(sMi, frame=2)

    # Finally, make a kernelSet from these 2 images
    kernelCellSet = afwMath.SpatialCellSet(afwGeom.Box2I(afwGeom.Point2I(0, 0),
                                                         afwGeom.Extent2I(sizeCell * nCell,
                                                                          sizeCell * nCell)),
                                           sizeCell,
                                           sizeCell)
    stampHalfWidth = 2 * kSize
    for x in range(nCell):
        for y in range(nCell):
            xCoord = x * sizeCell + sizeCell // 2
            yCoord = y * sizeCell + sizeCell // 2
            p0 = afwGeom.Point2I(xCoord - stampHalfWidth,
                                 yCoord - stampHalfWidth)
            p1 = afwGeom.Point2I(xCoord + stampHalfWidth,
                                 yCoord + stampHalfWidth)
            bbox = afwGeom.Box2I(p0, p1)
            tsi = afwImage.MaskedImageF(tMi, bbox, afwImage.LOCAL)
            ssi = afwImage.MaskedImageF(sMi, bbox, afwImage.LOCAL)

            kc = diffimLib.makeKernelCandidate(xCoord, yCoord, tsi, ssi, policyFake)
            kernelCellSet.insertCandidate(kc)

    tMi.setXY0(0, 0)
    sMi.setXY0(0, 0)
    return tMi, sMi, sKernel, kernelCellSet, configFake


#######
# Background subtraction for ip_diffim
#######

def backgroundSubtract(config, maskedImages):
    backgrounds = []
    t0 = time.time()
    algorithm = config.algorithm
    binsize = config.binSize
    undersample = config.undersampleStyle
    bctrl = afwMath.BackgroundControl(algorithm)
    bctrl.setUndersampleStyle(undersample)
    for maskedImage in maskedImages:
        bctrl.setNxSample(maskedImage.getWidth()//binsize + 1)
        bctrl.setNySample(maskedImage.getHeight()//binsize + 1)
        image = maskedImage.getImage()
        backobj = afwMath.makeBackground(image, bctrl)

        image -= backobj.getImageF()
        backgrounds.append(backobj.getImageF())
        del backobj

    t1 = time.time()
    pexLog.Trace("lsst.ip.diffim.backgroundSubtract", 1,
                 "Total time for background subtraction : %.2f s" % (t1-t0))
    return backgrounds

#######
# More coarse debugging
#######


def writeKernelCellSet(kernelCellSet, psfMatchingKernel, backgroundModel, outdir):
    if not os.path.isdir(outdir):
        os.makedirs(outdir)

    for cell in kernelCellSet.getCellList():
        for cand in cell.begin(False):  # False = include bad candidates
            cand = diffimLib.cast_KernelCandidateF(cand)
            if cand.getStatus() == afwMath.SpatialCellCandidate.GOOD:
                xCand = int(cand.getXCenter())
                yCand = int(cand.getYCenter())
                idCand = cand.getId()
                diffIm = cand.getDifferenceImage(diffimLib.KernelCandidateF.ORIG)
                kernel = cand.getKernelImage(diffimLib.KernelCandidateF.ORIG)
                diffIm.writeFits(os.path.join(outdir, 'diffim_c%d_x%d_y%d.fits' % (idCand, xCand, yCand)))
                kernel.writeFits(os.path.join(outdir, 'kernel_c%d_x%d_y%d.fits' % (idCand, xCand, yCand)))

                # Diffim from spatial model
                ski = afwImage.ImageD(kernel.getDimensions())
                psfMatchingKernel.computeImage(ski, False, xCand, yCand)
                sk = afwMath.FixedKernel(ski)
                sbg = backgroundModel(xCand, yCand)
                sdmi = cand.getDifferenceImage(sk, sbg)
                sdmi.writeFits(os.path.join(outdir, 'sdiffim_c%d_x%d_y%d.fits' % (idCand, xCand, yCand)))

#######
# Converting types
#######


def sourceToFootprintList(candidateInList, templateExposure, scienceExposure, kernelSize, config, log):
    """ Takes an input list of Sources that were selected to constrain
    the Psf-matching Kernel and turns them into a List of Footprints,
    which are used to seed a set of KernelCandidates.  The function
    checks both the template and science image for masked pixels,
    rejecting the Source if certain Mask bits (defined in config) are
    set within the Footprint.

    @param candidateInList: Input list of Sources
    @param templateExposure: Template image, to be checked for Mask bits in Source Footprint
    @param scienceExposure: Science image, to be checked for Mask bits in Source Footprint
    @param config: Config that defines the Mask planes that indicate an invalid Source and Bbox grow radius
    @param log: Log for output

    @return a list of dicts having a "source" and "footprint" field, to be used for Psf-matching
    """

    candidateOutList = []
    fsb = diffimLib.FindSetBitsU()
    badBitMask = 0
    for mp in config.badMaskPlanes:
        badBitMask |= afwImage.MaskU.getPlaneBitMask(mp)
    bbox = scienceExposure.getBBox()

    # Size to grow Sources
    if config.scaleByFwhm:
        fpGrowPix = int(config.fpGrowKernelScaling * kernelSize + 0.5)
    else:
        fpGrowPix = config.fpGrowPix
    log.info("Growing %d kernel candidate stars by %d pixels" % (len(candidateInList), fpGrowPix))

    for kernelCandidate in candidateInList:
        if not type(kernelCandidate) == afwTable.SourceRecord:
            raise RuntimeError, ("Candiate not of type afwTable.SourceRecord")
        bm1 = 0
        bm2 = 0
        center = afwGeom.Point2I(scienceExposure.getWcs().skyToPixel(kernelCandidate.getCoord()))
        if center[0] < bbox.getMinX() or center[0] > bbox.getMaxX():
            continue
        if center[1] < bbox.getMinY() or center[1] > bbox.getMaxY():
            continue

        xmin = center[0] - fpGrowPix
        xmax = center[0] + fpGrowPix
        ymin = center[1] - fpGrowPix
        ymax = center[1] + fpGrowPix

        # Keep object centered
        if (xmin - bbox.getMinX()) < 0:
            xmax += (xmin - bbox.getMinX())
            xmin -= (xmin - bbox.getMinX())
        if (ymin - bbox.getMinY()) < 0:
            ymax += (ymin - bbox.getMinY())
            ymin -= (ymin - bbox.getMinY())
        if (bbox.getMaxX() - xmax) < 0:
            xmin -= (bbox.getMaxX() - xmax)
            xmax += (bbox.getMaxX() - xmax)
        if (bbox.getMaxY() - ymax) < 0:
            ymin -= (bbox.getMaxY() - ymax)
            ymax += (bbox.getMaxY() - ymax)
        if xmin > xmax or ymin > ymax:
            continue

        kbbox = afwGeom.Box2I(afwGeom.Point2I(xmin, ymin), afwGeom.Point2I(xmax, ymax))
        try:
            fsb.apply(afwImage.MaskedImageF(templateExposure.getMaskedImage(), kbbox, False).getMask())
            bm1 = fsb.getBits()
            fsb.apply(afwImage.MaskedImageF(scienceExposure.getMaskedImage(), kbbox, False).getMask())
            bm2 = fsb.getBits()
        except Exception:
            pass
        else:
            if not((bm1 & badBitMask) or (bm2 & badBitMask)):
                candidateOutList.append({'source': kernelCandidate, 'footprint': afwDetect.Footprint(kbbox)})
    log.info("Selected %d / %d sources for KernelCandidacy" % (len(candidateOutList), len(candidateInList)))
    return candidateOutList


def sourceTableToCandidateList(sourceTable, templateExposure, scienceExposure, kConfig, dConfig, log,
                               basisList, doBuild=False):
    """Takes an input list of Sources, and turns them into
    KernelCandidates for fitting of the Psf-matching kernel."""
    kernelSize = basisList[0].getWidth()
    footprintList = sourceToFootprintList(list(sourceTable), templateExposure, scienceExposure,
                                          kernelSize, dConfig, log)
    candList = []

    if doBuild and not basisList:
        doBuild = False
    else:
        policy = pexConfig.makePolicy(kConfig)
        visitor = diffimLib.BuildSingleKernelVisitorF(basisList, policy)

    policy = pexConfig.makePolicy(kConfig)
    for cand in footprintList:
        bbox = cand['footprint'].getBBox()
        tmi = afwImage.MaskedImageF(templateExposure.getMaskedImage(), bbox)
        smi = afwImage.MaskedImageF(scienceExposure.getMaskedImage(), bbox)
        kCand = diffimLib.makeKernelCandidate(cand['source'], tmi, smi, policy)
        if doBuild:
            visitor.processCandidate(kCand)
            kCand.setStatus(afwMath.SpatialCellCandidate.UNKNOWN)
        candList.append(kCand)
    return candList


#######
#
#######


class NbasisEvaluator(object):
    """A functor to evaluate the Bayesian Information Criterion for the number of basis sets going into the kernel fitting"""

    def __init__(self, psfMatchConfig, psfFwhmPixTc, psfFwhmPixTnc):
        self.psfMatchConfig = psfMatchConfig
        self.psfFwhmPixTc = psfFwhmPixTc
        self.psfFwhmPixTnc = psfFwhmPixTnc
        if not self.psfMatchConfig.kernelBasisSet == "alard-lupton":
            raise RuntimeError, "BIC only implemnted for AL (alard lupton) basis"

    def __call__(self, kernelCellSet, log):
        d1, d2, d3 = self.psfMatchConfig.alardDegGauss
        bicArray = {}
        for d1i in range(1, d1+1):
            for d2i in range(1, d2+1):
                for d3i in range(1, d3+1):
                    dList = [d1i, d2i, d3i]
                    bicConfig = type(self.psfMatchConfig)(self.psfMatchConfig, alardDegGauss=dList)
                    kList = makeKernelBasisList(bicConfig, self.psfFwhmPixTc, self.psfFwhmPixTnc)
                    k = len(kList)
                    visitor = diffimLib.BuildSingleKernelVisitorF(kList, pexConfig.makePolicy(bicConfig))
                    visitor.setSkipBuilt(False)
                    kernelCellSet.visitCandidates(visitor, bicConfig.nStarPerCell)

                    for cell in kernelCellSet.getCellList():
                        for cand in cell.begin(False):  # False = include bad candidates
                            cand = diffimLib.cast_KernelCandidateF(cand)
                            if cand.getStatus() != afwMath.SpatialCellCandidate.GOOD:
                                continue
                            diffIm = cand.getDifferenceImage(diffimLib.KernelCandidateF.RECENT)
                            bbox = cand.getKernel(diffimLib.KernelCandidateF.RECENT).shrinkBBox(
                                diffIm.getBBox(afwImage.LOCAL))
                            diffIm = type(diffIm)(diffIm, bbox, True)
                            chi2 = diffIm.getImage().getArray()**2 / diffIm.getVariance().getArray()
                            n = chi2.shape[0] * chi2.shape[1]
                            bic = np.sum(chi2) + k * np.log(n)
                            if not bicArray.has_key(cand.getId()):
                                bicArray[cand.getId()] = {}
                            bicArray[cand.getId()][(d1i, d2i, d3i)] = bic

        bestConfigs = []
        candIds = bicArray.keys()
        for candId in candIds:
            cconfig, cvals = bicArray[candId].keys(), bicArray[candId].values()
            idx = np.argsort(cvals)
            bestConfig = cconfig[idx[0]]
            bestConfigs.append(bestConfig)

        counter = Counter(bestConfigs).most_common(3)
        log.info("B.I.C. prefers basis complexity %s %d times; %s %d times; %s %d times" % (counter[0][0], counter[0][1],
                                                                                            counter[1][
                                                                                                0], counter[1][1],
                                                                                            counter[2][0], counter[2][1]))
        return counter[0][0], counter[1][0], counter[2][0]
