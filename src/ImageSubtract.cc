// -*- lsst-c++ -*-
/**
 * @file
 *
 * @brief Implementation of image subtraction functions declared in ImageSubtract.h
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup diffim
 */
#include <iostream>
#include <limits>
#include <boost/timer.hpp> 

#include <vw/Math/Functions.h> 
#include <vw/Math/Vector.h> 
#include <vw/Math/Matrix.h> 
#include <vw/Math/LinearAlgebra.h> 

#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_machine.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_linalg.h>

#define LSST_MAX_TRACE 5                // NOTE -  trace statements >= 6 can ENTIRELY kill the run time
#include <lsst/afw/image.h>
#include <lsst/afw/math.h>
#include <lsst/pex/exceptions/Exception.h>
#include <lsst/pex/logging/Trace.h>
#include <lsst/pex/logging/Log.h>
#include "lsst/ip/diffim/Pca.h"
#include "lsst/ip/diffim/ImageSubtract.h"

#define DEBUG_MATRIX 0

//
// Constructors
//

template <typename ImageT, typename MaskT>
lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>::DifferenceImageStatistics() :
    lsst::daf::data::LsstBase(typeid(this)),
    _residualMean(0),
    _residualStd(0)
{
}

template <typename ImageT, typename MaskT>
lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>::DifferenceImageStatistics(
    const lsst::afw::image::MaskedImage<ImageT, MaskT> differenceMaskedImage
    ) :
    lsst::daf::data::LsstBase(typeid(this)),
    _residualMean(0),
    _residualStd(0)
{
    int nGood;
    double mean, variance;

    lsst::ip::diffim::calculateMaskedImageStatistics(nGood, mean, variance, differenceMaskedImage);
    this->_residualMean = mean;
    this->_residualStd  = sqrt(variance);
}

template <typename ImageT, typename MaskT>
lsst::ip::diffim::DifferenceImageFootprintInformation<ImageT, MaskT>::DifferenceImageFootprintInformation(
    lsst::detection::Footprint::PtrType footprintPtr,
    MaskedImagePtr imageToConvolvePtr,
    MaskedImagePtr imageToNotConvolvePtr
    ) :
    lsst::daf::data::LsstBase(typeid(this)),
    _id(-1),
    _colcNorm(0),
    _rowcNorm(0),
    _footprintPtr(footprintPtr),
    _imageToConvolvePtr(imageToConvolvePtr),
    _imageToNotConvolvePtr(imageToNotConvolvePtr),
    _singleKernelPtr(), // start off NULL
    _singleBackground(0),
    _singleKernelStats( lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>() ),
    _isGood(false)
{
}
    
//
// Public Member Functions
//

template <typename ImageT, typename MaskT>
bool lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>::evaluateQuality(
    lsst::pex::policy::Policy &policy
    ) {
    double maxResidualMean = policy.getDouble("maximumFootprintResidualMean");
    double maxResidualStd  = policy.getDouble("maximumFootprintResidualStd");
    if ( (this->getResidualMean()) > maxResidualMean ) return false;
    if ( (this->getResidualStd()) > maxResidualStd ) return false;
    return true;
}

template <typename ImageT, typename MaskT>
lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT> 
lsst::ip::diffim::DifferenceImageFootprintInformation<ImageT, MaskT>::computeImageStatistics(
    boost::shared_ptr<lsst::afw::math::Kernel> kernelPtr,
    double background
    ) {

    lsst::afw::image::MaskedImage<ImageT, MaskT> differenceMaskedImage = convolveAndSubtract( *(_imageToConvolvePtr),
                                                                                              *(_imageToNotConvolvePtr),
                                                                                              kernelPtr,
                                                                                              background );
    lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT> diffimStats = 
        lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>(differenceMaskedImage);
    return diffimStats;
}


//
// Subroutines
//

template <typename ImageT, typename MaskT>
typename lsst::ip::diffim::DifferenceImageFootprintInformation<ImageT, MaskT>::DifiList
lsst::ip::diffim::getGoodFootprints( 
    std::vector<boost::shared_ptr<DifferenceImageFootprintInformation<ImageT, MaskT> > > &difiList
    )
{
    typename DifferenceImageFootprintInformation<ImageT,MaskT>::DifiList goodList;
    for (typename DifferenceImageFootprintInformation<ImageT,MaskT>::DifiList::iterator i = difiList.begin(); 
         i != difiList.end(); ++i) {
        if ((*i)->getStatus() == true) {
            goodList.push_back((*i));
        }
    }
    return goodList;
}
    
/** 
 * @brief Generate a basis set of delta function Kernels.
 *
 * Generates a vector of Kernels sized nCols * nRows, where each Kernel has
 * a unique pixel set to value 1.0 with the other pixels valued 0.0.  This
 * is the "delta function" basis set.
 * 
 * @return Vector of orthonormal delta function Kernels.
 *
 * @throw lsst::pex::exceptions::DomainError if nRows or nCols not positive
 *
 * @ingroup diffim
 */
lsst::afw::math::KernelList<lsst::afw::math::Kernel>
lsst::ip::diffim::generateDeltaFunctionKernelSet(
    unsigned int nCols, ///< Number of colunms in the basis kernels
    unsigned int nRows  ///< Number of rows in the basis kernels
    )
{
    if ((nCols < 1) || (nRows < 1)) {
        throw lsst::pex::exceptions::DomainError("nRows and nCols must be positive");
    }
    const int signedNCols = static_cast<int>(nCols);
    const int signedNRows = static_cast<int>(nRows);
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> kernelBasisList;
    for (int row = 0; row < signedNRows; ++row) {
        for (int col = 0; col < signedNCols; ++col) {
            boost::shared_ptr<lsst::afw::math::Kernel> kernelPtr(
                new lsst::afw::math::DeltaFunctionKernel(col, row, nCols, nRows)
                );
            
            kernelBasisList.push_back(kernelPtr);
        }
    }
    return kernelBasisList;
}

/** 
 * @brief Generate an Alard-Lupton basis set of Kernels.
 *
 * Not implemented.
 * 
 * @return Vector of Alard-Lupton Kernels.
 *
 * @throw lsst::pex::exceptions::DomainError if nRows or nCols not positive
 * @throw lsst::pex::exceptions::DomainError until implemented
 *
 * @ingroup diffim
 */
lsst::afw::math::KernelList<lsst::afw::math::Kernel>
lsst::ip::diffim::generateAlardLuptonKernelSet(
    unsigned int nRows, ///< Number of rows in the kernel basis
    unsigned int nCols, ///< Number of columns in the kernel basis
    std::vector<double> const &sigGauss, ///< Width of gaussians in basis; size = number of Gaussians
    std::vector<double> const &degGauss  ///< Degree of spatial variation within each Gaussian; size = sigGauss.size()
    )
{
    if ((nCols < 1) || (nRows < 1)) {
        throw lsst::pex::exceptions::DomainError("nRows and nCols must be positive");
    }
    throw lsst::pex::exceptions::DomainError("Not implemented");

    lsst::afw::math::KernelList<lsst::afw::math::Kernel> kernelBasisList;
    return kernelBasisList;
}

/** 
 * @brief Implement fundamental difference imaging step of convolution and
 * subtraction : D = I - (K.x.T + bg)
 *
 * @return Difference image
 *
 * @ingroup diffim
 */
template <typename ImageT, typename MaskT>
lsst::afw::image::MaskedImage<ImageT, MaskT> lsst::ip::diffim::convolveAndSubtract(
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToConvolve,
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToNotConvolve,
    boost::shared_ptr<lsst::afw::math::Kernel> const &convolutionKernelPtr,
    double background
    ) {

    int edgeMaskBit = imageToConvolve.getMask()->getMaskPlane("EDGE");
    lsst::afw::image::MaskedImage<ImageT, MaskT>
        convolvedMaskedImage = lsst::afw::math::convolveNew(
            imageToConvolve,
            *(convolutionKernelPtr),
            edgeMaskBit, 
            false);
    
    /* Add in background */
    convolvedMaskedImage += background;

    /* Do actual subtraction */
    convolvedMaskedImage -= imageToNotConvolve;
    convolvedMaskedImage *= -1.0;
    
    return convolvedMaskedImage;
}

/** 
 * @brief Runs Detection on a single image for significant peaks, and checks
 * returned Footprints for Masked pixels.
 *
 * Accepts two MaskedImages, one of which is to be convolved to match the
 * other.  The Detection package is run on the image to be convolved
 * (assumed to be higher S/N than the other image).  The subimages
 * associated with each returned Footprint in both images are checked for
 * Masked pixels; Footprints containing Masked pixels are rejected.  The
 * Footprints are grown by an amount specified in the Policy.  The
 * acceptible Footprints are returned in a vector.
 *
 * @return Vector of "clean" Footprints around which Image Subtraction
 * Kernels will be built.
 *
 * @ingroup diffim
 */
