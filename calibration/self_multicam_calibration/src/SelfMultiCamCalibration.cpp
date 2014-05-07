#include "self_multicam_calibration/SelfMultiCamCalibration.h"

#include <boost/filesystem.hpp>
#include <boost/unordered_set.hpp>

#include "cauldron/EigenQuaternionParameterization.h"
#include "cauldron/EigenUtils.h"
#include "camera_calibration/StereoCameraCalibration.h"
#include "camera_models/CostFunctionFactory.h"
#include "ceres/ceres.h"
#include "hand_eye_calibration/HandEyeCalibration.h"
#include "location_recognition/OrbLocationRecognition.h"
#include "pose_imu_calibration/PoseIMUCalibration.h"
#include "pose_graph/PoseGraph.h"
#include "pose_graph/PoseGraphViz.h"

namespace px
{

class GraphVizCallback: public ceres::IterationCallback
{
public:
    GraphVizCallback(SparseGraphViz& sgv)
     : m_sgv(sgv) {}

    ceres::CallbackReturnType operator()(const ceres::IterationSummary& summary)
    {
        m_sgv.visualize();

        return ceres::SOLVER_CONTINUE;
    }

private:
    SparseGraphViz& m_sgv;
};

SelfMultiCamCalibration::SelfMultiCamCalibration(ros::NodeHandle& nh,
                                                 const CameraSystemPtr& cameraSystem,
                                                 const SparseGraphPtr& sparseGraph)
 : m_nh(nh)
 , m_cameraSystem(cameraSystem)
 , m_sparseGraph(sparseGraph)
 , m_sgv(nh, sparseGraph)
{
    int nStereoCams = cameraSystem->cameraCount() / 2;

    for (int i = 0; i < nStereoCams; ++i)
    {
        m_svo.push_back(boost::make_shared<StereoVO>(cameraSystem, i * 2, i * 2 + 1, false));
    }

    for (int i = 0; i < nStereoCams; ++i)
    {
        m_subSparseGraphs.push_back(boost::make_shared<SparseGraph>());

        std::ostringstream oss;
        oss << "sub";
        oss << i;
        m_subsgv.push_back(boost::make_shared<SparseGraphViz>(boost::ref(nh),
                                                              m_subSparseGraphs.at(i),
                                                              oss.str()));
    }
}

bool
SelfMultiCamCalibration::init(const std::string& detectorType,
                              const std::string& descriptorExtractorType,
                              const std::string& descriptorMatcherType)
{
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        if (!m_svo.at(i)->init(detectorType, descriptorExtractorType, descriptorMatcherType))
        {
            return false;
        }
    }

    return true;
}

bool
SelfMultiCamCalibration::processFrames(const ros::Time& stamp,
                                       const std::vector<cv::Mat>& images,
                                       const sensor_msgs::ImuConstPtr& imuMsg)
{
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        if (!m_svo.at(i)->readFrames(stamp, images.at(i * 2),
                                     images.at(i * 2 + 1)))
        {
            return false;
        }
    }

    std::vector<FrameSetPtr> frameSets(m_svo.size());
    std::vector<boost::shared_ptr<boost::thread> > threads(m_svo.size());
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        threads.at(i) = boost::make_shared<boost::thread>(boost::bind(&StereoVO::processFrames,
                                                                      m_svo.at(i),
                                                                      boost::ref(frameSets.at(i))));
    }

    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        threads.at(i)->join();

        if (!frameSets.at(i))
        {
            return false;
        }

        frameSets.at(i)->imuMeasurement() = imuMsg;
    }

    bool init = true;
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        if (m_subSparseGraphs.at(i)->frameSetSegment(0).empty())
        {
            m_subSparseGraphs.at(i)->frameSetSegment(0).push_back(frameSets.at(i));

            m_subsgv.at(i)->visualize(10);
        }
        else
        {
            init = false;
        }
    }

    if (init)
    {
        return true;
    }

    bool keyFrames = false;
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        if (m_svo.at(i)->getCurrent2D3DCorrespondenceCount() < 40)
        {
            keyFrames = true;
            break;
        }
    }

    if (keyFrames)
    {
        for (size_t i = 0; i < m_svo.size(); ++i)
        {
            m_svo.at(i)->keyCurrentFrameSet();

            m_subSparseGraphs.at(i)->frameSetSegment(0).push_back(frameSets.at(i));

            m_subsgv.at(i)->visualize(10);
        }
    }

    return true;
}

bool
SelfMultiCamCalibration::run(const std::string& vocFilename,
                             const std::string& chessboardDataDir,
                             bool readIntermediateData)
{
    cv::Mat matchingMask = cv::Mat::zeros(m_cameraSystem->cameraCount(), m_cameraSystem->cameraCount(), CV_8U);
    for (int i = 0; i < m_cameraSystem->cameraCount(); i += 2)
    {
        matchingMask.at<unsigned char>(i,i) = 1;
    }

    if (!readIntermediateData)
    {
        std::vector<boost::shared_ptr<boost::thread> > threads(m_svo.size());
        for (size_t i = 0; i < m_svo.size(); ++i)
        {
            ROS_INFO("Processing subgraph for stereo camera %lu...", i);

            threads.at(i) = boost::make_shared<boost::thread>(boost::bind(&SelfMultiCamCalibration::processSubGraph,
                                                                          this,
                                                                          boost::ref(m_subSparseGraphs.at(i)),
                                                                          boost::ref(m_subsgv.at(i)),
                                                                          boost::cref(vocFilename),
                                                                          boost::cref(matchingMask)));
        }

        for (size_t i = 0; i < m_svo.size(); ++i)
        {
            threads.at(i)->join();
        }

        ROS_INFO("Running hand-eye calibration...");
        if (!runHandEyeCalibration())
        {
            return false;
        }

        ROS_INFO("Merging maps...");
        mergeMaps();

        ROS_INFO("Writing intermediate data...");
        m_sparseGraph->writeToBinaryFile("int_map.sg");
        m_cameraSystem->writePosesToTextFile("int_camera_system_extrinsics.txt");
        ROS_INFO("Done!");
    }
    else
    {
        ROS_INFO("Reading intermediate data...");
        if (!m_sparseGraph->readFromBinaryFile("int_map.sg"))
        {
            ROS_ERROR("Failed!");
            return false;
        }
        else
        {
            ROS_INFO("Done!");
        }
    }

    ROS_INFO("Running pose graph optimization for all cameras...");
    matchingMask = cv::Mat::zeros(m_cameraSystem->cameraCount(), m_cameraSystem->cameraCount(), CV_8U);
    for (int i = 0; i < m_cameraSystem->cameraCount(); i += 2)
    {
        for (int j = 0; j < m_cameraSystem->cameraCount(); j += 2)
        {
            if (i == j)
            {
                continue;
            }
            matchingMask.at<unsigned char>(i,j) = 1;
        }
    }

    runPG(m_sparseGraph, vocFilename, 15, 30, matchingMask,
          Point3DFeature::OBSERVED_BY_MULTIPLE_STEREO_RIGS);

    m_sgv.visualize();

    ROS_INFO("Running full bundle adjustment for all cameras...");
    std::vector<std::string> chessboardDataFilenames;
    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        std::string filename = chessboardDataDir + "/" +
                               m_cameraSystem->getCamera(i * 2)->cameraName() + "_" +
                               m_cameraSystem->getCamera(i * 2 + 1)->cameraName() +
                               "_chessboard_data.dat";

        chessboardDataFilenames.push_back(filename);
    }

    runFullBA(chessboardDataFilenames);

    m_sgv.visualize();

    ROS_INFO("Running pose-IMU calibration...");
    if (!runPoseIMUCalibration())
    {
        return false;
    }

    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        origin += m_cameraSystem->getGlobalCameraPose(i).block<3,1>(0,3);
    }
    origin /= static_cast<double>(m_cameraSystem->cameraCount());

    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        Eigen::Matrix4d H_cam = m_cameraSystem->getGlobalCameraPose(i);

        H_cam.block<3,1>(0,3) -= origin;

        m_cameraSystem->setGlobalCameraPose(i, H_cam);
    }

    return true;
}

