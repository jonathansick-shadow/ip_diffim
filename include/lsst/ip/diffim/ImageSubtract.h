// -*- lsst-c++ -*-
/**
 * @file ImageSubtract.h
 *
 * @brief Image Subtraction helper functions
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */

#ifndef LSST_IP_DIFFIM_IMAGESUBTRACT_H
#define LSST_IP_DIFFIM_IMAGESUBTRACT_H

#include <vector>
#include <string>

#include <Eigen/Core>

#include <boost/shared_ptr.hpp>

#include <lsst/pex/policy/Policy.h>
#include <lsst/afw/math/Kernel.h>
#include <lsst/afw/math/KernelFunctions.h>
#include <lsst/afw/math/Function.h>
#include <lsst/afw/math/SpatialCell.h>
#include <lsst/afw/image/Mask.h>
#include <lsst/afw/image/MaskedImage.h>
#include <lsst/afw/detection/Footprint.h>

namespace lsst {
namespace ip {
namespace diffim {

    
    /** Mask plane definitions */
    std::string const diffimStampCandidateStr = "DIFFIM_STAMP_CANDIDATE";
    std::string const diffimStampUsedStr      = "DIFFIM_STAMP_USED";
    
    /** Uses a functor to accumulate Mask bits
     *
     * @ingroup diffim
     *
     * @note Search through a footprint for any set mask fits.
     * 
     * @note May need to modify this as our mask planes evolve to include
     * non-bad mask information
     *
     * Example usage : 
     *  FindSetBits<image::Mask<image::MaskPixel> > count(mask); 
     *  count.reset(); 
     *  count.apply(footprint); 
     *  nSet = count.getBits();
     * 
     */
    template <typename MaskT>
    class FindSetBits : public lsst::afw::detection::FootprintFunctor<MaskT> {
    public:
        FindSetBits(MaskT const& mask) : 
            lsst::afw::detection::FootprintFunctor<MaskT>(mask), _bits(0) {;}
        
        void operator()(typename MaskT::xy_locator loc, ///< locator pointing at the pixel
                        int x,                          ///< column-position of pixel
                        int y                           ///< row-position of pixel
            ) {
            _bits |= *loc;
        }
        
        // Return the bits set
        typename MaskT::Pixel getBits() const { return _bits; }
        
        // Clear the accumulator
        void reset() { _bits = 0; }
        
    private:
        typename MaskT::Pixel _bits;
    };

    /** Uses a functor to calculate difference image statistics
     *
     * @ingroup diffim
     *
     * @note Looks like this is (almost) implemented in lsst/afw/math/Statistics.h
     * 
     * @note Find mean and unbiased variance of pixel residuals in units of
     * sqrt(variance)
     * 
     */
    template <typename PixelT>
    class ImageStatistics {
    public:
        typedef boost::shared_ptr<ImageStatistics> Ptr;
        typedef typename lsst::afw::image::MaskedImage<PixelT>::x_iterator x_iterator;

        ImageStatistics() : 
            _xsum(0.), _x2sum(0.), _npix(0) {} ;
        virtual ~ImageStatistics() {} ;

        // Clear the accumulators
        void reset() { _xsum = _x2sum = 0.; _npix = 0;}

        // Work your magic
        void apply(lsst::afw::image::MaskedImage<PixelT> const& image) {
            reset();
            for (int y = 0; y != image.getHeight(); ++y) {
                for (x_iterator ptr = image.row_begin(y), end = image.row_end(y); ptr != end; ++ptr) {
                    if ((*ptr).mask() == 0) {
                        double const ivar = 1. / (*ptr).variance();
                        _xsum  += (*ptr).image() * sqrt(ivar);
                        _x2sum += (*ptr).image() * (*ptr).image() * ivar;
                        _npix  += 1;
                    }
                }
            }
        }
        
        // Mean of distribution
        double getMean() const { 
            return (_npix > 0) ? _xsum/_npix : std::numeric_limits<double>::quiet_NaN(); 
        }
        // Variance of distribution 
        double getVariance() const { 
            return (_npix > 1) ? (_x2sum/_npix - _xsum/_npix * _xsum/_npix) * _npix/(_npix-1.) : std::numeric_limits<double>::quiet_NaN(); 
        }
        // RMS
        double getRms() const { 
            return sqrt(getVariance());
        }
        // Return the number of good pixels
        int getNpix() const { return _npix; }

