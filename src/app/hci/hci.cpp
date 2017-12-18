/*! -*-c++-*-
  @file   hci.cpp
  @author David Hirvonen
  @brief  Face and eye tracking, optical flow, corner detection, etc.

  \copyright Copyright 2014-2016 Elucideye, Inc. All rights reserved.
  \license{This project is released under the 3 Clause BSD License.}

*/

// Local includes:
#include "drishti/core/drishti_stdlib_string.h" // android workaround
#include "drishti/core/Semaphore.h"
#include "drishti/core/Logger.h"
#include "drishti/hci/FaceFinderPainter.h"
#include "drishti/hci/FaceMonitor.h"
#include "drishti/testlib/drishti_cli.h"
#include "drishti/face/FaceDetectorFactoryJson.h"
#include "drishti/core/drishti_string_hash.h"

#define USE_EOS 0

#if USE_EOS
#  include "drishti/face/FaceMeshMapperFactory.h"
#endif

#include "drishti/geometry/motion.h"

#include "ogles_gpgpu/common/proc/swizzle.h"

#include "videoio/VideoSourceCV.h"
#include "videoio/VideoSinkCV.h"

#include "aglet/GLContext.h"

// Package includes:
#include "cxxopts.hpp"

#include "ogles_gpgpu/common/proc/disp.h"
#include "ogles_gpgpu/common/proc/swizzle.h"

#include <spdlog/fmt/ostr.h>

#define DRISHTI_BUILD_OPENCV_CONTRIB 1

#if DRISHTI_BUILD_OPENCV_CONTRIB
#  include <opencv2/tracking.hpp>
#endif
#include <opencv2/imgproc.hpp>

#include <memory>

using string_hash::operator"" _hash;

// clang-format off
#ifdef ANDROID
#  define TEXTURE_FORMAT GL_RGBA
#else
#  define TEXTURE_FORMAT GL_BGRA
#endif
// clang-format on

#define DRISHTI_HCI_USE_CACHE 0 // simulate speed of real video

#include <opencv2/highgui.hpp>

using LoggerPtr = std::shared_ptr<spdlog::logger>;

static void* void_ptr(const cv::Mat& image)
{
    return const_cast<void*>(image.ptr<void>());
}

static bool checkModel(LoggerPtr& logger, const std::string& sModel, const std::string& description);
static ogles_gpgpu::SwizzleProc::SwizzleKind getSwizzleKind(const std::string &sSwizzle);

// Simple FaceMonitor class to report face detection results over time.
struct FaceMonitorLogger : public drishti::hci::FaceMonitor
{
    FaceMonitorLogger(std::shared_ptr<spdlog::logger> &logger) : m_logger(logger)
    {

    }
    
    /**
     * A user defined virtual method callback that should report the number
     * of frames that should be captured from teh FIFO buffer based on the
     * reported face location.
     * @param faces a vector of faces for the current frame
     * @param timestmap the acquisition timestamp for the frame
     * @return a frame request for the last n frames with requested image formats
     */
    virtual Request request(const Faces& faces, const TimePoint& timeStamp)
    {
        cv::Point3f xyz = faces.size() ? (*faces.front().eyesCenter) : cv::Point3f();
        m_logger->info("SimpleFaceMonitor: Found {} faces {}", faces.size(), xyz);
        return {};
    }
    
    /**
     * A user defined virtual method callback that will be called with a
     * a populated vector of FaceImage objects for the last N frames, where
     * N is the number of frames requested in the preceding request callback.
     * @param frames A vector containing the last N consecutive FaceImage objects
     * @param isInitialized Return true if the FIFO buffer is fully initialized.
     */
    virtual void grab(const std::vector<FaceImage>& frames, bool isInitialized)
    {
        m_logger->info("SimpleFaceMonitor: Received {} frames", frames.size());
    }
    
    std::shared_ptr<spdlog::logger> m_logger;
};

//////


#if DRISHTI_BUILD_OPENCV_CONTRIB

// Simple FaceMonitor class to report face detection results over time.
struct FaceMonitorTracker: public drishti::hci::FaceMonitor
{
    typedef cv::TrackerMedianFlow PatchTracker;
    typedef cv::TrackerMedianFlow::Params PatchParams;
    
#if USE_EOS
    using FaceMeshMapperFactory = drishti::face::FaceMeshMapperFactory;
    using FaceMeshMapper = drishti::face::FaceMeshMapper;
    using FaceMeshContainer = drishti::face::FaceMeshContainer;
#endif
    