template <typename ImageT, typename MaskT>
std::vector<lsst::detection::Footprint::PtrType> lsst::ip::diffim::getCollectionOfFootprintsForPsfMatching(
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToConvolve, ///< Template image; is convolved
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToNotConvolve, ///< Science image; is not convolved
    lsst::pex::policy::Policy &policy ///< Policy directing the behavior
    ) {
    
    /* Parse the Policy */
    unsigned int footprintDiffimNpixMin = policy.getInt("footprintDiffimNpixMin");
    unsigned int footprintDiffimGrow = policy.getInt("footprintDiffimGrow");
    int minimumCleanFootprints = policy.getInt("minimumCleanFootprints");
    double footprintDetectionThreshold = policy.getDouble("footprintDetectionThreshold");
    double detectionThresholdScaling = policy.getDouble("detectionThresholdScaling");
    double minimumDetectionThreshold = policy.getDouble("minimumDetectionThreshold");

    /* grab mask bits from the image to convolve, since that is what we'll be operating on */
    int badMaskBit = imageToConvolve.getMask()->getMaskPlane("BAD");
    MaskT badPixelMask = (badMaskBit < 0) ? 0 : (1 << badMaskBit);

    /* Reusable view of each Footprint */
    typename lsst::afw::image::MaskedImage<ImageT, MaskT>::MaskedImagePtrT imageToConvolveFootprintPtr;
    typename lsst::afw::image::MaskedImage<ImageT, MaskT>::MaskedImagePtrT imageToNotConvolveFootprintPtr;

    /* Reusable list of Footprints */
    std::vector<lsst::detection::Footprint::PtrType> footprintListIn;
    std::vector<lsst::detection::Footprint::PtrType> footprintListOut;

    int nCleanFootprints = 0;
    while ( (nCleanFootprints < minimumCleanFootprints) and (footprintDetectionThreshold > minimumDetectionThreshold) ) {
        footprintListIn.clear();
        footprintListOut.clear();
        
        /* Find detections */
        lsst::detection::DetectionSet<ImageT, MaskT> 
            detectionSet(imageToConvolve, lsst::detection::Threshold(footprintDetectionThreshold, lsst::detection::Threshold::VALUE));
        
        /* get the footprints */
        footprintListIn = detectionSet.getFootprints();
        
        nCleanFootprints = 0;
        for (std::vector<lsst::detection::Footprint::PtrType>::iterator i = footprintListIn.begin(); i != footprintListIn.end(); ++i) {
            /* footprint has not enough pixels */
            if (static_cast<unsigned int>((*i)->getNpix()) < footprintDiffimNpixMin) {
                continue;
            } 

            /* grab the BBox and grow it; this will eventually be overridden by a grow method on Footprint */

            /* this caused a gcc -O2 error; will leave in for some future programmer's amusement */
            /*
            vw::BBox2i footprintBBox = (*i)->getBBox();
            footprintBBox.grow(footprintBBox.max() + vw::Vector2i(footprintDiffimGrow,footprintDiffimGrow));
            footprintBBox.grow(footprintBBox.min() - vw::Vector2i(footprintDiffimGrow,footprintDiffimGrow));
            */

            /* the workaround provided by Serge */
            vw::BBox2i const & bb = (*i)->getBBox();
            vw::Vector2i const minVec(bb.min().x() - footprintDiffimGrow, bb.min().y() - footprintDiffimGrow);
            vw::Vector2i const maxVec(bb.max().x() + footprintDiffimGrow, bb.max().y() + footprintDiffimGrow);
            vw::BBox2i const footprintBBox(minVec, maxVec);
            
            /* grab a subimage; there is an exception if its e.g. too close to the image */
            try {
                imageToConvolveFootprintPtr = imageToConvolve.getSubImage(footprintBBox);
                imageToNotConvolveFootprintPtr = imageToNotConvolve.getSubImage(footprintBBox);
            } catch (lsst::pex::exceptions::ExceptionStack &e) {
                continue;
            }
            
            if (lsst::ip::diffim::maskOk(*(imageToConvolveFootprintPtr->getMask()), badPixelMask) && 
                lsst::ip::diffim::maskOk(*(imageToNotConvolveFootprintPtr->getMask()), badPixelMask) ) {

                /* Create a new footprint with grow'd box */
                lsst::detection::Footprint::PtrType fpGrow(new lsst::detection::Footprint(footprintBBox));
                footprintListOut.push_back(fpGrow);
                
                nCleanFootprints += 1;
            }
        }
        
        footprintDetectionThreshold *= detectionThresholdScaling;
    }
    lsst::pex::logging::TTrace<3>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                                "Found %d clean footprints above threshold %.3f",
                                footprintListOut.size(), footprintDetectionThreshold/detectionThresholdScaling);

    return footprintListOut;
}

/** 
 * \brief Computes a single Kernel (Model 1) around a single subimage.
 *
 * Accepts two MaskedImages, generally subimages of a larger image, one of
 * which is to be convolved to match the other.  The output Kernel is
 * generated using an input list of basis Kernels by finding the
 * coefficients in front of each basis.
 *
 * \return Vector of coefficients representing the relative contribution of
 * each input basis function.
 *
 * \return Differential background offset between the two images
 *
 * \ingroup diffim
 */
template <typename ImageT, typename MaskT>
std::vector<double> lsst::ip::diffim::computePsfMatchingKernelForFootprint(
    double &background, ///< Difference in the backgrounds
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToConvolve, ///< Image to convolve
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToNotConvolve, ///< Image to not convolve
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList, ///< Input kernel basis set
    lsst::pex::policy::Policy &policy ///< Policy directing the behavior
    ) { 
    
    /* grab mask bits from the image to convolve, since that is what we'll be operating on */
    int edgeMaskBit = imageToConvolve.getMask()->getMaskPlane("EDGE");
    
    int nKernelParameters = 0;
    int nBackgroundParameters = 0;
    int nParameters = 0;

    boost::timer t;
    double time;
    t.restart();

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Entering subroutine computePsfMatchingKernelForFootprint");
    
    /* We assume that each kernel in the Set has 1 parameter you fit for */
    nKernelParameters = kernelInBasisList.size();
    /* Or, we just assume that across a single kernel, background 0th order.  This quite makes sense. */
    nBackgroundParameters = 1;
    /* Total number of parameters */
    nParameters = nKernelParameters + nBackgroundParameters;
    
    vw::math::Vector<double> B(nParameters);
    vw::math::Matrix<double> M(nParameters, nParameters);
    for (unsigned int i = nParameters; i--;) {
        B(i) = 0;
        for (unsigned int j = nParameters; j--;) {
            M(i,j) = 0;
        }
    }
    
    /* convolve creates a MaskedImage, push it onto the back of the Vector */
    /* need to use shared pointers because MaskedImage copy does not work */
    std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > > convolvedImageList(nKernelParameters);
    /* and an iterator over this */
    typename std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > >::iterator citer = convolvedImageList.begin();
    
    /* Iterator for input kernel basis */
    std::vector<boost::shared_ptr<lsst::afw::math::Kernel> >::const_iterator kiter = kernelInBasisList.begin();
    /* Create C_ij in the formalism of Alard & Lupton */
    for (; kiter != kernelInBasisList.end(); ++kiter, ++citer) {
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolving an Object with Basis");
        
        /* NOTE : we could also *precompute* the entire template image convolved with these functions */
        /*        and save them somewhere to avoid this step each time.  however, our paradigm is to */
        /*        compute whatever is needed on the fly.  hence this step here. */
        boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > imagePtr(
            new lsst::afw::image::MaskedImage<ImageT, MaskT>
            (lsst::afw::math::convolveNew(imageToConvolve, **kiter, edgeMaskBit, false))
            );
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolved an Object with Basis");
        
        *citer = imagePtr;
        
    } 
    
    /* NOTE ABOUT CONVOLUTION : */
    /* getCtrCol:getCtrRow pixels are masked on the left:bottom side */
    /* getCols()-getCtrCol():getRows()-getCtrRow() masked on right/top side */
    /* */
    /* The convolved image and the input image are by default the same size, so */
    /* we offset our initial pixel references by the same amount */
    kiter = kernelInBasisList.begin();
    citer = convolvedImageList.begin();
    unsigned int startCol = (*kiter)->getCtrCol();
    unsigned int startRow = (*kiter)->getCtrRow();
    /* NOTE - I determined I needed this +1 by eye */
    unsigned int endCol   = (*citer)->getCols() - ((*kiter)->getCols() - (*kiter)->getCtrCol()) + 1;
    unsigned int endRow   = (*citer)->getRows() - ((*kiter)->getRows() - (*kiter)->getCtrRow()) + 1;
    /* NOTE - we need to enforce that the input images are large enough */
    /* How about some multiple of the PSF FWHM?  Or second moments? */
    
    /* An accessor for each convolution plane */
    /* NOTE : MaskedPixelAccessor has no empty constructor, therefore we need to push_back() */
    std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorRowList;
    for (citer = convolvedImageList.begin(); citer != convolvedImageList.end(); ++citer) {
        convolvedAccessorRowList.push_back(lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT>(**citer));
    }
    
    /* An accessor for each input image; address rows and cols separately */
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveRow(imageToConvolve);
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveRow(imageToNotConvolve);
    
    /* Take into account buffer for kernel images */
    imageToConvolveRow.advance(startCol, startRow);
    imageToNotConvolveRow.advance(startCol, startRow);
    for (int ki = 0; ki < nKernelParameters; ++ki) {
        convolvedAccessorRowList[ki].advance(startCol, startRow);
    }

    for (unsigned int row = startRow; row < endRow; ++row) {
        
        /* An accessor for each convolution plane */
        std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorColList = convolvedAccessorRowList;
        
        /* An accessor for each input image; places the col accessor at the correct row */
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveCol = imageToConvolveRow;
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveCol = imageToNotConvolveRow;
        
        for (unsigned int col = startCol; col < endCol; ++col) {
            
            ImageT ncCamera   = *imageToNotConvolveCol.image;
            ImageT ncVariance = *imageToNotConvolveCol.variance;
            MaskT  ncMask     = *imageToNotConvolveCol.mask;
            
            ImageT cVariance  = *imageToConvolveCol.variance;
            
            /* Variance for a particlar pixel; do we use this variance of the */
            /* input data, or include the variance after its been convolved with */
            /* the basis?  For now, use the average of the input varianes. */
            ImageT iVariance  = 1.0 / (cVariance + ncVariance);
            
            lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                          "Accessing image row %d col %d : %.3f %.3f %d",
                                          row, col, ncCamera, ncVariance, ncMask);
            
            /* kernel index i */
            typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator
                kiteri = convolvedAccessorColList.begin();
            
            for (int kidxi = 0; kiteri != convolvedAccessorColList.end(); ++kiteri, ++kidxi) {
                ImageT cdCamerai   = *kiteri->image;
                
                ImageT cdVariancei = *kiteri->variance;
                MaskT  cdMaski     = *kiteri->mask;
                lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                              "Accessing convolved image %d : %.3f %.3f %d",
                                              kidxi, cdCamerai, cdVariancei, cdMaski);
                
                /* kernel index j  */
                typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator kiterj = kiteri;
                for (int kidxj = kidxi; kiterj != convolvedAccessorColList.end(); ++kiterj, ++kidxj) {
                    ImageT cdCameraj   = *kiterj->image;
                    
                    /* NOTE - These inner trace statements can ENTIRELY kill the run time */
                    ImageT cdVariancej = *kiterj->variance;
                    MaskT  cdMaskj     = *kiterj->mask;
                    lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                                  "Accessing convolved image %d : %.3f %.3f %d",
                                                  kidxj, cdCameraj, cdVariancej, cdMaskj);
                    
                    M[kidxi][kidxj] += cdCamerai * cdCameraj * iVariance;
                } 
                
                B[kidxi] += ncCamera * cdCamerai * iVariance;
                
                /* Constant background term; effectively j=kidxj+1 */
                M[kidxi][nParameters-1] += cdCamerai * iVariance;
            } 
            
            /* Background term; effectively i=kidxi+1 */
            B[nParameters-1] += ncCamera * iVariance;
            M[nParameters-1][nParameters-1] += 1.0 * iVariance;
            
            lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                          "Background terms : %.3f %.3f",
                                          B[nParameters-1], M[nParameters-1][nParameters-1]);
            
            /* Step each accessor in column */
            imageToConvolveCol.nextCol();
            imageToNotConvolveCol.nextCol();
            for (int ki = 0; ki < nKernelParameters; ++ki) {
                convolvedAccessorColList[ki].nextCol();
            }             
            
        } /* col */
        
        /* Step each accessor in row */
        imageToConvolveRow.nextRow();
        imageToNotConvolveRow.nextRow();
        for (int ki = 0; ki < nKernelParameters; ++ki) {
            convolvedAccessorRowList[ki].nextRow();
        }
        
    } /* row */

    /* NOTE: If we are going to regularize the solution to M, this is the place to do it */
    
    /* Fill in rest of M */
    for (int kidxi=0; kidxi < nParameters; ++kidxi) 
        for (int kidxj=kidxi+1; kidxj < nParameters; ++kidxj) 
            M[kidxj][kidxi] = M[kidxi][kidxj];
    