        // Return Sdqa rating
        bool evaluateQuality(lsst::pex::policy::Policy const& policy) {
            if ( fabs(getMean())     > policy.getDouble("maximumFootprintResidualMean") ) return false;
            if ( getRms()            > policy.getDouble("maximumFootprintResidualStd")  ) return false;
            return true;
        }           
        
    private:
        double _xsum;
        double _x2sum;
        int    _npix;
    };


    /* Build a set of Delta Function basis kernels
     * 
     * @note Total number of basis functions is width*height
     * 
     * @param width  Width of basis set (cols)
     * @param height Height of basis set (rows)
     */    
    lsst::afw::math::KernelList generateDeltaFunctionBasisSet(
        unsigned int width,
        unsigned int height
        );

    /* Build a regularization matrix for Delta function kernels
     * 
     * @param width            Width of basis set you want to regularize
     * @param height           Height of basis set you want to regularize
     * @param order            Which derivative you expect to be smooth (derivative order+1 is penalized) 
     * @param boundary_style   0 = unwrapped, 1 = wrapped, 2 = order-tappered ('order' is highest used) 
     * @param difference_style 0 = forward, 1 = central
     * @param printB           debugging
     */    
    boost::shared_ptr<Eigen::MatrixXd> generateFiniteDifferenceRegularization(
        unsigned int width,
        unsigned int height,
        unsigned int order,
	unsigned int boundary_style = 1, 
	unsigned int difference_style = 0,
	bool printB=false
        );

    /** Renormalize a list of basis kernels
     *
     * @note Renormalization means make Ksum_0 = 1.0, Ksum_i = 0.0, K_i.dot.K_i = 1.0
     * @note Output list of shared pointers to FixedKernels
     *
     * @param kernelListIn input list of basis kernels
     */
    lsst::afw::math::KernelList renormalizeKernelList(lsst::afw::math::KernelList const &kernelListIn);

    /** Build a set of Alard/Lupton basis kernels
     *
     * @note Should consider implementing as SeparableKernels for additional speed,
     * but this will make the normalization a bit more complicated
     * 
     * @param halfWidth  size is 2*N + 1
     * @param nGauss     number of gaussians
     * @param sigGauss   Widths of the Gaussian Kernels
     * @param degGauss   Local spatial variation of bases
     */    
    lsst::afw::math::KernelList generateAlardLuptonBasisSet(
        unsigned int halfWidth,                ///< size is 2*N + 1
        unsigned int nGauss,                   ///< number of gaussians
        std::vector<double> const& sigGauss,   ///< width of the gaussians
        std::vector<int>    const& degGauss    ///< local spatial variation of gaussians
        );

    /*
     * Execute fundamental task of convolving template and subtracting it from science image
     */
    template <typename PixelT, typename BackgroundT>
    lsst::afw::image::MaskedImage<PixelT> convolveAndSubtract(
        lsst::afw::image::MaskedImage<PixelT> const& imageToConvolve,
        lsst::afw::image::MaskedImage<PixelT> const& imageToNotConvolve,
        lsst::afw::math::Kernel const& convolutionKernel,
        BackgroundT background,
        bool invert=true
        );

    template <typename PixelT, typename BackgroundT>
    lsst::afw::image::MaskedImage<PixelT> convolveAndSubtract(
        lsst::afw::image::Image<PixelT> const& imageToConvolve,
        lsst::afw::image::MaskedImage<PixelT> const& imageToNotConvolve,
        lsst::afw::math::Kernel const& convolutionKernel,
        BackgroundT background,
        bool invert=true
        );

    /** Search through images for Footprints with no masked pixels
     *
     * @note Uses Eigen math backend
     *
     * @param imageToConvolve  MaskedImage to convolve with Kernel
     * @param imageToNotConvolve  MaskedImage to subtract convolved template from
     * @param policy  Policy for operations; in particular object detection
     */    
    template <typename PixelT>
    std::vector<lsst::afw::detection::Footprint::Ptr> getCollectionOfFootprintsForPsfMatching(
        lsst::afw::image::MaskedImage<PixelT> const& imageToConvolve,
        lsst::afw::image::MaskedImage<PixelT> const& imageToNotConvolve,
        lsst::pex::policy::Policy             const& policy
        );