bool
SelfMultiCamCalibration::writePosesToTextFile(const std::string& filename) const
{
    std::ofstream ofs(filename.c_str());
    if (!ofs.is_open())
    {
        return false;
    }

    ofs << std::fixed << std::setprecision(20);

    const std::vector<FrameSetPtr>& frameSets = m_sparseGraph->frameSetSegment(0);
    for (size_t i = 0; i < frameSets.size(); ++i)
    {
        const PosePtr& pose = frameSets.at(i)->systemPose();

        ofs << pose->timeStamp().toSec() << " "
            << pose->rotation().x() << " "
            << pose->rotation().y() << " "
            << pose->rotation().z() << " "
            << pose->rotation().w() << " "
            << pose->translation()(0) << " "
            << pose->translation()(1) << " "
            << pose->translation()(2) << std::endl;
    }

    ofs.close();

    return true;
}

bool
SelfMultiCamCalibration::writeMapToVRMLFile(const std::string& filename) const
{
    std::ofstream ofs(filename.c_str());
    if (!ofs.is_open())
    {
        return false;
    }

    boost::unordered_set<Point3DFeature*> scenePoints;

    for (size_t i = 0; i < m_sparseGraph->frameSetSegments().size(); ++i)
    {
        const FrameSetSegment& segment = m_sparseGraph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            const FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                const FramePtr& frame = frameSet->frames().at(k);

                const std::vector<Point2DFeaturePtr>& features = frame->features2D();

                for (size_t l = 0; l < features.size(); ++l)
                {
                    const Point3DFeaturePtr& scenePoint = features.at(l)->feature3D();

                    scenePoints.insert(scenePoint.get());
                }
            }

            // create VRML file that encodes geometry representing camera poses
            std::ostringstream oss;
            oss << frameSet->systemPose()->timeStamp().toNSec() << "_pose.wrl";

            std::ofstream poseOfs(oss.str().c_str());

            poseOfs << "#VRML V2.0 utf8" << std::endl;
            poseOfs << "Shape {" << std::endl;

            Eigen::Matrix4d H_sys = invertHomogeneousTransform(frameSet->systemPose()->toMatrix());

            std::vector<std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > > frustumVec;
            for (int k = 0; k < m_cameraSystem->cameraCount(); ++k)
            {
                Eigen::Matrix4d H_cam = H_sys * m_cameraSystem->getGlobalCameraPose(k);

                double xBound = 0.06;
                double yBound = 0.06;
                double zFar = 0.08;

                std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > frustum;
                frustum.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
                frustum.push_back(Eigen::Vector3d(-xBound, -yBound, zFar));
                frustum.push_back(Eigen::Vector3d(xBound, -yBound, zFar));
                frustum.push_back(Eigen::Vector3d(xBound, yBound, zFar));
                frustum.push_back(Eigen::Vector3d(-xBound, yBound, zFar));

                for (size_t l = 0; l < frustum.size(); ++l)
                {
                    frustum.at(l) = transformPoint(H_cam, frustum.at(l));
                }

                frustumVec.push_back(frustum);
            }

            poseOfs << "     appearance Appearance {" << std::endl;
            poseOfs << "         material Material {" << std::endl;
            poseOfs << "             diffuseColor    0 1 0" << std::endl;
            poseOfs << "         }" << std::endl;
            poseOfs << "     }" << std::endl;
            poseOfs << "     geometry IndexedLineSet {" << std::endl;
            poseOfs << "       coord Coordinate {" << std::endl;
            poseOfs << "           point [" << std::endl;

            for (int k = 0; k < frustumVec.size(); ++k)
            {
                for (size_t l = 0; l < frustumVec.at(k).size(); ++l)
                {
                    Eigen::Vector3d P = frustumVec.at(k).at(l);

                    poseOfs << "               " << P(0) << " " << P(1) << " " << P(2) << "," << std::endl;
                }
            }

            poseOfs << "           ]" << std::endl;
            poseOfs << "       }" << std::endl;
            poseOfs << "       coordIndex [" << std::endl;

            for (int k = 0; k < frustumVec.size(); ++k)
            {
                for (int l = 1; l < 5; ++l)
                {
                    poseOfs << "           "
                            << frustumVec.at(0).size() * k << ", "
                            << frustumVec.at(0).size() * k + l << ", -1," << std::endl;
                }
            }

            poseOfs << "       ]" << std::endl;
            poseOfs << "     }" << std::endl;

            poseOfs << "     geometry IndexedFaceSet {" << std::endl;
            poseOfs << "       coord Coordinate {" << std::endl;
            poseOfs << "           point [" << std::endl;

            for (int k = 0; k < frustumVec.size(); ++k)
            {
                for (size_t l = 0; l < frustumVec.at(k).size(); ++l)
                {
                    Eigen::Vector3d P = frustumVec.at(k).at(l);

                    poseOfs << "               " << P(0) << " " << P(1) << " " << P(2) << "," << std::endl;
                }
            }

            poseOfs << "           ]" << std::endl;
            poseOfs << "       }" << std::endl;
            poseOfs << "       coordIndex [" << std::endl;

            for (int k = 0; k < frustumVec.size(); ++k)
            {
                poseOfs << "           ";
                for (int l = 1; l < 5; ++l)
                {
                    poseOfs << frustumVec.at(0).size() * k + l << ", ";
                }
                poseOfs << "-1," << std::endl;
            }

            poseOfs << "       ]" << std::endl;

            poseOfs << "     }" << std::endl;
            poseOfs << "}" << std::endl;

            poseOfs.close();
        }
    }

    ofs << std::fixed << std::setprecision(5);

    ofs << "#VRML V2.0 utf8" << std::endl;
    ofs << "Shape {" << std::endl;
    ofs << "     geometry PointSet {" << std::endl;
    ofs << "       coord Coordinate {" << std::endl;
    ofs << "           point [" << std::endl;

    for (boost::unordered_set<Point3DFeature*>::iterator it = scenePoints.begin();
             it != scenePoints.end(); ++it)
    {
        Eigen::Vector3d P = (*it)->point();

        ofs << "               " << P(0) << " " << P(1) << " " << P(2) << "," << std::endl;
    }

    ofs << "           ]" << std::endl;
    ofs << "       }" << std::endl;
    ofs << "     }" << std::endl;
    ofs << "}" << std::endl;

    ofs.close();

    return true;
}