#if DEBUG_MATRIX
    std::cout << "B : " << B << std::endl;
    std::cout << "M : " << M << std::endl;
#endif

    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time before matrix inversions : %.2f s", time);

    /* Invert using SVD and Pseudoinverse */
    vw::math::Matrix<double> Minv;
    Minv = vw::math::pseudoinverse(M);
    /*Minv = vw::math::inverse(M); */
    
#if DEBUG_MATRIX
    std::cout << "Minv : " << Minv << std::endl;
#endif
    
    /* Solve for x in Mx = B */
    vw::math::Vector<double> Soln = Minv * B;
    
#if DEBUG_MATRIX
    std::cout << "Solution : " << Soln << std::endl;
#endif
    
    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time after matrix inversions : %.2f s", time);

    /* Translate from VW std::vectors to std std::vectors */
    std::vector<double> kernelCoeffs(kernelInBasisList.size());
    for (int ki = 0; ki < nKernelParameters; ++ki) {
        kernelCoeffs[ki] = Soln[ki];
    }
    background = Soln[nParameters-1];

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Leaving subroutine computePsfMatchingKernelForFootprint");

    return kernelCoeffs;
}


/** 
 * \brief Computes a single Kernel (Model 1) around a single subimage.
 *
 * Accepts two MaskedImages, generally subimages of a larger image, one of which
 * is to be convolved to match the other.  The output Kernel is generated using
 * an input list of basis Kernels by finding the coefficients in front of each
 * basis.  This version accepts an input variance image.
 *
 * \return Vector of coefficients representing the relative contribution of
 * each input basis function.
 *
 * \return Differential background offset between the two images
 *
 * \ingroup diffim
 */
template <typename ImageT, typename MaskT>
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprint2(
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToConvolve, ///< Image to convolve
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToNotConvolve, ///< Image to not convolve
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &varianceImage, ///< Model of variance per pixel
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList, ///< Input kernel basis set
    lsst::pex::policy::Policy &policy ///< Policy directing the behavior
    ) { 
    
    /* grab mask bits from the image to convolve, since that is what we'll be operating on */
    int edgeMaskBit = imageToConvolve.getMask()->getMaskPlane("EDGE");
    
    int nKernelParameters = 0;
    int nBackgroundParameters = 0;
    int nParameters = 0;

    boost::timer t;
    double time;
    t.restart();

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Entering subroutine computePsfMatchingKernelForFootprint");
    
    /* We assume that each kernel in the Set has 1 parameter you fit for */
    nKernelParameters = kernelInBasisList.size();
    /* Or, we just assume that across a single kernel, background 0th order.  This quite makes sense. */
    nBackgroundParameters = 1;
    /* Total number of parameters */
    nParameters = nKernelParameters + nBackgroundParameters;
    
    vw::math::Vector<double> B(nParameters);
    vw::math::Matrix<double> M(nParameters, nParameters);
    for (unsigned int i = nParameters; i--;) {
        B(i) = 0;
        for (unsigned int j = nParameters; j--;) {
            M(i,j) = 0;
        }
    }
    
    /* convolve creates a MaskedImage, push it onto the back of the Vector */
    /* need to use shared pointers because MaskedImage copy does not work */
    std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > > convolvedImageList(nKernelParameters);
    /* and an iterator over this */
    typename std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > >::iterator citer = convolvedImageList.begin();
    
    /* Iterator for input kernel basis */
    std::vector<boost::shared_ptr<lsst::afw::math::Kernel> >::const_iterator kiter = kernelInBasisList.begin();
    /* Create C_ij in the formalism of Alard & Lupton */
    for (; kiter != kernelInBasisList.end(); ++kiter, ++citer) {
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolving an Object with Basis");
        
        /* NOTE : we could also *precompute* the entire template image convolved with these functions */
        /*        and save them somewhere to avoid this step each time.  however, our paradigm is to */
        /*        compute whatever is needed on the fly.  hence this step here. */
        boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > imagePtr(
            new lsst::afw::image::MaskedImage<ImageT, MaskT>
            (lsst::afw::math::convolveNew(imageToConvolve, **kiter, edgeMaskBit, false))
            );
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolved an Object with Basis");
        
        *citer = imagePtr;
        
    } 
    
    /* NOTE ABOUT CONVOLUTION : */
    /* getCtrCol:getCtrRow pixels are masked on the left:bottom side */
    /* getCols()-getCtrCol():getRows()-getCtrRow() masked on right/top side */
    /* */
    /* The convolved image and the input image are by default the same size, so */
    /* we offset our initial pixel references by the same amount */
    kiter = kernelInBasisList.begin();
    citer = convolvedImageList.begin();
    unsigned int startCol = (*kiter)->getCtrCol();
    unsigned int startRow = (*kiter)->getCtrRow();
    /* NOTE - I determined I needed this +1 by eye */
    unsigned int endCol   = (*citer)->getCols() - ((*kiter)->getCols() - (*kiter)->getCtrCol()) + 1;
    unsigned int endRow   = (*citer)->getRows() - ((*kiter)->getRows() - (*kiter)->getCtrRow()) + 1;
    /* NOTE - we need to enforce that the input images are large enough */
    /* How about some multiple of the PSF FWHM?  Or second moments? */
    
    /* An accessor for each convolution plane */
    /* NOTE : MaskedPixelAccessor has no empty constructor, therefore we need to push_back() */
    std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorRowList;
    for (citer = convolvedImageList.begin(); citer != convolvedImageList.end(); ++citer) {
        convolvedAccessorRowList.push_back(lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT>(**citer));
    }
    
    /* An accessor for each input image; address rows and cols separately */
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveRow(imageToConvolve);
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveRow(imageToNotConvolve);
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> varianceRow(varianceImage);
    
    /* Take into account buffer for kernel images */
    imageToConvolveRow.advance(startCol, startRow);
    imageToNotConvolveRow.advance(startCol, startRow);
    varianceRow.advance(startCol, startRow);

    for (int ki = 0; ki < nKernelParameters; ++ki) {
        convolvedAccessorRowList[ki].advance(startCol, startRow);
    }

    for (unsigned int row = startRow; row < endRow; ++row) {
        
        /* An accessor for each convolution plane */
        std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorColList = convolvedAccessorRowList;
        
        /* An accessor for each input image; places the col accessor at the correct row */
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveCol = imageToConvolveRow;
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveCol = imageToNotConvolveRow;
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> varianceCol = varianceRow;

        for (unsigned int col = startCol; col < endCol; ++col) {
            
            ImageT ncCamera   = *imageToNotConvolveCol.image;
            ImageT ncVariance = *imageToNotConvolveCol.variance;
            MaskT  ncMask     = *imageToNotConvolveCol.mask;
            
            ImageT iVariance  = 1.0 / *varianceCol.variance;
            
            lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                          "Accessing image row %d col %d : %.3f %.3f %d",
                                          row, col, ncCamera, ncVariance, ncMask);
            
            /* kernel index i */
            typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator
                kiteri = convolvedAccessorColList.begin();
            
            for (int kidxi = 0; kiteri != convolvedAccessorColList.end(); ++kiteri, ++kidxi) {
                ImageT cdCamerai   = *kiteri->image;

                /* NOTE - Commenting in these additional pixel accesses yields
                 * an additional second of run-time per kernel with opt=1 at 2.8
                 * GHz*/

                /* ignore unnecessary pixel accesses 
                ImageT cdVariancei = *kiteri->variance;
                MaskT  cdMaski     = *kiteri->mask;
                lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                            "Accessing convolved image %d : %.3f %.3f %d",
                                            kidxi, cdCamerai, cdVariancei, cdMaski);
                */

                /* kernel index j  */
                typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator kiterj = kiteri;
                for (int kidxj = kidxi; kiterj != convolvedAccessorColList.end(); ++kiterj, ++kidxj) {
                    ImageT cdCameraj   = *kiterj->image;

                    /* ignore unnecessary pixel accesses 
                    ImageT cdVariancej = *kiterj->variance;
                    MaskT  cdMaskj     = *kiterj->mask;
                    lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                                "Accessing convolved image %d : %.3f %.3f %d",
                                                kidxj, cdCameraj, cdVariancej, cdMaskj);
                    */

                    M[kidxi][kidxj] += cdCamerai * cdCameraj * iVariance;
                } 
                
                B[kidxi] += ncCamera * cdCamerai * iVariance;
                
                /* Constant background term; effectively j=kidxj+1 */
                M[kidxi][nParameters-1] += cdCamerai * iVariance;
            } 
            
            /* Background term; effectively i=kidxi+1 */
            B[nParameters-1] += ncCamera * iVariance;
            M[nParameters-1][nParameters-1] += 1.0 * iVariance;
            
            lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                          "Background terms : %.3f %.3f",
                                          B[nParameters-1], M[nParameters-1][nParameters-1]);
            
            /* Step each accessor in column */
            imageToConvolveCol.nextCol();
            imageToNotConvolveCol.nextCol();
            varianceCol.nextCol();
            for (int ki = 0; ki < nKernelParameters; ++ki) {
                convolvedAccessorColList[ki].nextCol();
            }             
            
        } /* col */
        
        /* Step each accessor in row */
        imageToConvolveRow.nextRow();
        imageToNotConvolveRow.nextRow();
        varianceRow.nextRow();
        for (int ki = 0; ki < nKernelParameters; ++ki) {
            convolvedAccessorRowList[ki].nextRow();
        }
        
    } /* row */

    /* NOTE: If we are going to regularize the solution to M, this is the place to do it */
    
    /* Fill in rest of M */
    for (int kidxi=0; kidxi < nParameters; ++kidxi) 
        for (int kidxj=kidxi+1; kidxj < nParameters; ++kidxj) 
            M[kidxj][kidxi] = M[kidxi][kidxj];
    