    struct Tracker
    {
        std::string name;
        cv::Rect2d roi;
        cv::Ptr<PatchTracker> tracker;
    };
    
    FaceMonitorTracker(std::shared_ptr<spdlog::logger> &logger, const std::string &sMapper={})
        : m_logger(logger)
    {
#if USE_EOS
        if(!sMapper.empty())
        {
            m_mapper = FaceMeshMapperFactory(sMapper).create(FaceMeshMapperFactory::kLandmarksContours);
        }
#endif
        std::cout << "sqrt # kepoints" << m_params.pointsInGrid << std::endl;
    }
    
    ~FaceMonitorTracker()
    {
        // Serialize the gaze measurements:
        std::vector< std::vector< std::pair<std::string, float
    }
    
    /**
     * A user defined virtual method callback that should report the number
     * of frames that should be captured from teh FIFO buffer based on the
     * reported face location.
     * @param faces a vector of faces for the current frame
     * @param timestmap the acquisition timestamp for the frame
     * @return a frame request for the last n frames with requested image formats
     */
    virtual Request request(const Faces& faces, const TimePoint& timeStamp)
    {
        cv::Point3f xyz = faces.size() ? (*faces.front().eyesCenter) : cv::Point3f();
        m_logger->info("SimpleFaceMonitor: Found {} faces {}", faces.size(), xyz);
        return { 3, true, true }; // n, getImage, getTexture
    }
    
    static cv::Rect makeRect(const cv::Point &center, const cv::Size &size)
    {
        return { center - cv::Point(size.width/2, size.height/2), size };
    }

    /**
     * A user defined virtual method callback that will be called with a
     * a populated vector of FaceImage objects for the last N frames, where
     * N is the number of frames requested in the preceding request callback.
     * @param frames A vector containing the last N consecutive FaceImage objects
     * @param isInitialized Return true if the FIFO buffer is fully initialized.
     */
    virtual void grab(const std::vector<FaceImage>& frames, bool isInitialized)
    {
        m_logger->info("SimpleFaceMonitor: Received {} frames", frames.size());
        
#if USE_EOS
        std::shared_ptr<drishti::face::FaceMeshContainer> mesh;
#endif
        
        if(frames.size() >= 3) // account for latency
        {
            const auto &image = frames.back().image.image;
            const auto &face = frames.back().faceModels.front();
            
            if(!(frames[0].eyes.image.empty()) && !(frames[2].eyes.image.empty()))
            {
                //estimateGaze( {{frames[0].eyes.image, frames[1].eyes.image }} );
            }
            
#if USE_EOS
            if(m_mapper)
            {
                cv::Mat gray;
                cv::extractChannel(image, gray, 1);
                mesh = (*m_mapper)(*face.points, gray);
            }
#endif
            
            if(m_trackers.empty())
            {
                std::array<cv::Rect2f, 2> eyes, eyesUpper, eyesLower;
                face.getEyeRegions(eyes[0], eyes[1], 0.33);

                cv::Point2f nose = (*face.noseTip);
                std::array<std::array<cv::Point2f, 2>, 2> corners
                {{
                    {{ face.eyeFullR->getOuterCorner(), face.eyeFullR->getInnerCorner() }},
                    {{ face.eyeFullL->getInnerCorner(), face.eyeFullL->getOuterCorner() }}
                }};

                // Create new line indexed templates (somewhat pose invariant):
                const cv::Point2f vro = corners[0][0] - nose;
                const cv::Point2f vri = corners[0][1] - nose;
                const cv::Point2f vli = corners[1][0] - nose;
                const cv::Point2f vlo = corners[1][1] - nose;
               
                m_trackers =
                {
                    { "right-upper", makeRect(nose + vri * 1.666f, eyes[0].size()), PatchTracker::create() },
                    { "right-lower", makeRect(nose + vro * 0.666f, eyes[0].size()), PatchTracker::create() },
                    { "left-lower", makeRect(nose + vlo * 0.666f, eyes[1].size()), PatchTracker::create() },
                    { "left-upper", makeRect(nose + vli * 1.666f, eyes[1].size()), PatchTracker::create() },
                    { "dorsal-bridge", getDorsalBridgeRoi(face, 0.33f), PatchTracker::create() }
                };

                for(auto &t : m_trackers)
                {
                    t.roi &= cv::Rect2d({0.0,0.0}, image.size());
                }
                
                // Skip if any patch isn't visible:
                cv::parallel_for_({0, static_cast<int>(m_trackers.size())}, [&](const cv::Range &r) {
                    for(int i = r.start; i < r.end; i++) {
                        m_trackers[i].tracker->init(image, m_trackers[i].roi);
                    }
                });
            }
            else
            {
                // Update the trackers:
                cv::parallel_for_({0, static_cast<int>(m_trackers.size())}, [&](const cv::Range &r) {
                    for(int i = r.start; i < r.end; i++) {
                        m_trackers[i].tracker->update(image, m_trackers[i].roi);
                    }
                });
            }
            
            addFaceToLog(face, 0) ;
            
            draw(face, image);
        }
    }
    