void
SelfMultiCamCalibration::processSubGraph(const SparseGraphPtr& graph,
                                         const boost::shared_ptr<SparseGraphViz>& graphViz,
                                         const std::string& vocFilename,
                                         const cv::Mat& matchingMask)
{
    graphViz->visualize();

    ROS_INFO("Running pose graph optimization...");
    runPG(graph, vocFilename, 50, 10, matchingMask,
          Point3DFeature::OBSERVED_BY_STEREO_RIG_MULTIPLE_TIMES);

    graphViz->visualize();

    ROS_INFO("Running bundle adjustment...");
    runBA(graph, graphViz);
}

bool
SelfMultiCamCalibration::runHandEyeCalibration(void)
{
    if (m_svo.size() < 2)
    {
        return false;
    }

    std::vector<std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > > H(m_svo.size());

    for (size_t i = 0; i < m_svo.size(); ++i)
    {
        H.at(i) = computeRelativeSystemPoses(m_subSparseGraphs.at(i)->frameSetSegment(0));
    }

    for (size_t i = 1; i < m_svo.size(); ++i)
    {
        HandEyeCalibration hec;

        Eigen::Matrix4d H_i_0;
        hec.solve(H.at(i), H.at(0), H_i_0);

        m_cameraSystem->setGlobalCameraPose(i * 2, H_i_0 * m_cameraSystem->getGlobalCameraPose(i * 2));
        m_cameraSystem->setGlobalCameraPose(i * 2 + 1, H_i_0 * m_cameraSystem->getGlobalCameraPose(i * 2 + 1));

        ROS_INFO("Initial transform between stereo cameras 0 and %lu:", i);
        ROS_INFO_STREAM(H_i_0);
    }

    return true;
}

void
SelfMultiCamCalibration::runPG(const SparseGraphPtr& graph,
                               const std::string& vocFilename,
                               int minLoopCorrespondences2D3D,
                               int nImageMatches,
                               const cv::Mat& matchingMask,
                               int scenePointFlag)
{
    PoseGraphPtr poseGraph = boost::make_shared<PoseGraph>(m_cameraSystem,
                                                           boost::ref(graph),
                                                           matchingMask,
                                                           minLoopCorrespondences2D3D,
                                                           nImageMatches);
    PoseGraphViz pgv(m_nh, poseGraph);

    poseGraph->setVerbose(true);

    poseGraph->buildEdges(vocFilename);

    pgv.visualize("pose_graph_before");

    double avgError, maxError, avgScenePointDepth;
    size_t featureCount;
    reprojErrorStats(graph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error before pose graph optimization: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);

    poseGraph->optimize(true);

    pgv.visualize("pose_graph_after");

    reprojErrorStats(graph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error after pose graph optimization: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);

    // merge pairs of duplicate scene points
    std::vector<std::pair<Point2DFeaturePtr, Point3DFeaturePtr> > corr2D3D = poseGraph->getCorrespondences2D3D();

    int nMergedScenePoints = 0;
    for (size_t i = 0; i < corr2D3D.size(); ++i)
    {
        Point3DFeaturePtr scenePoint1 = corr2D3D.at(i).first->feature3D();
        Point3DFeaturePtr scenePoint2 = corr2D3D.at(i).second;

        scenePoint1->attributes() |= scenePointFlag;

        if (scenePoint1 == scenePoint2)
        {
            continue;
        }

        bool merge = false;
        for (size_t j = 0; j < scenePoint2->features2D().size(); ++j)
        {
            Point2DFeature* feature2 = scenePoint2->features2D().at(j);

            bool found = false;
            for (size_t k = 0; k < scenePoint1->features2D().size(); ++k)
            {
                Point2DFeature* feature1 = scenePoint1->features2D().at(k);

                if (feature1 == feature2)
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                scenePoint1->features2D().push_back(feature2);
                merge = true;
            }
        }

        for (size_t j = 0; j < scenePoint1->features2D().size(); ++j)
        {
            Point2DFeature* feature = scenePoint1->features2D().at(j);
            feature->feature3D() = scenePoint1;
        }

        if (merge)
        {
            ++nMergedScenePoints;
        }
    }

    ROS_INFO("Merged %d pairs of duplicate scene points.", nMergedScenePoints);

    // reconstruct scene points
    boost::unordered_set<Point3DFeaturePtr> scenePoints;

    for (size_t i = 0; i < graph->frameSetSegments().size(); ++i)
    {
        FrameSetSegment& segment = graph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                FramePtr& frame = frameSet->frames().at(k);

                std::vector<Point2DFeaturePtr>& features = frame->features2D();

                for (size_t l = 0; l < features.size(); ++l)
                {
                    Point3DFeaturePtr& scenePoint = features.at(l)->feature3D();

                    scenePoints.insert(scenePoint);
                }
            }
        }
    }

    for (boost::unordered_set<Point3DFeaturePtr>::iterator it = scenePoints.begin();
             it != scenePoints.end(); ++it)
    {
        Point3DFeaturePtr scenePoint = *it;

        // reset 3D coordinates to those originally computed from stereo
        Frame* frame = scenePoint->features2D().front()->frame();
        FrameSet* frameSet = frame->frameSet();
        Eigen::Matrix4d pose = invertHomogeneousTransform(frameSet->systemPose()->toMatrix()) *
                               m_cameraSystem->getGlobalCameraPose(frame->cameraId());

        scenePoint->point() = transformPoint(pose, scenePoint->pointFromStereo());

        ceres::Problem problem;
        ceres::Solver::Options options;
        options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
        options.max_num_iterations = 100;

        for (size_t i = 0; i < scenePoint->features2D().size(); ++i)
        {
            Point2DFeature* feature = scenePoint->features2D().at(i);
            Frame* frame = feature->frame();
            FrameSet* frameSet = frame->frameSet();

            Eigen::Matrix4d H = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(frame->cameraId())) *
                                frameSet->systemPose()->toMatrix();

            ceres::CostFunction* costFunction =
                CostFunctionFactory::instance()->generateCostFunction(H, feature->ray());

            problem.AddResidualBlock(costFunction, 0, scenePoint->pointData());
        }

        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
    }

    reprojErrorStats(graph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error after scene point reconstruction: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);
}