#if DEBUG_MATRIX
    std::cout << "B : " << B << std::endl;
    std::cout << "M : " << M << std::endl;
#endif

    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time before matrix inversions : %.2f s", time);

    /* Invert using SVD and Pseudoinverse : This is a full second slower per
     * kernel than vw::math::least_squares, compiled at opt=1 at 2.8 GHz.

     vw::math::Matrix<double> Minv = vw::math::pseudoinverse(M);
     vw::math::Vector<double> Soln = Minv * B;
     */     

    /* Invert using VW's internal method */
    vw::math::Vector<double> Soln = vw::math::least_squares(M, B);

    /* Additional gymnastics to get the parameter uncertainties */
    vw::math::Matrix<double> Mt = vw::math::transpose(M);
    vw::math::Matrix<double> MtM = Mt * M;
    vw::math::Matrix<double> Error = vw::math::pseudoinverse(MtM);

    /*
      NOTE : for any real kernels I have looked at, these solutions have agreed
      exactly.  However, when designing the testDeconvolve unit test with
      hand-built gaussians as objects and non-realistic noise, the solutions did
      *NOT* agree.

      std::cout << "Soln : " << Soln << std::endl;
      std::cout << "Soln2 : " << Soln2 << std::endl;
    */

#if DEBUG_MATRIX
    for (int kidxi=0; kidxi < nParameters; ++kidxi) {
        std::cout << "Par " << kidxi << " : " << Soln[kidxi] << " +/- " << sqrt(Error[kidxi][kidxi]) << std::endl;
    }
#endif
    
    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time after matrix inversions : %.2f s", time);

    /* Translate from VW std::vectors to std::vectors */
    std::vector<std::pair<double,double> > kernelSolution;
    for (int ki = 0; ki < nParameters; ++ki) {
        if (std::isnan(Soln[ki])) {
            throw lsst::pex::exceptions::DomainError("Unable to determine kernel solution (nan)");
        }
        if (std::isnan(Error[ki][ki])) {
            throw lsst::pex::exceptions::DomainError("Unable to determine kernel uncertainty (nan)");
        }
        if (Error[ki][ki] < 0.0) {
            /* Should this really be an exception, or should I instead
               assign a large uncertainty instead? */
            throw lsst::pex::exceptions::DomainError(boost::format("Unable to determine kernel uncertainty, negative variance (%.3e)") % Error[ki][ki]);
        }

        kernelSolution.push_back(std::make_pair<double,double>( Soln[ki], sqrt(Error[ki][ki]) ));
    }

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Leaving subroutine computePsfMatchingKernelForFootprint");
    
    return kernelSolution;
}


/** 
 * \brief Computes a single Kernel (Model 1) around a single subimage.
 *
 * Accepts two MaskedImages, generally subimages of a larger image, one of which
 * is to be convolved to match the other.  The output Kernel is generated using
 * an input list of basis Kernels by finding the coefficients in front of each
 * basis.  This version accepts an input variance image, and uses GSL for the
 * matrices.
 *
 * \return Vector of coefficients representing the relative contribution of
 * each input basis function.
 *
 * \return Differential background offset between the two images
 *
 * \ingroup diffim
 */