    void addFaceToLog(const drishti::face::FaceModel &faceIn, std::size_t index)
    {
        auto face = faceIn;
        for(const auto &t : m_trackers)
        {
            face.userFeatures.emplace_back(t.name, (t.roi.tl() + t.roi.br()) * 0.5);
        }
        
        faceHistory.emplace_back(index, face);
        if(faceHistory.size() > 4000)
        {
            faceHistory.pop_front();
        }
    }
    
    void draw(const drishti::face::FaceModel &face, const cv::Mat4b &image)
    {
        cv::Mat canvas = image.clone();
        face.draw(canvas, 4, true, false);
        
#if USE_EOS
        if(mesh)
        {
            mesh->drawWireFrame(canvas);
        }
#endif
        
        //            for(const auto &r : face.rois)
        //            {
        //                cv::rectangle(canvas, r, {255,255,0}, 2, 8);
        //            }
        
        for(const auto &t : m_trackers)
        {
            cv::Point2f tl(t.roi.tl()), br(t.roi.br()), tr(br.x, tl.y), bl(tl.x, br.y);
            cv::rectangle(canvas, t.roi, {0,255,0}, 2, 8);
            cv::line(canvas, tl, br, {0,255,0}, 2, 8);
            cv::line(canvas, tr, bl, {0,255,0}, 2, 8);
            
            cv::line(canvas, (tl+br)*0.5, face.eyeFullL->irisEllipse.center, {0,255,0}, 2, 8);
            cv::line(canvas, (tl+br)*0.5, face.eyeFullR->irisEllipse.center, {0,255,0}, 2, 8);
            
            cv::circle(canvas, (tl + br) * 0.5f, std::max(6, static_cast<int>(cv::norm(tl-br)*0.025f)), {255,0,255}, -1, 8);
        }
        cv::imshow("face", canvas); cv::waitKey(0);
        
        if(!m_writer.isOpened())
        {
            m_writer.open("/tmp/drishti_gaze_poc.mov", CV_FOURCC('m','p','4','v'), 24, canvas.size(), true);
        }
        m_writer << canvas;
    }

    
    cv::Rect2d getDorsalBridgeRoi(const drishti::face::FaceModel &face, float scale) const
    {
        const cv::Point2f l = face.eyeFullL->irisEllipse.center;
        const cv::Point2f r = face.eyeFullR->irisEllipse.center;
        const float iod = cv::norm(l - r);
        const cv::Point2f center = (l + r) * 0.5f, diag(iod * scale * 0.5, iod * scale * 0.5f);
        return { center - diag, center + diag };
    }

    /*
     * Calculates gradient and difference between images
     * \param[in] img1 Image one
     * \param[in] img2 Image two
     * \param[out] Ix Gradient x-coordinate
     * \param[out] Iy Gradient y-coordinate
     * \param[out] It Difference of images
     */
    
    void gradient(const cv::Mat& img1, const cv::Mat& img2, cv::Mat& Ix, cv::Mat& Iy, cv::Mat& It) const
    {
        cv::Size sz1 = img2.size();
        
        cv::Mat xkern = (cv::Mat_<double>(1, 3) << -1., 0., 1.)/2.;
        cv::filter2D(img2, Ix, -1, xkern, {-1,-1}, 0., cv::BORDER_REPLICATE);
        
        cv::Mat ykern = (cv::Mat_<double>(3, 1) << -1., 0., 1.)/2.;
        cv::filter2D(img2, Iy, -1, ykern, {-1,-1}, 0., cv::BORDER_REPLICATE);
        
        It = cv::Mat::zeros(sz1, img1.type());
        It = img2 - img1;
    }
    