void
SelfMultiCamCalibration::runBA(const SparseGraphPtr& graph,
                               const boost::shared_ptr<SparseGraphViz>& graphViz) const
{
    // run bundle adjustment
    ceres::Problem problem;

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    options.max_num_iterations = 1000;
    options.num_threads = 8;
    options.num_linear_solver_threads = 8;

    // visualize sparse graph at end of each optimization iteration
    GraphVizCallback callback(*graphViz);
    options.callbacks.push_back(&callback);
    options.update_state_every_iteration = true;

    std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond> > q_sys_cam;
    std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > t_sys_cam;
    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        Eigen::Matrix4d H_inv = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(i));

        q_sys_cam.push_back(Eigen::Quaterniond(H_inv.block<3,3>(0,0)));
        t_sys_cam.push_back(H_inv.block<3,1>(0,3));
    }

    for (size_t i = 0; i < graph->frameSetSegments().size(); ++i)
    {
        FrameSetSegment& segment = graph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                FramePtr& frame = frameSet->frames().at(k);
                int cameraId = frame->cameraId();

                std::vector<Point2DFeaturePtr>& features = frame->features2D();
                for (size_t l = 0; l < features.size(); ++l)
                {
                    Point2DFeaturePtr& feature = features.at(l);
                    Point3DFeaturePtr& scenePoint = feature->feature3D();

                    ceres::LossFunction* lossFunction = new ceres::HuberLoss(0.0000055555);

                    ceres::CostFunction* costFunction =
                        CostFunctionFactory::instance()->generateCostFunction(q_sys_cam.at(cameraId),
                                                                              t_sys_cam.at(cameraId),
                                                                              feature->ray());

                    problem.AddResidualBlock(costFunction, lossFunction,
                                             frameSet->systemPose()->rotationData(),
                                             frameSet->systemPose()->translationData(),
                                             scenePoint->pointData());
                }
            }

            ceres::LocalParameterization* quaternionParameterization =
                new EigenQuaternionParameterization;

            problem.SetParameterization(frameSet->systemPose()->rotationData(),
                                        quaternionParameterization);
        }
    }

    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    ROS_INFO_STREAM(summary.BriefReport());

    double avgError, maxError, avgScenePointDepth;
    size_t featureCount;

    reprojErrorStats(graph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error after bundle adjustment: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);
}