template <typename ImageT, typename MaskT>
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprintGSL(
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToConvolve, ///< Image to convolve
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &imageToNotConvolve, ///< Image to not convolve
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &varianceImage, ///< Model of variance per pixel
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList, ///< Input kernel basis set
    lsst::pex::policy::Policy &policy ///< Policy directing the behavior
    ) { 
    
    /* grab mask bits from the image to convolve, since that is what we'll be operating on */
    int edgeMaskBit = imageToConvolve.getMask()->getMaskPlane("EDGE");
    
    int nKernelParameters = 0;
    int nBackgroundParameters = 0;
    int nParameters = 0;

    boost::timer t;
    double time;
    t.restart();

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Entering subroutine computePsfMatchingKernelForFootprint");
    
    /* We assume that each kernel in the Set has 1 parameter you fit for */
    nKernelParameters = kernelInBasisList.size();
    /* Or, we just assume that across a single kernel, background 0th order.  This quite makes sense. */
    nBackgroundParameters = 1;
    /* Total number of parameters */
    nParameters = nKernelParameters + nBackgroundParameters;

    gsl_vector *B = gsl_vector_alloc (nParameters);
    gsl_matrix *M = gsl_matrix_alloc (nParameters, nParameters);
    
    gsl_vector_set_zero(B);
    gsl_matrix_set_zero(M);
    
    /* convolve creates a MaskedImage, push it onto the back of the Vector */
    /* need to use shared pointers because MaskedImage copy does not work */
    std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > > convolvedImageList(nKernelParameters);
    /* and an iterator over this */
    typename std::vector<boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > >::iterator citer = convolvedImageList.begin();
    
    /* Iterator for input kernel basis */
    std::vector<boost::shared_ptr<lsst::afw::math::Kernel> >::const_iterator kiter = kernelInBasisList.begin();
    /* Create C_ij in the formalism of Alard & Lupton */
    for (; kiter != kernelInBasisList.end(); ++kiter, ++citer) {
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolving an Object with Basis");
        
        /* NOTE : we could also *precompute* the entire template image convolved with these functions */
        /*        and save them somewhere to avoid this step each time.  however, our paradigm is to */
        /*        compute whatever is needed on the fly.  hence this step here. */
        boost::shared_ptr<lsst::afw::image::MaskedImage<ImageT, MaskT> > imagePtr(
            new lsst::afw::image::MaskedImage<ImageT, MaskT>
            (lsst::afw::math::convolveNew(imageToConvolve, **kiter, edgeMaskBit, false))
            );
        
        lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                      "Convolved an Object with Basis");
        
        *citer = imagePtr;
        
    } 
    
    /* NOTE ABOUT CONVOLUTION : */
    /* getCtrCol:getCtrRow pixels are masked on the left:bottom side */
    /* getCols()-getCtrCol():getRows()-getCtrRow() masked on right/top side */
    /* */
    /* The convolved image and the input image are by default the same size, so */
    /* we offset our initial pixel references by the same amount */
    kiter = kernelInBasisList.begin();
    citer = convolvedImageList.begin();
    unsigned int startCol = (*kiter)->getCtrCol();
    unsigned int startRow = (*kiter)->getCtrRow();
    /* NOTE - I determined I needed this +1 by eye */
    unsigned int endCol   = (*citer)->getCols() - ((*kiter)->getCols() - (*kiter)->getCtrCol()) + 1;
    unsigned int endRow   = (*citer)->getRows() - ((*kiter)->getRows() - (*kiter)->getCtrRow()) + 1;
    /* NOTE - we need to enforce that the input images are large enough */
    /* How about some multiple of the PSF FWHM?  Or second moments? */
    
    /* An accessor for each convolution plane */
    /* NOTE : MaskedPixelAccessor has no empty constructor, therefore we need to push_back() */
    std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorRowList;
    for (citer = convolvedImageList.begin(); citer != convolvedImageList.end(); ++citer) {
        convolvedAccessorRowList.push_back(lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT>(**citer));
    }
    
    /* An accessor for each input image; address rows and cols separately */
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveRow(imageToConvolve);
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveRow(imageToNotConvolve);
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> varianceRow(varianceImage);
    
    /* Take into account buffer for kernel images */
    imageToConvolveRow.advance(startCol, startRow);
    imageToNotConvolveRow.advance(startCol, startRow);
    varianceRow.advance(startCol, startRow);

    for (int ki = 0; ki < nKernelParameters; ++ki) {
        convolvedAccessorRowList[ki].advance(startCol, startRow);
    }

    for (unsigned int row = startRow; row < endRow; ++row) {
        
        /* An accessor for each convolution plane */
        std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorColList = convolvedAccessorRowList;
        
        /* An accessor for each input image; places the col accessor at the correct row */
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveCol = imageToConvolveRow;
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveCol = imageToNotConvolveRow;
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> varianceCol = varianceRow;

        for (unsigned int col = startCol; col < endCol; ++col) {
            
            ImageT ncCamera   = *imageToNotConvolveCol.image;
            ImageT ncVariance = *imageToNotConvolveCol.variance;
            MaskT  ncMask     = *imageToNotConvolveCol.mask;
            
            ImageT iVariance  = 1.0 / *varianceCol.variance;
            
            lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                          "Accessing image row %d col %d : %.3f %.3f %d",
                                          row, col, ncCamera, ncVariance, ncMask);
            
            /* kernel index i */
            typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator
                kiteri = convolvedAccessorColList.begin();
            
            for (int kidxi = 0; kiteri != convolvedAccessorColList.end(); ++kiteri, ++kidxi) {
                ImageT cdCamerai   = *kiteri->image;

                /* NOTE - Commenting in these additional pixel accesses yields
                 * an additional second of run-time per kernel with opt=1 at 2.8
                 * GHz*/

                /* ignore unnecessary pixel accesses 
                ImageT cdVariancei = *kiteri->variance;
                MaskT  cdMaski     = *kiteri->mask;
                lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                            "Accessing convolved image %d : %.3f %.3f %d",
                                            kidxi, cdCamerai, cdVariancei, cdMaski);
                */

                /* kernel index j  */
                typename std::vector<lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> >::iterator kiterj = kiteri;
                for (int kidxj = kidxi; kiterj != convolvedAccessorColList.end(); ++kiterj, ++kidxj) {
                    ImageT cdCameraj   = *kiterj->image;

                    /* ignore unnecessary pixel accesses 
                    ImageT cdVariancej = *kiterj->variance;
                    MaskT  cdMaskj     = *kiterj->mask;
                    lsst::pex::logging::TTrace<8>("lsst.ip.diffim.computePsfMatchingKernelForFootprint",
                                                "Accessing convolved image %d : %.3f %.3f %d",
                                                kidxj, cdCameraj, cdVariancej, cdMaskj);
                    */

                    gsl_matrix_set(M, kidxi, kidxj, 
                                   gsl_matrix_get(M, kidxi, kidxj) + cdCamerai * cdCameraj * iVariance);
                } 
             
                gsl_vector_set(B, kidxi,
                               gsl_vector_get(B, kidxi) + ncCamera * cdCamerai * iVariance);
                
                /* Constant background term; effectively j=kidxj+1 */
                gsl_matrix_set(M, kidxi, nParameters-1, 
                               gsl_matrix_get(M, kidxi, nParameters-1) + cdCamerai * iVariance);
            } 
            
            /* Background term; effectively i=kidxi+1 */
            gsl_vector_set(B, nParameters-1,
                           gsl_vector_get(B, nParameters-1) + ncCamera * iVariance);

            gsl_matrix_set(M, nParameters-1, nParameters-1, 
                           gsl_matrix_get(M, nParameters-1, nParameters-1) + 1.0 * iVariance);
            
            lsst::pex::logging::TTrace<7>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                          "Background terms : %.3f %.3f",
                                          gsl_vector_get(B, nParameters-1), 
                                          gsl_matrix_get(M, nParameters-1, nParameters-1));
            
            /* Step each accessor in column */
            imageToConvolveCol.nextCol();
            imageToNotConvolveCol.nextCol();
            varianceCol.nextCol();
            for (int ki = 0; ki < nKernelParameters; ++ki) {
                convolvedAccessorColList[ki].nextCol();
            }             
            
        } /* col */
        
        /* Step each accessor in row */
        imageToConvolveRow.nextRow();
        imageToNotConvolveRow.nextRow();
        varianceRow.nextRow();
        for (int ki = 0; ki < nKernelParameters; ++ki) {
            convolvedAccessorRowList[ki].nextRow();
        }
        
    } /* row */

    /* NOTE: If we are going to regularize the solution to M, this is the place to do it */
    
    /* Fill in rest of M */
    for (int kidxi=0; kidxi < nParameters; ++kidxi) 
        for (int kidxj=kidxi+1; kidxj < nParameters; ++kidxj) 
            gsl_matrix_set(M, kidxj, kidxi,
                           gsl_matrix_get(M, kidxi, kidxj));
    
#if DEBUG_MATRIX
    std::cout << "B : " << B << std::endl;
    std::cout << "M : " << M << std::endl;
#endif

    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time before matrix inversions : %.2f s", time);

    /* temporary matrices */
    //gsl_matrix *V    = gsl_matrix_alloc (nParameters, nParameters);
    //gsl_vector *S    = gsl_vector_alloc (nParameters);
    //gsl_vector *work = gsl_vector_alloc (nParameters);

    //gsl_matrix *MtM = gsl_matrix_alloc (nParameters, nParameters);
    //gsl_blas_dgemm(CblasTrans, CblasNoTrans,
    //1.0, M, M, 
    //0.0, MtM);

    
    gsl_multifit_linear_workspace *work = gsl_multifit_linear_alloc (nParameters, nParameters);
    gsl_vector *X                       = gsl_vector_alloc (nParameters);
    gsl_matrix *Cov                     = gsl_matrix_alloc (nParameters, nParameters);
    double chi2;
    size_t rank;
    /* This has too much other computation in there; take only what I need 
       gsl_multifit_linear_svd(M, B, GSL_DBL_EPSILON, &rank, X, Cov, &chi2, work);
    */
    gsl_matrix *A   = work->A;
    gsl_matrix *Q   = work->Q;
    gsl_matrix *QSI = work->QSI;
    gsl_vector *S   = work->S;
    gsl_vector *xt  = work->xt;
    gsl_vector *D   = work->D;
    gsl_matrix_memcpy (A, M);
    gsl_linalg_balance_columns (A, D);
    gsl_linalg_SV_decomp_mod (A, QSI, Q, S, xt);
    gsl_blas_dgemv (CblasTrans, 1.0, A, B, 0.0, xt);
    gsl_matrix_memcpy (QSI, Q);
    {
        double alpha0 = gsl_vector_get (S, 0);
        size_t p_eff = 0;

        const size_t p = M->size2;

        for (size_t j = 0; j < p; j++)
        {
            gsl_vector_view column = gsl_matrix_column (QSI, j);
            double alpha = gsl_vector_get (S, j);
            
            if (alpha <= GSL_DBL_EPSILON * alpha0) {
                alpha = 0.0;
            } else {
                alpha = 1.0 / alpha;
                p_eff++;
            }
            
            gsl_vector_scale (&column.vector, alpha);
        }
        
        rank = p_eff;
    }
    gsl_vector_set_zero (X);
    gsl_blas_dgemv (CblasNoTrans, 1.0, QSI, xt, 0.0, X);
    gsl_vector_div (X, D);
    gsl_multifit_linear_free(work);


    //gsl_linalg_SV_decomp(MtM, V, S, work); /* MtM -> U */
    //gsl_matrix * VT = gsl_matrix_alloc (nParameters, nParameters);
    //gsl_matrix_transpose_memcpy (VT, V);

    //gsl_matrix *Si   = gsl_matrix_alloc (nParameters, nParameters);
    //for (int i = 0; i < nParametrs; i++)
    //if (gsl_vector_get (S, i) > MIN_EIGVENVALUE)
    //gsl_matrix_set (Si, i, i, 1.0 / gsl_vector_get (S, i));

    //gsl_matrix * VSi = gsl_matrix_alloc (nParameters, nParameters);
    //gsl_blas_dgemm (CblasNoTrans, CblasNoTrans,
    //1.0, V, Si,
    //0.0, VSi);

    //gsl_matrix * pinv = gsl_matrix_alloc (nParameters, nParameters);
    //gsl_blas_dgemm (CblasTrans, CblasNoTrans,
    //1.0, MtM, VSi,
    //0.0, pinv);
    
    /* get solution using SVD */
    //gsl_vector *X    = gsl_vector_alloc (nParameters);
    //gsl_linalg_SV_decomp(M, V, S, work); /* M -> U */
    //gsl_vector_free(work);
    //gsl_linalg_SV_solve(M, V, S, B, X);  /* Solution in X */

    /* OR, Using LU Decomposition */
    /*
    gsl_permutation *P = gsl_permutation_alloc (nParameters);
    int s;
    gsl_linalg_LU_decomp (M, p, &s);  // M -> LU
    gsl_linalg_LU_solve (M, p, B, X);
    */
        



    /* Invert using VW's internal method */
    //vw::math::Vector<double> Soln = vw::math::least_squares(M, B);

    /* Additional gymnastics to get the parameter uncertainties */
     //vw::math::Matrix<double> Mt = vw::math::transpose(M);
     //vw::math::Matrix<double> MtM = Mt * M;
     //vw::math::Matrix<double> Error = vw::math::pseudoinverse(MtM);

    /*
      NOTE : for any real kernels I have looked at, these solutions have agreed
      exactly.  However, when designing the testDeconvolve unit test with
      hand-built gaussians as objects and non-realistic noise, the solutions did
      *NOT* agree.

      std::cout << "Soln : " << Soln << std::endl;
      std::cout << "Soln2 : " << Soln2 << std::endl;
    */