    void estimateGaze(const std::array<cv::Mat4b, 2> &eyes)
    {
        cv::Mat gradx, grady, imgDiff;
        gradient(eyes[0], eyes[1], gradx, grady, imgDiff);
        
        // Calculate parameters using least squares
        cv::Matx<double, 2, 2> A;
        cv::Vec<double, 2> b;
        // For each value in A, all the matrix elements are added and then the channels are also added,
        // so we have two calls to "sum". The result can be found in the first element of the final
        // Scalar object.
        
        A(0, 0) = cv::sum(cv::sum(gradx.mul(gradx)))[0];
        A(0, 1) = cv::sum(cv::sum(gradx.mul(grady)))[0];
        A(1, 1) = cv::sum(cv::sum(grady.mul(grady)))[0];
        A(1, 0) = A(0, 1);
        
        b(0) = -cv::sum(cv::sum(imgDiff.mul(gradx)))[0];
        b(1) = -cv::sum(cv::sum(imgDiff.mul(grady)))[0];
        
        // Calculate shift. We use Cholesky decomposition, as A is symmetric.
        cv::Vec<double, 2> shift = A.inv(cv::DECOMP_CHOLESKY)*b;
        
        const float scale = 40.0;
        cv::Point center(eyes[0].cols/2, eyes[0].rows/2);
        cv::line(eyes[0], center, center - cv::Point(shift[0]*scale, shift[1]*scale), {0,255,255}, 2, 8);
        cv::imshow("e0", eyes[0]);
        cv::imshow("e1", eyes[1]);
        cv::waitKey(0);
    }
   
    std::deque< std::pair<int, drishti::face::FaceModel> > faceHistory;
    
    cv::VideoWriter m_writer;

#if USE_EOS
    std::shared_ptr<drishti::face::FaceMeshMapper> m_mapper;
#endif
    
    PatchParams m_params;
    std::vector< Tracker > m_trackers;
    std::shared_ptr<spdlog::logger> m_logger;
};
#endif

//////

// Add a simple timer to report frame rate.  For any offline VideoSource type
// the processing will almost certainly be IO limited.  As a simple workaround
// you can define
//
// #define DRISHTI_HCI_USE_CACHE 1
//
// For videos that will fit in memory and it will cache the frames in memory.
// The reported times will be closer to what one would see with a live video
// capture.
struct SimpleTimer
{
    int frames = 0;
    std::chrono::high_resolution_clock::time_point tic;
    void reset()
    {
        frames = 0;
        tic = std::chrono::high_resolution_clock::now();
    }
    SimpleTimer& operator++()
    {
        frames++;
        return (*this);
    }
    float fps() const
    {
        const auto toc = std::chrono::high_resolution_clock::now();
        const double elapsed = std::chrono::duration<double>(toc - tic).count();
        return static_cast<float>(frames / elapsed);
    }
};