void
SelfMultiCamCalibration::runFullBA(const std::vector<std::string>& chessboardDataFilenames)
{
    // run full bundle adjustment
    ceres::Problem problem;

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    options.function_tolerance = 1e-8;
    options.max_num_iterations = 1000;
    options.num_threads = 8;
    options.num_linear_solver_threads = 8;

    // visualize sparse graph at end of each optimization iteration
    GraphVizCallback callback(m_sgv);
    options.callbacks.push_back(&callback);
    options.update_state_every_iteration = true;

    // intrinsics
    std::vector<std::vector<double> > intrinsicCameraParams(m_cameraSystem->cameraCount());
    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        m_cameraSystem->getCamera(i)->writeParameters(intrinsicCameraParams.at(i));
    }

    // extrinsics
    std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond> > q_sys_cam;
    std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > t_sys_cam;
    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        Eigen::Matrix4d H_inv = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(i));

        q_sys_cam.push_back(Eigen::Quaterniond(H_inv.block<3,3>(0,0)));
        t_sys_cam.push_back(H_inv.block<3,1>(0,3));
    }

    size_t nFeatureSResiduals = 0;
    size_t nFeatureMResiduals = 0;
    boost::unordered_set<Point3DFeature*> singletonScenePoints;
    for (size_t i = 0; i < m_sparseGraph->frameSetSegments().size(); ++i)
    {
        FrameSetSegment& segment = m_sparseGraph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                FramePtr& frame = frameSet->frames().at(k);
                int cameraId = frame->cameraId();

                std::vector<Point2DFeaturePtr>& features = frame->features2D();

                for (size_t l = 0; l < features.size(); ++l)
                {
                    Point2DFeaturePtr& feature = features.at(l);
                    Point3DFeaturePtr& scenePoint = feature->feature3D();

                    if (scenePoint->features2D().size() <= 2)
                    {
                        singletonScenePoints.insert(scenePoint.get());

                        continue;
                    }

                    if (scenePoint->attributes() & Point3DFeature::OBSERVED_BY_MULTIPLE_STEREO_RIGS)
                    {
                        ++nFeatureMResiduals;
                    }
                    else
                    {
                        ++nFeatureSResiduals;
                    }
                }
            }
        }
    }

    double wFeatureMResidual = static_cast<double>(nFeatureSResiduals) /
                               static_cast<double>(nFeatureMResiduals);
    for (size_t i = 0; i < m_sparseGraph->frameSetSegments().size(); ++i)
    {
        FrameSetSegment& segment = m_sparseGraph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                FramePtr& frame = frameSet->frames().at(k);
                int cameraId = frame->cameraId();

                std::vector<Point2DFeaturePtr>& features = frame->features2D();

                for (size_t l = 0; l < features.size(); ++l)
                {
                    Point2DFeaturePtr& feature = features.at(l);
                    Point3DFeaturePtr& scenePoint = feature->feature3D();

                    if (scenePoint->features2D().size() <= 2)
                    {
                        continue;
                    }

                    double weight = 1.0;
                    if (scenePoint->attributes() & Point3DFeature::OBSERVED_BY_MULTIPLE_STEREO_RIGS)
                    {
                        weight = wFeatureMResidual;
                    }

                    ceres::LossFunction* lossFunction = new ceres::ScaledLoss(new ceres::HuberLoss(0.0000055555), weight, ceres::TAKE_OWNERSHIP);

                    ceres::CostFunction* costFunction =
                        CostFunctionFactory::instance()->generateCostFunction(m_cameraSystem->getCamera(cameraId),
                                                                              Eigen::Vector2d(feature->keypoint().pt.x,
                                                                                              feature->keypoint().pt.y));

                    problem.AddResidualBlock(costFunction, lossFunction,
                                             intrinsicCameraParams.at(cameraId).data(),
                                             q_sys_cam.at(cameraId).coeffs().data(),
                                             t_sys_cam.at(cameraId).data(),
                                             frameSet->systemPose()->rotationData(),
                                             frameSet->systemPose()->translationData(),
                                             scenePoint->pointData());
                }
            }

            ceres::LocalParameterization* quaternionParameterization =
                new EigenQuaternionParameterization;

            problem.SetParameterization(frameSet->systemPose()->rotationData(),
                                        quaternionParameterization);
        }
    }

    std::vector<StereoCameraCalibration> scCalibs(m_svo.size());
    std::vector<std::vector<std::vector<cv::Point3f> > > chessboardScenePoints(m_svo.size());
    size_t nChessboardResiduals = 0;
    for (size_t i = 0; i < scCalibs.size(); ++i)
    {
        StereoCameraCalibration& scCalib = scCalibs.at(i);

        scCalib.readChessboardData(chessboardDataFilenames.at(i));

        nChessboardResiduals += scCalib.scenePoints().size() *
                                scCalib.scenePoints().front().size();

        chessboardScenePoints.at(i) = scCalib.scenePoints();

        // print out reprojection error from chessboard data for left camera
        std::vector<cv::Mat> rvecs1(chessboardScenePoints.at(i).size());
        std::vector<cv::Mat> tvecs1(chessboardScenePoints.at(i).size());

        for (int j = 0; j < scCalib.cameraPosesLeft().rows; ++j)
        {
            cv::Mat rvec(3, 1, CV_64F);
            rvec.at<double>(0) =  scCalib.cameraPosesLeft().at<double>(j,0);
            rvec.at<double>(1) =  scCalib.cameraPosesLeft().at<double>(j,1);
            rvec.at<double>(2) =  scCalib.cameraPosesLeft().at<double>(j,2);

            cv::Mat tvec(3, 1, CV_64F);
            tvec.at<double>(0) =  scCalib.cameraPosesLeft().at<double>(j,3);
            tvec.at<double>(1) =  scCalib.cameraPosesLeft().at<double>(j,4);
            tvec.at<double>(2) =  scCalib.cameraPosesLeft().at<double>(j,5);

            rvecs1.at(j) = rvec;
            tvecs1.at(j) = tvec;
        }

        double err1 = m_cameraSystem->getCamera(i * 2)->reprojectionError(chessboardScenePoints.at(i),
                                                                          scCalib.imagePointsLeft(),
                                                                          rvecs1, tvecs1);
        ROS_INFO("[%s] Initial reprojection error (chessboard): %.3f pixels",
                 m_cameraSystem->getCamera(i * 2)->cameraName().c_str(), err1);

        // print out reprojection error from chessboard data for right camera
        std::vector<cv::Mat> rvecs2(chessboardScenePoints.at(i).size());
        std::vector<cv::Mat> tvecs2(chessboardScenePoints.at(i).size());

        for (int j = 0; j < scCalib.cameraPosesRight().rows; ++j)
        {
            cv::Mat rvec(3, 1, CV_64F);
            rvec.at<double>(0) =  scCalib.cameraPosesRight().at<double>(j,0);
            rvec.at<double>(1) =  scCalib.cameraPosesRight().at<double>(j,1);
            rvec.at<double>(2) =  scCalib.cameraPosesRight().at<double>(j,2);

            cv::Mat tvec(3, 1, CV_64F);
            tvec.at<double>(0) =  scCalib.cameraPosesRight().at<double>(j,3);
            tvec.at<double>(1) =  scCalib.cameraPosesRight().at<double>(j,4);
            tvec.at<double>(2) =  scCalib.cameraPosesRight().at<double>(j,5);

            rvecs2.at(j) = rvec;
            tvecs2.at(j) = tvec;
        }

        double err2 = m_cameraSystem->getCamera(i * 2 + 1)->reprojectionError(chessboardScenePoints.at(i),
                                                                              scCalib.imagePointsRight(),
                                                                              rvecs2, tvecs2);
        ROS_INFO("[%s] Initial reprojection error (chessboard): %.3f pixels",
                 m_cameraSystem->getCamera(i * 2 + 1)->cameraName().c_str(), err2);
    }

    double wChessboardResidual = static_cast<double>(nFeatureSResiduals) /
                                 static_cast<double>(nChessboardResiduals);

    ROS_INFO("Optimizing over %lu features seen by one stereo rig, %lu features seen by multiple stereo rigs, and %lu corners",
             nFeatureSResiduals, nFeatureMResiduals, nChessboardResiduals);
    ROS_INFO("Assigned weight of %.2f to residuals corresponding to features seen by multiple stereo rigs.", wFeatureMResidual);
    ROS_INFO("Assigned weight of %.2f to corner residuals.", wChessboardResidual);

    std::vector<std::vector<std::vector<double> > > chessboardCameraPoses1(m_svo.size());

    for (size_t i = 0; i < scCalibs.size(); ++i)
    {
        StereoCameraCalibration& scCalib = scCalibs.at(i);

        const cv::Mat& cameraPoses1 = scCalib.cameraPosesLeft();

        chessboardCameraPoses1.at(i).resize(cameraPoses1.rows);

        const std::vector<std::vector<cv::Point2f> >& imagePoints1 = scCalib.imagePointsLeft();
        const std::vector<std::vector<cv::Point2f> >& imagePoints2 = scCalib.imagePointsRight();
        const std::vector<std::vector<cv::Point3f> >& scenePoints = chessboardScenePoints.at(i);

        for (size_t j = 0; j < scenePoints.size(); ++j)
        {
            chessboardCameraPoses1.at(i).at(j).resize(7);

            Eigen::Vector3d rvec1(cameraPoses1.at<double>(j,0),
                                  cameraPoses1.at<double>(j,1),
                                  cameraPoses1.at<double>(j,2));

            AngleAxisToQuaternion(rvec1, chessboardCameraPoses1.at(i).at(j).data());

            chessboardCameraPoses1.at(i).at(j).at(4) = cameraPoses1.at<double>(j,3);
            chessboardCameraPoses1.at(i).at(j).at(5) = cameraPoses1.at<double>(j,4);
            chessboardCameraPoses1.at(i).at(j).at(6) = cameraPoses1.at<double>(j,5);

            for (size_t k = 0; k < scenePoints.at(j).size(); ++k)
            {
                const cv::Point3f& spt = scenePoints.at(j).at(k);
                const cv::Point2f& ipt1 = imagePoints1.at(j).at(k);
                const cv::Point2f& ipt2 = imagePoints2.at(j).at(k);

                ceres::CostFunction* costFunction =
                    CostFunctionFactory::instance()->generateCostFunction(m_cameraSystem->getCamera(i * 2),
                                                                          Eigen::Vector2d(ipt1.x, ipt1.y),
                                                                          m_cameraSystem->getCamera(i * 2 + 1),
                                                                          Eigen::Vector2d(ipt2.x, ipt2.y),
                                                                          Eigen::Vector3d(spt.x, spt.y, spt.z),
                                                                          CAMERA_INTRINSICS | SYSTEM_CAMERA_TRANSFORM | SYSTEM_POSE);

                ceres::LossFunction* lossFunction = new ceres::ScaledLoss(new ceres::CauchyLoss(1.0), wChessboardResidual, ceres::TAKE_OWNERSHIP);
                problem.AddResidualBlock(costFunction, lossFunction,
                                         intrinsicCameraParams.at(i * 2).data(),
                                         intrinsicCameraParams.at(i * 2 + 1).data(),
                                         q_sys_cam.at(i * 2).coeffs().data(),
                                         t_sys_cam.at(i * 2).data(),
                                         q_sys_cam.at(i * 2 + 1).coeffs().data(),
                                         t_sys_cam.at(i * 2 + 1).data(),
                                         chessboardCameraPoses1.at(i).at(j).data(),
                                         chessboardCameraPoses1.at(i).at(j).data() + 4);
            }

            ceres::LocalParameterization* quaternionParameterization =
                new EigenQuaternionParameterization;

            problem.SetParameterization(chessboardCameraPoses1.at(i).at(j).data(),
                                        quaternionParameterization);
        }
    }

    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        ceres::LocalParameterization* quaternionParameterization =
            new EigenQuaternionParameterization;

        problem.SetParameterization(q_sys_cam.at(i).coeffs().data(),
                                    quaternionParameterization);
    }

    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    ROS_INFO_STREAM(summary.BriefReport());

    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        m_cameraSystem->getCamera(i)->readParameters(intrinsicCameraParams.at(i));

        Eigen::Matrix4d H = Eigen::Matrix4d::Identity();
        H.block<3,3>(0,0) = q_sys_cam.at(i).toRotationMatrix();
        H.block<3,1>(0,3) = t_sys_cam.at(i);

        Eigen::Matrix4d H_inv = invertHomogeneousTransform(H);

        m_cameraSystem->setGlobalCameraPose(i, H_inv);
    }

    for (size_t i = 0; i < scCalibs.size(); ++i)
    {
        StereoCameraCalibration& scCalib = scCalibs.at(i);

        Eigen::Matrix4d H_1_2 = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(i * 2 + 1)) *
                                m_cameraSystem->getGlobalCameraPose(i * 2);
        Eigen::Quaterniond q_1_2(H_1_2.block<3,3>(0,0));
        Eigen::Vector3d t_1_2 = H_1_2.block<3,1>(0,3);

        for (int j = 0; j < scCalib.cameraPosesLeft().rows; ++j)
        {
            Eigen::Quaterniond q_1;
            q_1.x() = chessboardCameraPoses1.at(i).at(j).at(0);
            q_1.y() = chessboardCameraPoses1.at(i).at(j).at(1);
            q_1.z() = chessboardCameraPoses1.at(i).at(j).at(2);
            q_1.w() = chessboardCameraPoses1.at(i).at(j).at(3);

            Eigen::Vector3d t_1;
            t_1(0) = chessboardCameraPoses1.at(i).at(j).at(4);
            t_1(1) = chessboardCameraPoses1.at(i).at(j).at(5);
            t_1(2) = chessboardCameraPoses1.at(i).at(j).at(6);

            Eigen::AngleAxisd aa_1(q_1);
            Eigen::Vector3d rvec = aa_1.angle() * aa_1.axis();

            scCalib.cameraPosesLeft().at<double>(j,0) = rvec(0);
            scCalib.cameraPosesLeft().at<double>(j,1) = rvec(1);
            scCalib.cameraPosesLeft().at<double>(j,2) = rvec(2);
            scCalib.cameraPosesLeft().at<double>(j,3) = t_1(0);
            scCalib.cameraPosesLeft().at<double>(j,4) = t_1(1);
            scCalib.cameraPosesLeft().at<double>(j,5) = t_1(2);

            Eigen::Quaterniond q_2 = q_1_2 * q_1;
            Eigen::Vector3d t_2 = q_1_2 * t_1 + t_1_2;

            Eigen::AngleAxisd aa_2(q_2);
            rvec = aa_2.angle() * aa_2.axis();

            scCalib.cameraPosesRight().at<double>(j,0) = rvec(0);
            scCalib.cameraPosesRight().at<double>(j,1) = rvec(1);
            scCalib.cameraPosesRight().at<double>(j,2) = rvec(2);
            scCalib.cameraPosesRight().at<double>(j,3) = t_2(0);
            scCalib.cameraPosesRight().at<double>(j,4) = t_2(1);
            scCalib.cameraPosesRight().at<double>(j,5) = t_2(2);
        }

        // print out reprojection error from chessboard data for left camera
        std::vector<cv::Mat> rvecs1(chessboardScenePoints.at(i).size());
        std::vector<cv::Mat> tvecs1(chessboardScenePoints.at(i).size());

        for (int j = 0; j < scCalib.cameraPosesLeft().rows; ++j)
        {
            cv::Mat rvec(3, 1, CV_64F);
            rvec.at<double>(0) =  scCalib.cameraPosesLeft().at<double>(j,0);
            rvec.at<double>(1) =  scCalib.cameraPosesLeft().at<double>(j,1);
            rvec.at<double>(2) =  scCalib.cameraPosesLeft().at<double>(j,2);

            cv::Mat tvec(3, 1, CV_64F);
            tvec.at<double>(0) =  scCalib.cameraPosesLeft().at<double>(j,3);
            tvec.at<double>(1) =  scCalib.cameraPosesLeft().at<double>(j,4);
            tvec.at<double>(2) =  scCalib.cameraPosesLeft().at<double>(j,5);

            rvecs1.at(j) = rvec;
            tvecs1.at(j) = tvec;
        }

        double err1 = m_cameraSystem->getCamera(i * 2)->reprojectionError(chessboardScenePoints.at(i),
                                                                          scCalib.imagePointsLeft(),
                                                                          rvecs1, tvecs1);
        ROS_INFO("[%s] Final reprojection error (chessboard): %.3f pixels",
                 m_cameraSystem->getCamera(i * 2)->cameraName().c_str(), err1);

        // print out reprojection error from chessboard data for right camera
        std::vector<cv::Mat> rvecs2(chessboardScenePoints.at(i).size());
        std::vector<cv::Mat> tvecs2(chessboardScenePoints.at(i).size());

        for (int j = 0; j < scCalib.cameraPosesRight().rows; ++j)
        {
            cv::Mat rvec(3, 1, CV_64F);
            rvec.at<double>(0) =  scCalib.cameraPosesRight().at<double>(j,0);
            rvec.at<double>(1) =  scCalib.cameraPosesRight().at<double>(j,1);
            rvec.at<double>(2) =  scCalib.cameraPosesRight().at<double>(j,2);

            cv::Mat tvec(3, 1, CV_64F);
            tvec.at<double>(0) =  scCalib.cameraPosesRight().at<double>(j,3);
            tvec.at<double>(1) =  scCalib.cameraPosesRight().at<double>(j,4);
            tvec.at<double>(2) =  scCalib.cameraPosesRight().at<double>(j,5);

            rvecs2.at(j) = rvec;
            tvecs2.at(j) = tvec;
        }

        double err2 = m_cameraSystem->getCamera(i * 2 + 1)->reprojectionError(chessboardScenePoints.at(i),
                                                                              scCalib.imagePointsRight(),
                                                                              rvecs2, tvecs2);
        ROS_INFO("[%s] Final reprojection error (chessboard): %.3f pixels",
                 m_cameraSystem->getCamera(i * 2 + 1)->cameraName().c_str(), err2);
    }

    for (boost::unordered_set<Point3DFeature*>::iterator it = singletonScenePoints.begin();
         it != singletonScenePoints.end(); ++it)
    {
        Point3DFeature* scenePoint = *it;

        // reset 3D coordinates to those originally computed from stereo
        Frame* frame = scenePoint->features2D().front()->frame();
        FrameSet* frameSet = frame->frameSet();
        Eigen::Matrix4d pose = invertHomogeneousTransform(frameSet->systemPose()->toMatrix()) *
                               m_cameraSystem->getGlobalCameraPose(frame->cameraId());

        scenePoint->point() = transformPoint(pose, scenePoint->pointFromStereo());
    }

    double avgError, maxError, avgScenePointDepth;
    size_t featureCount;

    reprojErrorStats(m_sparseGraph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error after full bundle adjustment: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);
}