#if DEBUG_MATRIX
    std::cout << "Chi2 = " << chi2 << std::endl;
    //gsl_vector_fprintf(std::cout, X, '%.3f');
    //gsl_matrix_fprintf(std::cout, Cov, '%.3f');
#endif
    
    time = t.elapsed();
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Total compute time after matrix inversions : %.2f s", time);

    /* Translate into vector pair */
    std::vector<std::pair<double,double> > kernelSolution;
    for (int ki = 0; ki < nParameters; ++ki) {
        kernelSolution.push_back(std::make_pair<double,double>( gsl_vector_get(X, ki),
                                                                gsl_matrix_get(Cov, ki, ki)));
        //sqrt(gsl_matrix_get(Cov, ki, ki)) ));
    }

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.computePsfMatchingKernelForFootprint", 
                                  "Leaving subroutine computePsfMatchingKernelForFootprint");
    
    return kernelSolution;
}

/** 
 * @brief Checks a Mask image to see if a particular Mask plane is set.
 *
 * @return True if the mask is *not* set, False if it is.
 *
 * @ingroup diffim
 */
template <typename MaskT>
bool lsst::ip::diffim::maskOk(
    lsst::afw::image::Mask<MaskT> const &inputMask,
    MaskT const badPixelMask ///< Mask value for bad data
    )
{
    typename lsst::afw::image::Mask<MaskT>::pixel_accessor rowAcc = inputMask.origin();
    for (unsigned int row = 0; row < inputMask.getRows(); ++row, rowAcc.next_row()) {
        typename lsst::afw::image::Mask<MaskT>::pixel_accessor colAcc = rowAcc;
        for (unsigned int col = 0; col < inputMask.getCols(); ++col, colAcc.next_col()) {
            /*std::cout << "MASK " << (*colAcc) << " " << badPixelMask << " " << ((*colAcc) & badPixelMask) << std::endl; */
            
            if (((*colAcc) & badPixelMask) != 0) {
                return false;
            }
        }
    }
    return true;
}

/** 
 * @brief Calculates mean and unbiased variance of values (normalized by the
 * sqrt of the image variance) in a MaskedImage
 *
 * The pixel values in the image portion are normalized by the sqrt of the
 * variance.  The mean and variance of this distribution are calculated.  If
 * the MaskedImage is a difference image, the results should follow a
 * normal(0,1) distribution.
 *
 * @return Number of unmasked pixels in the image, and the mean and variance
 * of the residuals divided by the sqrt(variance).
 *
 * @ingroup diffim
 */
template <typename ImageT, typename MaskT>
void lsst::ip::diffim::calculateMaskedImageStatistics(
    int &nGoodPixels, ///< Number of good pixels in the image
    double &mean, ///< Mean value/variance; ideally 0 in a difference image
    double &variance, ///< Average variance of value/variance; ideally 1 in a difference image
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &inputImage, ///< Input image to be analyzed
    MaskT const badPixelMask ///< Mask for bad data
    ) {
    
    double x2Sum=0.0, xSum=0.0;
    
    nGoodPixels = 0;
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> rowAcc(inputImage);
    for (unsigned int row = 0; row < inputImage.getRows(); ++row, rowAcc.nextRow()) {
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> colAcc = rowAcc;
        for (unsigned int col = 0; col < inputImage.getCols(); ++col, colAcc.nextCol()) {
            if (((*colAcc.mask) & badPixelMask) == 0) {
                xSum  += (*colAcc.image) / sqrt(*colAcc.variance);
                x2Sum += (*colAcc.image) * (*colAcc.image) / (*colAcc.variance);

                nGoodPixels += 1;
            }
        }
    }
    
    if (nGoodPixels > 0) {
        mean = xSum / nGoodPixels;
    } else {
        mean = std::numeric_limits<double>::quiet_NaN();
    }
    if (nGoodPixels > 1) {
        variance  = x2Sum / nGoodPixels - mean*mean;
        variance *= nGoodPixels / (nGoodPixels - 1);
    } else {
        variance = std::numeric_limits<double>::quiet_NaN();
    }
    
}

/** 
 * @brief Calculates mean and unbiased variance of values (normalized by the
 * sqrt of the image variance) in a MaskedImage.  This version does not look for
 * particular mask bits, instead requiring Mask == 0.
 *
 * @return Number of unmasked pixels in the image, and the mean and variance
 * of the residuals divided by the sqrt(variance).
 *
 * @ingroup diffim
 */
template <typename ImageT, typename MaskT>
void lsst::ip::diffim::calculateMaskedImageStatistics(
    int &nGoodPixels, ///< Number of good pixels in the image
    double &mean, ///< Mean value/variance; ideally 0 in a difference image
    double &variance, ///< Average variance of value/variance; ideally 1 in a difference image
    lsst::afw::image::MaskedImage<ImageT, MaskT> const &inputImage ///< Input image to be analyzed
    ) {
    
    double x2Sum=0.0, xSum=0.0;
    
    nGoodPixels = 0;
    lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> rowAcc(inputImage);
    for (unsigned int row = 0; row < inputImage.getRows(); ++row, rowAcc.nextRow()) {
        lsst::afw::image::MaskedPixelAccessor<ImageT, MaskT> colAcc = rowAcc;
        for (unsigned int col = 0; col < inputImage.getCols(); ++col, colAcc.nextCol()) {
            if ((*colAcc.mask) == 0) {
                xSum  += (*colAcc.image) / sqrt(*colAcc.variance);
                x2Sum += (*colAcc.image) * (*colAcc.image) / (*colAcc.variance);

                nGoodPixels += 1;
            }
        }
    }
    
    if (nGoodPixels > 0) {
        mean = xSum / nGoodPixels;
    } else {
        mean = std::numeric_limits<double>::quiet_NaN();
    }
    if (nGoodPixels > 1) {
        variance  = x2Sum / nGoodPixels - mean*mean;
        variance *= nGoodPixels / (nGoodPixels - 1);
    } else {
        variance = std::numeric_limits<double>::quiet_NaN();
    }
    
}

/** 
 * @brief Calculates mean and variance of the values in an Image
 *
 * @return Number of pixels in the image, and the mean and variance of the
 * values.
 *
 * @ingroup diffim
 */
template <typename ImageT>
void lsst::ip::diffim::calculateImageStatistics(
    int &nGoodPixels, ///< Number of good pixels in the image
    double &mean, ///< Mean value; nan if nGoodPixels < 1
    double &variance, ///< Variance of values; nan if nGoodPixels < 2
    lsst::afw::image::Image<ImageT> const &inputImage ///< Input image to be analyzed
    ) {
    
    double x2Sum=0.0, xSum=0.0, wSum=0.0;
    
    nGoodPixels = 0;
    typedef typename vw::ImageView<ImageT>::pixel_accessor imageAccessorType;
    
    imageAccessorType imageAccessorCol(inputImage.origin());
    for (unsigned int col = 0; col < inputImage.getCols(); ++col) {
        
        imageAccessorType imageAccessorRow(imageAccessorCol);
        for (unsigned int row = 0; row < inputImage.getRows(); ++row) {
            nGoodPixels += 1;
            x2Sum       += (*imageAccessorRow) * (*imageAccessorRow);
            xSum        += (*imageAccessorRow);
            wSum        += 1;
            imageAccessorRow.next_row();
        }
        imageAccessorCol.next_col();
    }
    
    if (nGoodPixels > 0) {
        mean = xSum / wSum;
    } else {
        mean = std::numeric_limits<double>::quiet_NaN();
    }
    if (nGoodPixels > 1) {
        variance  = x2Sum / wSum - mean*mean;
        variance *= nGoodPixels / (nGoodPixels - 1);
        variance /= nGoodPixels;
    } else {
        variance = std::numeric_limits<double>::quiet_NaN();
    }
}

/** 
 * @brief Calculates mean and variance of the values in a vector
 *
 * @return The mean and variance of the values.
 *
 * @ingroup diffim
 */
template <typename VectorT>
void lsst::ip::diffim::calculateVectorStatistics(
    vw::math::Vector<VectorT> const &inputVector,
    double &mean,
    double &variance
    ) {
    
    double x2Sum = 0.0, xSum = 0.0, wSum = 0.0;
    for (unsigned int i = 0; i < inputVector.size(); ++i) {
        x2Sum += inputVector[i] * inputVector[i];
        xSum  += inputVector[i];
        wSum  += 1;
    }

    if (wSum > 0) {
        mean      = xSum / wSum;
    } else {
        mean = std::numeric_limits<double>::quiet_NaN();
    } 
    if (wSum > 1) {
        variance  = x2Sum / wSum - mean*mean;
        variance *= wSum / (wSum - 1);
    } else {
        variance = std::numeric_limits<double>::quiet_NaN();
    }
}

/** 
 * @brief Adds a Function to an Image
 *
 * @ingroup diffim
 */
template <typename PixelT, typename FunctionT>
void lsst::ip::diffim::addFunctionToImage(
    lsst::afw::image::Image<PixelT> &image, ///< image
    lsst::afw::math::Function2<FunctionT> const &function ///< 2-d function
) {
    typedef typename lsst::afw::image::Image<PixelT>::pixel_accessor imageAccessorType;
    unsigned int numCols = image.getCols();
    unsigned int numRows = image.getRows();
    imageAccessorType imRow = image.origin();
    for (unsigned int row = 0; row < numRows; ++row, imRow.next_row()) {
        imageAccessorType imCol = imRow;
        double rowPos = lsst::afw::image::positionToIndex(row);
        for (unsigned int col = 0; col < numCols; ++col, imCol.next_col()) {
            double colPos = lsst::afw::image::positionToIndex(col);
            *imCol += static_cast<PixelT>(function(colPos, rowPos));
        }
    }
}