int gauze_main(int argc, char** argv)
{
    const auto argumentCount = argc;

    // Instantiate line logger:
    auto logger = drishti::core::Logger::create("drishti-hci");

    // ############################
    // ### Command line parsing ###
    // ############################

    bool doWindow = false;
    bool doMovie = false;
    bool doDebug = false;
    int loops = 0;
    
    std::string sInput, sOutput, sSwizzle = "rgba", sEOS;

    float resolution = 1.f;
    float cascCal = 0.f;
    float scale = 1.f;
    float fx = 0.f;
    
    bool doInner = false; // inner face processing
    
    // Create FaceDetectorFactory (default file based):
    std::string sFactory;
    auto factory = std::make_shared<drishti::face::FaceDetectorFactory>();

    cxxopts::Options options("drishti-hci", "Command line interface for video sequence FaceFinder processing.");

    float minZ = 0.1f, maxZ = 2.f;

    // clang-format off
    options.add_options()
        ("i,input", "Input file", cxxopts::value<std::string>(sInput))
        ("o,output", "Output directory", cxxopts::value<std::string>(sOutput))

        ("swizzle", "Swizzle channel operation", cxxopts::value<std::string>(sSwizzle))
    
#if !defined(DRISHTI_IS_MOBILE)
        ("w,window", "Create a display window", cxxopts::value<bool>(doWindow))
        ("r,resolution", "Display resolution (scale factory)", cxxopts::value<float>(resolution))
        ("debug", "Provide debugging annotations", cxxopts::value<bool>(doDebug))
#endif
        ("l,loops", "Loop the input video", cxxopts::value<int>(loops))
    
        ("e,eos", "Eos assets file (factory)", cxxopts::value<std::string>(sEOS))
    
        // Generate a quicktime movie:
        ("m,movie", "Output quicktime movie", cxxopts::value<bool>(doMovie))
    
        // Detection and regression parameters:
        ("c,calibration", "Cascade calibration", cxxopts::value<float>(cascCal))
        ("s,scale", "Scale term for detection->regression mapping", cxxopts::value<float>(scale))
        ("f,focal-length", "Focal length in pixels",cxxopts::value<float>(fx))
        ("min", "Nearest distance in meters", cxxopts::value<float>(minZ))
        ("max", "Farthest distance in meters", cxxopts::value<float>(maxZ))
    
        // Clasifier and regressor models:
        ("D,detector", "Face detector model", cxxopts::value<std::string>(factory->sFaceDetector))
        ("M,mean", "Face detector mean", cxxopts::value<std::string>(factory->sFaceDetectorMean))
        ("R,regressor", "Face regressor", cxxopts::value<std::string>(factory->sFaceRegressor))
        ("E,eye", "Eye model", cxxopts::value<std::string>(factory->sEyeRegressor))

        // ... factory can be used instead of D,M,R,E
        ("F,factory", "Factory (json model zoo)", cxxopts::value<std::string>(sFactory))
        ("inner", "Inner face landmakrs", cxxopts::value<bool>(doInner))
    
        ("h,help", "Print help message");
    // clang-format on

    options.parse(argc, argv);

    if ((argumentCount <= 1) || options.count("help"))
    {
        std::cout << options.help({ "" }) << std::endl;
        return 0;
    }

    // ############################################
    // ### Command line argument error checking ###
    // ############################################

    // ### Directory
    if (sOutput.empty())
    {
        logger->error("Must specify output directory");
        return 1;
    }

    if (drishti::cli::directory::exists(sOutput, ".drishti-hci"))
    {
        std::string filename = sOutput + "/.drishti-hci";
        remove(filename.c_str());
    }
    else
    {
        logger->error("Specified directory {} does not exist or is not writeable", sOutput);
        return 1;
    }

    // ### Input
    if (sInput.empty())
    {
        logger->error("Must specify input image or list of images");
        return 1;
    }
    if (!sInput.find(".test") && !drishti::cli::file::exists(sInput))
    {
        logger->error("Specified input file does not exist or is not readable");
        return 1;
    }

    if (!sFactory.empty())
    {
        factory = std::make_shared<drishti::face::FaceDetectorFactoryJson>(sFactory);
    }
    factory->inner = doInner;

    // Check for valid models
    std::vector<std::pair<std::string, std::string>> config{
        { factory->sFaceDetector, "face-detector" },
        { factory->sFaceDetectorMean, "face-detector-mean" },
        { factory->sFaceRegressor, "face-regressor" },
        { factory->sEyeRegressor, "eye-regressor" }
    };

    for (const auto& c : config)
    {
        if (checkModel(logger, c.first, c.second))
        {
            return 1;
        }
    }

    // In some glfw + avfoundation + os x combinations we can see the following system
    // error.  This may be behind us now!
    // ~~~~~
    // !!! BUG: The current event queue and the main event queue are not the same.
    // Events will not be handled correctly. This is probably because _TSGetMainThread
    // was called for the first time off the main thread.

    // NOTE: We can create the OpenGL context prior to AVFoundation use as a workaround
    auto opengl = aglet::GLContext::create(aglet::GLContext::kAuto, doWindow ? "hci" : "", 640, 480);
#if defined(_WIN32) || defined(_WIN64)
    CV_Assert(!glewInit());
#endif

    auto video = drishti::videoio::VideoSourceCV::create(sInput);
    video->setOutputFormat(drishti::videoio::VideoSourceCV::ARGB); // be explicit, fail on error

    // Retrieve first frame to configure sensor parameters:
    std::size_t counter = 0;
    auto frame = (*video)(counter);
    const cv::Size frameSize = frame.image.size();
    
    cv::Matx33f S = transformation::scale(1.f, 0.9f, cv::Point2f(frameSize.width/2, frameSize.height/2));
  
    if (frame.image.empty())
    {
        logger->info("No frames available in video");
        return -1;
    }

    if (maxZ < minZ)
    {
        logger->error("max distance must be > min distance");
        return -1;
    }

    cv::Size windowSize = cv::Size2f(frameSize) * resolution;
    opengl->resize(windowSize.width, windowSize.height);

    // Create configuration:
    drishti::hci::FaceFinder::Settings settings;
    settings.logger = drishti::core::Logger::create("test-drishti-hci");
    settings.outputOrientation = 0;
    settings.frameDelay = 2;
    settings.doLandmarks = true;
    settings.doFlow = false;
    settings.doBlobs = false;
    settings.threads = std::make_shared<tp::ThreadPool<>>();
    settings.outputOrientation = 0;
    settings.faceFinderInterval = 0.f;
    settings.regressorCropScale = scale;
    settings.acfCalibration = cascCal;
    settings.doOptimizedPipeline = true; // set to false for latency=0 else latency=2
        
    // ||||||||||||||||||||||||||||||||||||||||||||||||||||||||
    settings.renderFaces = true;          // *** rendering ***
    settings.renderPupils = true;         // *** rendering ***
    settings.renderCorners = false;       // *** rendering ***
    settings.renderEyesWidthRatio = 0.25f * opengl->getGeometry().sx; // *** rendering ***
    // |||||||||||||||||||||||||||||||||||||||||||||||||||||||
    
    settings.minDetectionDistance = minZ;
    settings.maxDetectionDistance = maxZ;
    settings.doSingleFace = true;
    { // Create a sensor specification
        if (fx == 0.f)
        {
            fx = frame.image.cols; // use a sensible default guess
        }
        const cv::Point2f p(frame.image.cols / 2, frame.image.rows / 2);
        drishti::sensor::SensorModel::Intrinsic params(p, fx, frame.image.size());
        settings.sensor = std::make_shared<drishti::sensor::SensorModel>(params);
    }

    (*opengl)(); // activate context

    // Allocate the detector and configure the display properties
    auto detector = drishti::hci::FaceFinderPainter::create(factory, settings, nullptr);
    detector->setLetterboxHeight(1.0);         // *** rendering ***
    detector->setShowMotionAxes(doDebug);      // *** rendering ***
    detector->setShowDetectionScales(doDebug); // *** rendering ***
    
#if DRISHTI_BUILD_OPENCV_CONTRIB
    FaceMonitorTracker faceTracker(logger, sEOS);
    detector->registerFaceMonitorCallback(&faceTracker);
#else
    // Instantiate and register a samle FaceMonitor class to log tracking results
    // over time.
    FaceMonitorLogger faceLogger(logger);
    detector->registerFaceMonitorCallback(&faceLogger);
#endif
    
    // Allocate an input video source for feeding texture or image buffers
    // into the gpgpu pipeline.
    ogles_gpgpu::VideoSource source;
    ogles_gpgpu::SwizzleProc swizzle(getSwizzleKind(sSwizzle));
    source.set(&swizzle);

    // Provide a default quicktime movie name for logging on Apple platforms.
    std::string filename = sOutput + "/movie.mov";
    if (drishti::cli::file::exists(filename))
    {
        remove(filename.c_str());
    }

    std::shared_ptr<drishti::videoio::VideoSinkCV> sink;
    if (doMovie)
    {
        sink = drishti::videoio::VideoSinkCV::create(filename, ".mov");
        if (sink)
        {
            sink->setProperties({ frame.cols(), frame.rows() });
            sink->begin();
        }
    }

    // Instantiate an ogles_gpgpu display class that will draw to the
    // default texture (0) which will be managed by aglet (typically glfw)
    std::shared_ptr<ogles_gpgpu::Disp> display;
    if (doWindow && opengl->hasDisplay())
    {
        display = std::make_shared<ogles_gpgpu::Disp>();
        display->init(frame.image.cols, frame.image.rows, TEXTURE_FORMAT);
        display->setOutputRenderOrientation(ogles_gpgpu::RenderOrientationFlipped);
    }

    SimpleTimer timer;
    
#if DRISHTI_HCI_USE_CACHE
    std::map<int, drishti::videoio::VideoSourceCV::Frame> cache;
#endif
    
    int loopCount = 0;
    std::function<bool(void)> render = [&]()
    {
#if DRISHTI_HCI_USE_CACHE
        if(cache.find(counter) != cache.end())
        {
            frame = cache[counter];
        }
        else
#endif
        {
            frame = (*video)(counter);
            
            //cv::warpAffine(frame.image, frame.image, S.get_minor<2,3>(0,0), frame.image.size());

#if DRISHTI_HCI_USE_CACHE
            cache[counter] = frame; // cache it
#endif            
            timer.reset(); // reset timer
        }
        
        logger->info("fps: {}", (++timer).fps());
        
        counter++;
        if (frame.image.empty())
        {
            logger->info("Frame {} is empty, skipping ...", counter);
            
            if(loopCount < loops)
            {
                counter = 0;
                loopCount++;
                return true;
            }
            
            return false;
        }
        
        if (frame.image.size() != frameSize)
        {
            logger->info("Frame size has changed, skipping ...", frameSize);
            return false;
        }
        
        if (frame.image.channels() == 3)
        {
            cv::cvtColor(frame.image, frame.image, cv::COLOR_BGR2BGRA);
        }

        std::stringstream ss;
        ss << "N=" << counter;
        std::cout << "TAG " << ss.str() << std::endl;
        cv::putText(frame.image,  ss.str(), {64, frame.image.rows-64}, CV_FONT_HERSHEY_SIMPLEX, 4.0, {255,255,255,255}, 8);

        
        
        CV_Assert(frame.image.channels() == 4);

        logger->info("{}", cv::mean(frame.image));

        // Perform texture swizzling:
        source({ { frame.cols(), frame.rows() }, void_ptr(frame.image), true, 0, TEXTURE_FORMAT });
        auto texture0 = swizzle.getOutputTexId();
        auto texture1 = (*detector)({ { frame.cols(), frame.rows() }, nullptr, false, texture0, TEXTURE_FORMAT });

        // Convert to texture as one of GL_BGRA or GL_RGBA
        if (display)
        {
            auto& geometry = opengl->getGeometry();
            display->setOffset(geometry.tx, geometry.ty);
            display->setDisplayResolution(geometry.sx * resolution, geometry.sy * resolution);
            display->useTexture(texture1);
            display->render(0);
        }

        if (sink && sink->good())
        {
            // clang-format off
            drishti::hci::FaceFinderPainter::FrameDelegate delegate = [&](const cv::Mat& image)
            {
                (*sink)(image);
            };
            // clang-format on
            detector->getOutputPixels(delegate);
        }

        return true;
    };

    (*opengl)(render);

    { // dump metadata
        for (const auto &f : )
        {
            
        }
    }
    
    if (sink)
    {
        drishti::core::Semaphore s(0);
        sink->end([&] { s.signal(); });
        s.wait();
    }
    return 0;
}

// utility:

static bool checkModel(LoggerPtr& logger, const std::string& sModel, const std::string& description)
{
    if (sModel.empty())
    {
        logger->error("Must specify valid model {}", sModel);
        return 1;
    }
    if (!drishti::cli::file::exists(sModel))
    {
        logger->error("Specified file {} does not exist or is not readable", sModel);
        return 1;
    }
    return 0;
}

static ogles_gpgpu::SwizzleProc::SwizzleKind getSwizzleKind(const std::string &sSwizzle)
{
    switch (string_hash::hash(sSwizzle))
    {
        case "rgba"_hash: return ogles_gpgpu::SwizzleProc::kSwizzleRGBA; break;
        case "bgra"_hash: return ogles_gpgpu::SwizzleProc::kSwizzleBGRA; break;
        case "argb"_hash: return ogles_gpgpu::SwizzleProc::kSwizzleARGB; break;
        case "abgr"_hash: return ogles_gpgpu::SwizzleProc::kSwizzleABGR; break;
        case "grab"_hash: return ogles_gpgpu::SwizzleProc::kSwizzleGRAB; break;
        default: throw std::runtime_error("Unsupported type specified in" + sSwizzle);
    }
}