    template <typename PixelT>
    Eigen::MatrixXd imageToEigenMatrix(
        lsst::afw::image::Image<PixelT> const& img
        );
    
    /** Functor to create PSF Matching Kernel
     *
     * @ingroup diffim
     *
     * @note This class owns the functionality to make a single difference
     * imaging kernel around one object realized in 2 different images.  If
     * constructed with a regularization matrix, will use it by default.  This
     * creates the M and B vectors that are used to solve for the kernel
     * parameters 'x' as in Mx = B.  This creates a single kernel around a
     * single object, and operates in tandem with the KernelCandidate +
     * BuildSingleKernelVisitor classes for the spatial modeling.
     * 
     */
    template <typename PixelT, typename VarT=lsst::afw::image::VariancePixel>
    class PsfMatchingFunctor {
    public:
        typedef boost::shared_ptr<PsfMatchingFunctor> Ptr;
        typedef typename lsst::afw::image::MaskedImage<PixelT>::xy_locator xy_locator;
        typedef typename lsst::afw::image::Image<VarT>::xy_locator         xyi_locator;

        PsfMatchingFunctor(
            lsst::afw::math::KernelList const& basisList
            );
        PsfMatchingFunctor(
            lsst::afw::math::KernelList const& basisList,
            boost::shared_ptr<Eigen::MatrixXd> const& H
            );
        virtual ~PsfMatchingFunctor() {};

        /* Shallow copy only; shared matrix product uninitialized */
        PsfMatchingFunctor(const PsfMatchingFunctor<PixelT,VarT> &rhs);

        std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> getSolution();
        std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> getSolutionUncertainty();
        
        /** Access to least squares info
         */
        std::pair<boost::shared_ptr<Eigen::MatrixXd>, boost::shared_ptr<Eigen::VectorXd> > getAndClearMB();

        /** Access to basis list
         */
        lsst::afw::math::KernelList getBasisList() const { return _basisList; }

        /* Create PSF matching kernel */
        void apply(lsst::afw::image::Image<PixelT> const& imageToConvolve,
                   lsst::afw::image::Image<PixelT> const& imageToNotConvolve,
                   lsst::afw::image::Image<VarT>   const& varianceEstimate,
                   lsst::pex::policy::Policy       const& policy
            );

    protected:
        lsst::afw::math::KernelList const _basisList;            ///< List of Kernel basis functions
        boost::shared_ptr<Eigen::MatrixXd> _M;                   ///< Least squares matrix
        boost::shared_ptr<Eigen::VectorXd> _B;                   ///< Least squares vector
        boost::shared_ptr<Eigen::VectorXd> _Soln;                ///< Least square solution
        boost::shared_ptr<Eigen::MatrixXd> const _H;             ///< Regularization matrix
        bool _initialized;                                       ///< Has been solved for
        bool _regularize;                                        ///< Has a _H matrix
    };
    
    template <typename PixelT>
    typename PsfMatchingFunctor<PixelT>::Ptr
    makePsfMatchingFunctor(lsst::afw::math::KernelList const& basisList) {
        return typename PsfMatchingFunctor<PixelT>::Ptr(new PsfMatchingFunctor<PixelT>(basisList));
    }

    template <typename PixelT>
    typename PsfMatchingFunctor<PixelT>::Ptr
    makePsfMatchingFunctor(lsst::afw::math::KernelList const& basisList,
                           boost::shared_ptr<Eigen::MatrixXd> const H) {
        return typename PsfMatchingFunctor<PixelT>::Ptr(new PsfMatchingFunctor<PixelT>(basisList, H));
    }

    template <typename PixelT, typename FunctionT>
    void addSomethingToImage(lsst::afw::image::Image<PixelT> &image,
                             FunctionT const &function);

    template <typename PixelT>
    void addSomethingToImage(lsst::afw::image::Image<PixelT> &image,
                             double value);

}}}

#endif