/************************************************************************************************************/
/* Explicit instantiations */

template class lsst::ip::diffim::DifferenceImageStatistics<float, lsst::afw::image::maskPixelType>;
template class lsst::ip::diffim::DifferenceImageStatistics<double, lsst::afw::image::maskPixelType>;
template class lsst::ip::diffim::DifferenceImageFootprintInformation<float, lsst::afw::image::maskPixelType>;
template class lsst::ip::diffim::DifferenceImageFootprintInformation<double, lsst::afw::image::maskPixelType>;

template 
lsst::ip::diffim::DifferenceImageFootprintInformation<float, lsst::afw::image::maskPixelType>::DifiList
lsst::ip::diffim::getGoodFootprints( 
    std::vector<boost::shared_ptr<DifferenceImageFootprintInformation<float, lsst::afw::image::maskPixelType> > > &difiList
    );

template 
lsst::ip::diffim::DifferenceImageFootprintInformation<double, lsst::afw::image::maskPixelType>::DifiList
lsst::ip::diffim::getGoodFootprints( 
    std::vector<boost::shared_ptr<DifferenceImageFootprintInformation<double, lsst::afw::image::maskPixelType> > > &difiList
    );

template 
lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> lsst::ip::diffim::convolveAndSubtract(
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    boost::shared_ptr<lsst::afw::math::Kernel> const &convolutionKernelPtr,
    double background);

template 
lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> lsst::ip::diffim::convolveAndSubtract(
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    boost::shared_ptr<lsst::afw::math::Kernel> const &convolutionKernelPtr,
    double background);