bool
SelfMultiCamCalibration::runPoseIMUCalibration(void)
{
    // find pose-IMU transform
    std::vector<PoseConstPtr> poseData;
    std::vector<sensor_msgs::ImuConstPtr> imuData;
    for (size_t i = 0; i < m_sparseGraph->frameSetSegment(0).size(); ++i)
    {
        FrameSetPtr& frameSet = m_sparseGraph->frameSetSegment(0).at(i);

        poseData.push_back(frameSet->systemPose());
        imuData.push_back(frameSet->imuMeasurement());
    }

    PoseIMUCalibration calib;
    Eigen::Quaterniond q_sys_imu;
    if (!calib.calibrate(poseData, imuData, q_sys_imu))
    {
        return false;
    }

    double avgError, maxError;
    calib.errorStats(q_sys_imu, avgError, maxError);

    ROS_INFO_STREAM("Avg error: " << r2d(avgError));
    ROS_INFO_STREAM("Max error: " << r2d(maxError));

    ROS_INFO("Rotation between system and IMU:");
    ROS_INFO_STREAM(q_sys_imu.toRotationMatrix());

    Eigen::Matrix4d H_sys_imu = Eigen::Matrix4d::Identity();
    H_sys_imu.block<3,3>(0,0) = q_sys_imu.toRotationMatrix();

    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        Eigen::Matrix4d pose = H_sys_imu * m_cameraSystem->getGlobalCameraPose(i);
        m_cameraSystem->setGlobalCameraPose(i, pose);
    }

    return true;
}

std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> >
SelfMultiCamCalibration::computeRelativeSystemPoses(const std::vector<FrameSetPtr>& frameSets) const
{
    std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > relPoses;

    if (frameSets.size() < 2)
    {
        return relPoses;
    }

    for (size_t i = 0; i < frameSets.size() - 1; ++i)
    {
        const FrameSetConstPtr& frameSet1 = frameSets.at(i);
        const FrameSetConstPtr& frameSet2 = frameSets.at(i + 1);

        Eigen::Matrix4d H = frameSet2->systemPose()->toMatrix() *
                            invertHomogeneousTransform(frameSet1->systemPose()->toMatrix());

        relPoses.push_back(H);
    }

    return relPoses;
}

void
SelfMultiCamCalibration::reconstructScenePoint(Point3DFeaturePtr& scenePoint) const
{
    size_t nFeatures = scenePoint->features2D().size();

    std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > H_sys_cam;
    for (int i = 0; i < m_cameraSystem->cameraCount(); ++i)
    {
        Eigen::Matrix4d H_inv = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(i));

        H_sys_cam.push_back(H_inv);
    }

    Eigen::MatrixXd A(nFeatures * 3, 3 + nFeatures);
    A.setZero();

    Eigen::VectorXd b(nFeatures * 3);

    for (size_t i = 0; i < scenePoint->features2D().size(); ++i)
    {
        Point2DFeature* feature = scenePoint->features2D().at(i);
        Frame* frame = feature->frame();
        FrameSet* frameSet = frame->frameSet();

        Eigen::Matrix4d pose = H_sys_cam.at(frame->cameraId()) * frameSet->systemPose()->toMatrix();

        A.block<3,3>(i * 3, 0) = - pose.block<3,3>(0,0);
        A.block<3,1>(i * 3, 3 + i) = feature->ray();

        b.block<3,1>(i * 3, 0) = pose.block<3,1>(0,3);
    }

    Eigen::VectorXd x = A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);

    scenePoint->point() = x.block<3,1>(0,0);
}