template
std::vector<double> lsst::ip::diffim::computePsfMatchingKernelForFootprint(
    double &background,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<double> lsst::ip::diffim::computePsfMatchingKernelForFootprint(
    double &background,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprint2(
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &varianceImage,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprint2(
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &varianceImage,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprintGSL(
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &varianceImage,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<std::pair<double,double> > lsst::ip::diffim::computePsfMatchingKernelForFootprintGSL(
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &varianceImage,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> const &kernelInBasisList,
    lsst::pex::policy::Policy &policy);

template
std::vector<lsst::detection::Footprint::PtrType> lsst::ip::diffim::getCollectionOfFootprintsForPsfMatching(
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::pex::policy::Policy &policy);

template
std::vector<lsst::detection::Footprint::PtrType> lsst::ip::diffim::getCollectionOfFootprintsForPsfMatching(
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToConvolve,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &imageToNotConvolve,
    lsst::pex::policy::Policy &policy);

template
bool lsst::ip::diffim::maskOk(
    lsst::afw::image::Mask<lsst::afw::image::maskPixelType> const &inputMask,
    lsst::afw::image::maskPixelType const badPixelMask);

template
void lsst::ip::diffim::calculateMaskedImageStatistics(
    int &nGoodPixels,
    double &mean,
    double &variance,
    lsst::afw::image::MaskedImage<float, lsst::afw::image::maskPixelType> const &inputImage,
    lsst::afw::image::maskPixelType const badPixelMask);

template
void lsst::ip::diffim::calculateMaskedImageStatistics(
    int &nGoodPixels,
    double &mean,
    double &variance,
    lsst::afw::image::MaskedImage<double, lsst::afw::image::maskPixelType> const &inputImage,
    lsst::afw::image::maskPixelType const badPixelMask);

template
void lsst::ip::diffim::calculateImageStatistics(
    int &nGoodPixels,
    double &mean,
    double &variance,
    lsst::afw::image::Image<float> const &inputImage);

template
void lsst::ip::diffim::calculateImageStatistics(
    int &nGoodPixels,
    double &mean,
    double &variance,
    lsst::afw::image::Image<double> const &inputImage);

template
void lsst::ip::diffim::addFunctionToImage(
    lsst::afw::image::Image<float>&,
    lsst::afw::math::Function2<float> const&);

template
void lsst::ip::diffim::addFunctionToImage(
    lsst::afw::image::Image<float>&,
    lsst::afw::math::Function2<double> const&);

template
void lsst::ip::diffim::addFunctionToImage(
    lsst::afw::image::Image<double>&,
    lsst::afw::math::Function2<float> const&);

template
void lsst::ip::diffim::addFunctionToImage(
    lsst::afw::image::Image<double>&,
    lsst::afw::math::Function2<double> const&);



/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/
/* SpatialModelCell stuff*/

template <typename ImageT, typename MaskT>
lsst::ip::diffim::KernelModelQa<ImageT, MaskT>::KernelModelQa(
    lsst::detection::Footprint::PtrType fpPtr,
    MaskedImagePtr miToConvolveParentPtr,
    MaskedImagePtr miToNotConvolveParentPtr,
    lsst::afw::math::KernelList<lsst::afw::math::Kernel> kBasisList,
    lsst::pex::policy::Policy &policy,
    bool build
    ) :
    _miToConvolveParentPtr(miToConvolveParentPtr),
    _miToNotConvolveParentPtr(miToNotConvolveParentPtr),
    _kBasisList(kBasisList),
    _policy(policy),
    _fpPtr(fpPtr),
    _isBuilt(false),
    _isGood(false)
{
    if (build == true) {
        this->buildModel();
    }
}

template <typename ImageT, typename MaskT>
bool lsst::ip::diffim::KernelModelQa<ImageT, MaskT>::buildModel() {

    typedef lsst::afw::image::Image<double>::pixel_accessor pixelAccessor;

    if (this->_isBuilt == true) {
        return false;
    }

    /* fill in information on position in the image */
    vw::BBox2i   fpBBox = this->_fpPtr->getBBox();
    vw::Vector2i fpMin  = fpBBox.min();
    vw::Vector2i fpMax  = fpBBox.max();
    this->_colcNorm = float(fpMin.x() + fpMax.x()) / this->_miToConvolveParentPtr->getCols() - 1.0;
    this->_rowcNorm = float(fpMin.y() + fpMax.y()) / this->_miToConvolveParentPtr->getRows() - 1.0; 

    lsst::pex::logging::TTrace<4>("lsst.ip.diffim.KernelModelQa.buildModel",
                                  "Footprint = %d,%d -> %d,%d",
                                  fpBBox.min().x(), fpBBox.min().y(),
                                  fpBBox.max().x(), fpBBox.max().y());

    /* fill in information on the actual pixels used */
    MaskedImagePtr miToConvolvePtr    = this->_miToConvolveParentPtr->getSubImage(fpBBox);
    MaskedImagePtr miToNotConvolvePtr = this->_miToNotConvolveParentPtr->getSubImage(fpBBox);
    this->_miToConvolvePtr = miToConvolvePtr;
    this->_miToNotConvolvePtr = miToNotConvolvePtr;

    /* estimate of the variance for first kernel pass */
    lsst::afw::image::MaskedImage<ImageT, MaskT> varEstimate = 
        lsst::afw::image::MaskedImage<ImageT, MaskT>(this->_miToConvolvePtr->getCols(), 
                                                     this->_miToConvolvePtr->getRows());
    varEstimate += *(this->_miToNotConvolvePtr);
    varEstimate -= *(this->_miToConvolvePtr);
    
    std::vector<std::pair<double,double> > kSolution;
    try {
        kSolution = computePsfMatchingKernelForFootprint2(*(this->_miToConvolvePtr), 
                                                          *(this->_miToNotConvolvePtr), 
                                                          varEstimate, this->_kBasisList, this->_policy);
    } catch (lsst::pex::exceptions::ExceptionStack &e) {
        this->_isGood = false;
        return false;
    }

    /* Create fixed Kernel and KernelError from solution */
    unsigned int kCols = this->_policy.getInt("kernelCols");
    unsigned int kRows = this->_policy.getInt("kernelRows");
    vector<double> kValues(kCols*kRows);
    vector<double> kErrValues(kCols*kRows);
    for (unsigned int row = 0, idx = 0; row < kRows; row++) {
        for (unsigned int col = 0; col < kCols; col++, idx++) {
            kValues[idx]    = kSolution[idx].first;
            kErrValues[idx] = kSolution[idx].second;
        }
    }
    boost::shared_ptr<lsst::afw::math::Kernel> kPtr (
        new lsst::afw::math::LinearCombinationKernel(this->_kBasisList, kValues)
        );
    boost::shared_ptr<lsst::afw::math::Kernel> kErrPtr (
        new lsst::afw::math::LinearCombinationKernel(this->_kBasisList, kErrValues)
        );
    
    double kSum = 0.;
    lsst::afw::image::Image<double> kImage = 
        lsst::afw::image::Image<double>(kCols, kRows);
    kImage = kPtr->computeNewImage(kSum, false);

    /* Find estimate of Background and Background Error */
    double bg    = kSolution[kCols*kRows].first;
    double bgErr = kSolution[kCols*kRows].second;

    lsst::afw::image::MaskedImage<ImageT, MaskT> diffIm = convolveAndSubtract( *(this->_miToConvolvePtr),
                                                                               *(this->_miToNotConvolvePtr),
                                                                               kPtr, bg);
    lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT> kStats1 = 
        lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>(diffIm);

    lsst::pex::logging::TTrace<6>("lsst.ip.diffim.KernelModelQa.buildModel",
                                  "Kernel1 : Kernel Sum = %.3f; Diffim residuals = %.2f +/- %.2f sigma",
                                  kSum, 
                                  kStats1.getResidualMean(),
                                  kStats1.getResidualStd());

    bool iterateKernel = this->_policy.getBool("iterateKernel");
    if (iterateKernel) {
        try {
            try {
                kSolution = computePsfMatchingKernelForFootprint2(*(this->_miToConvolvePtr), 
                                                                  *(this->_miToNotConvolvePtr), 
                                                                  diffIm, this->_kBasisList, this->_policy);
            } catch (lsst::pex::exceptions::ExceptionStack &e) {
                throw;
            }
            
            /* If computePsfMatchingKernelForFootprint2 does not return an
             * exception we continue on here 
             */
            std::fill(kValues.begin(), kValues.end(), 0.);
            std::fill(kErrValues.begin(), kErrValues.end(), 0.);
            for (unsigned int row = 0, idx = 0; row < kRows; row++) {
                for (unsigned int col = 0; col < kCols; col++, idx++) {
                    kValues[idx]    = kSolution[idx].first;
                    kErrValues[idx] = kSolution[idx].second;
                }
            }
            boost::shared_ptr<lsst::afw::math::Kernel> kPtr (
                new lsst::afw::math::LinearCombinationKernel(this->_kBasisList, kValues)
                );
            boost::shared_ptr<lsst::afw::math::Kernel> kErrPtr (
                new lsst::afw::math::LinearCombinationKernel(this->_kBasisList, kErrValues)
                );
            kSum    = 0.;
            kImage  = kPtr->computeNewImage(kSum, false);
            bg      = kSolution[kCols*kRows].first;
            bgErr   = kSolution[kCols*kRows].second;
            
            diffIm  = convolveAndSubtract( *(this->_miToConvolvePtr),
                                           *(this->_miToNotConvolvePtr),
                                           kPtr, bg);

            lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT> kStats2 = 
                lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>(diffIm);
            lsst::pex::logging::TTrace<6>("lsst.ip.diffim.KernelModelQa.buildModel",
                                          "Kernel2 : Kernel Sum = %.3f; Diffim residuals = %.2f +/- %.2f sigma",
                                          kSum, 
                                          kStats2.getResidualMean(),
                                          kStats2.getResidualStd());
            
        } catch (lsst::pex::exceptions::ExceptionStack &e) {
            /* Use the first solution */
            ;
        }
    }
        
    this->_kPtr    = kPtr;
    this->_kErrPtr = kErrPtr;
    this->_kSum    = kSum;
    this->_bg      = bg;
    this->_bgErr   = bgErr;

    lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT> kStats = 
        lsst::ip::diffim::DifferenceImageStatistics<ImageT, MaskT>(diffIm);

    this->_kStats  = kStats;
    this->_isGood  = kStats.evaluateQuality(this->_policy);
    lsst::pex::logging::TTrace<4>("lsst.ip.diffim.KernelModelQa.buildModel",
                                  "Kernel : Kernel Sum = %.3f; Diffim residuals = %.2f +/- %.2f sigma",
                                  this->_kSum, 
                                  this->_kStats.getResidualMean(),
                                  this->_kStats.getResidualStd());

    this->_isBuilt = true;

    /* Should this return the Quality of what was built, or just the fact that
     * it was built? */

    /* Screw it; if it builds a bad instance, let the other code know */
    return this->_isGood;
}

template <typename ImageT, typename MaskT>
double lsst::ip::diffim::KernelModelQa<ImageT, MaskT>::returnRating() {
    return this->_kStats.getResidualMean();
}

template <typename ImageT, typename MaskT, class ModelT>
lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::SpatialModelCell(
    std::string label,
    std::vector<lsst::detection::Footprint::PtrType> fpPtrList,
    std::vector<ModelT> modelPtrList) :
    lsst::daf::data::LsstBase(typeid(this)),
    _label(label),
    _colC(0),
    _rowC(0),
    _fpPtrList(fpPtrList),
    _modelPtrList(modelPtrList),
    _nModels(fpPtrList.size()),
    _currentID(-1),
    _modelIsFixed(false)
{
    if (!(fpPtrList.size() == modelPtrList.size())) {
        throw lsst::pex::exceptions::DomainError("SpatialModelCell : footprint list and model list are not the same size");
    }
    lsst::pex::logging::TTrace<5>("lsst.ip.diffim.SpatialModelCell.SpatialModelCell", 
                                  "%s : %d Footprints found", this->_label.c_str(), this->_nModels);
    this->_orderFootprints();
    this->increment();
}

template <typename ImageT, typename MaskT, class ModelT>
lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::SpatialModelCell(
    std::string label,
    int colC, int rowC, 
    std::vector<lsst::detection::Footprint::PtrType> fpPtrList,
    std::vector<ModelT> modelPtrList) :
    lsst::daf::data::LsstBase(typeid(this)),
    _label(label),
    _colC(colC),
    _rowC(rowC),
    _fpPtrList(fpPtrList),
    _modelPtrList(modelPtrList),
    _nModels(fpPtrList.size()),
    _currentID(-1),
    _modelIsFixed(false)
{
    if (!(fpPtrList.size() == modelPtrList.size())) {
        throw lsst::pex::exceptions::DomainError("SpatialModelCell : footprint list and model list are not the same size");
    }
    lsst::pex::logging::TTrace<3>("lsst.ip.diffim.SpatialModelCell.SpatialModelCell", 
                                  "%s : %d Footprints found", this->_label.c_str(), this->_nModels);
    this->_orderFootprints();
    this->increment();
}
    
template <typename ImageT, typename MaskT, class ModelT>
void lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::_orderFootprints() {
    /* do something with this->_fpPtrList; make sure _modelPtrList is updated also */
    for (int i = 0; i < this->_nModels; i++) {
        this->_modelPtrList[i]->setID(i);
    }
}

template <typename ImageT, typename MaskT, class ModelT>
void lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::selectBestModel(bool fix) {
    /* For now just choose the first statistically acceptable one */

    /* HOWEVER, WATCH OUT BECAUSE YOU SET ISGOOD BY HAND WHEN ITS BAD */
    /* NEED TO REVERT TO THE RAW QUALITY NUMBERS HERE */

    /* PERHAPS THE CALLING CODE COULD JUST SAY "ITERATE" INSTEAD OF MARKING AS
       BAD AND THEN ITERATING */

    bool found = false;
    for (int i = 0; i < this->_nModels; i++) {
        if (this->_modelPtrList[i]->isGood()) {
            this->_currentID = i;
            found = true;
            break;
        }
    }
    if (found == false) {
        /* There are no good models! */
        this->_currentID    = -1;
        this->_modelIsFixed = true;
        lsst::pex::logging::TTrace<4>("lsst.ip.diffim.SpatialModelCell.selectBestModel", 
                                      "%s : Locking with no good models", this->_label.c_str());
    }
    else {
        this->_modelIsFixed = fix;
    }
}

template <typename ImageT, typename MaskT, class ModelT>
bool lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::isUsable() {
    /* I should make this less indirect, but if my _currentID == -1 and
       modelIsFixed == true this is unusable */

    if ( (this->_currentID == -1) && (this->_modelIsFixed) ) {
        return false;
    }
    return true;
}

template <typename ImageT, typename MaskT, class ModelT>
lsst::detection::Footprint::PtrType lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::getFootprint(int i) {
    if ( (i < 0) || (i >= this->_nModels) ) {
        throw lsst::pex::exceptions::DomainError("Index out of range");
    }        
    return this->_fpPtrList[i];
}

template <typename ImageT, typename MaskT, class ModelT>
ModelT lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::getModel(int i) {
    if ( (i < 0) || (i >= this->_nModels) ) {
        throw lsst::pex::exceptions::DomainError("Index out of range");
    }        
    return this->_modelPtrList[i];
}

template <typename ImageT, typename MaskT, class ModelT>
bool lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::increment() {
    /* If the Cell has a fixed Kernel */
    if (this->_modelIsFixed) {
        return false;
    }

    if (this->_currentID == -1) {
        /* Its the first time through */

        if ((this->_nModels) == 0) {
            /* There are 0 Footprints */
            this->_modelIsFixed = true;
            return false;
        }
        else {
            /* There are at least 1 */
            this->setCurrentID(0);
            return true;
        }            
    }
    else {
        if ( (this->_currentID) == ((this->_nModels) - 1) ) {
            /* You are at the last one */
            this->selectBestModel(true);
            return false;
        }
        else {
            /* Standard increment */
            this->setCurrentID(this->_currentID + 1);
            return true;
        }
    }
}

template <typename ImageT, typename MaskT, class ModelT>
void lsst::ip::diffim::SpatialModelCell<ImageT, MaskT, ModelT>::setCurrentID(int id) {
    if ( (id < 0) || (id >= this->_nModels) ) {
        throw lsst::pex::exceptions::DomainError("Index out of range");
    }        

    this->_currentID = id;
    lsst::pex::logging::TTrace<4>("lsst.ip.diffim.SpatialModelCell.setCurrentID", 
                                  "%s : Footprint %d / %d", this->_label.c_str(), this->_currentID+1, this->_nModels);

    /* If the model does not build for some reason, move on to the next
     * option */
    if (! (this->_modelPtrList[this->_currentID]->buildModel()) ) 
        this->increment();
}


template class lsst::ip::diffim::KernelModelQa<float, lsst::afw::image::maskPixelType>;
template class lsst::ip::diffim::KernelModelQa<double, lsst::afw::image::maskPixelType>;

template class lsst::ip::diffim::SpatialModelCell<float, lsst::afw::image::maskPixelType, 
                                                  lsst::ip::diffim::KernelModelQa<float, lsst::afw::image::maskPixelType>::Ptr >;
template class lsst::ip::diffim::SpatialModelCell<double, lsst::afw::image::maskPixelType, 
                                                  lsst::ip::diffim::KernelModelQa<double, lsst::afw::image::maskPixelType>::Ptr >;