void
SelfMultiCamCalibration::mergeMaps(void)
{
    if (m_svo.size() < 2)
    {
        // no maps to merge
        return;
    }

    // Assume that the reference frame of the first stereo camera is the same
    // as that of the camera system.
    // Use the poses of the first stereo camera as the initial poses of
    // the camera system.
    SparseGraphPtr& graphRef = m_subSparseGraphs.at(0);
    for (size_t i = 0; i < graphRef->frameSetSegment(0).size(); ++i)
    {
        FrameSetPtr& src = graphRef->frameSetSegment(0).at(i);
        FrameSetPtr dst = boost::make_shared<FrameSet>();

        m_sparseGraph->frameSetSegment(0).push_back(dst);
        dst->frames().insert(dst->frames().end(),
                             src->frames().begin(), src->frames().end());
        dst->systemPose() = src->systemPose();
        dst->imuMeasurement() = src->imuMeasurement();
        dst->groundTruthMeasurement() = src->groundTruthMeasurement();

        for (size_t j = 0; j < src->frames().size(); ++j)
        {
            src->frames().at(j)->frameSet() = dst.get();
        }
    }

    // Use the inter-camera transforms estimated from hand-eye calibration
    // to compute the initial coordinates of the scene points observed
    // from a stereo camera other than the first stereo camera.
    for (size_t i = 1; i < m_svo.size(); ++i)
    {
        boost::unordered_set<Point3DFeaturePtr> scenePoints;

        SparseGraphPtr& graph = m_subSparseGraphs.at(i);

        for (size_t j = 0; j < graph->frameSetSegment(0).size(); ++j)
        {
            FrameSetPtr& src = graph->frameSetSegment(0).at(j);
            FrameSetPtr& dst = m_sparseGraph->frameSetSegment(0).at(j);
            dst->frames().insert(dst->frames().end(),
                                 src->frames().begin(),
                                 src->frames().end());

            FramePtr& frame = src->frames().at(0);
            for (size_t k = 0; k < src->frames().size(); ++k)
            {
                src->frames().at(k)->frameSet() = dst.get();
            }
            for (size_t k = 0; k < frame->features2D().size(); ++k)
            {
                scenePoints.insert(frame->features2D().at(k)->feature3D());
            }
        }

        for (boost::unordered_set<Point3DFeaturePtr>::iterator it = scenePoints.begin();
             it != scenePoints.end(); ++it)
        {
            Point3DFeaturePtr scenePoint = *it;

            // reset 3D coordinates to those originally computed from stereo
            Frame* frame = scenePoint->features2D().front()->frame();
            FrameSet* frameSet = frame->frameSet();
            Eigen::Matrix4d pose = invertHomogeneousTransform(frameSet->systemPose()->toMatrix()) *
                                   m_cameraSystem->getGlobalCameraPose(frame->cameraId());

            scenePoint->point() = transformPoint(pose, scenePoint->pointFromStereo());
        }
    }

    double avgError, maxError, avgScenePointDepth;
    size_t featureCount;

    reprojErrorStats(m_sparseGraph, avgError, maxError, avgScenePointDepth, featureCount);

    ROS_INFO("Reprojection error after map merging: avg = %.3f | max = %.3f | avg depth = %.3f | count = %lu",
             avgError, maxError, avgScenePointDepth, featureCount);
}

void
SelfMultiCamCalibration::reprojErrorStats(const SparseGraphConstPtr& graph,
                                          double& avgError, double& maxError,
                                          double& avgScenePointDepth,
                                          size_t& featureCount) const
{
    size_t count = 0;
    double sumError = 0.0;
    maxError = 0.0;
    double sumScenePointDepth = 0.0;
    for (size_t i = 0; i < graph->frameSetSegments().size(); ++i)
    {
        const FrameSetSegment& segment = graph->frameSetSegment(i);

        for (size_t j = 0; j < segment.size(); ++j)
        {
            const FrameSetPtr& frameSet = segment.at(j);

            for (size_t k = 0; k < frameSet->frames().size(); ++k)
            {
                FramePtr& frame = frameSet->frames().at(k);
                int cameraId = frame->cameraId();

                for (size_t l = 0; l < frame->features2D().size(); ++l)
                {
                    const Point2DFeaturePtr& feature = frame->features2D().at(l);
                    const Point3DFeaturePtr& scenePoint = feature->feature3D();

                    Eigen::Matrix4d pose = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(cameraId)) *
                                           frameSet->systemPose()->toMatrix();

                    Eigen::Vector3d P = transformPoint(pose, scenePoint->point());
                    Eigen::Vector2d p;
                    m_cameraSystem->getCamera(cameraId)->spaceToPlane(P, p);

                    double err = (p - Eigen::Vector2d(feature->keypoint().pt.x,
                                                      feature->keypoint().pt.y)).norm();

                    ++count;
                    sumError += err;

                    if (maxError < err)
                    {
                        maxError = err;
                    }

                    sumScenePointDepth += P.norm();
                }
            }
        }
    }

    if (count == 0)
    {
        avgError = 0.0;
        avgScenePointDepth = 0.0;
        featureCount = 0;
    }
    else
    {
        avgError = sumError / static_cast<double>(count);
        avgScenePointDepth = sumScenePointDepth / static_cast<double>(count);
        featureCount = count;
    }
}

}