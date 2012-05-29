#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <image_geometry/stereo_camera_model.h>
#include <opencv2/calib3d/calib3d.hpp>

#include <sensor_msgs/image_encodings.h>
#include <stereo_msgs/DisparityImage.h>

#include <frame_image_proc/DisparityConfig.h>
#include <dynamic_reconfigure/server.h>

#include <frame_image_proc/matcher.h>

#include <frame_common/Frame.h>

namespace frame_image_proc {

using namespace sensor_msgs;
using namespace stereo_msgs;
using namespace message_filters::sync_policies;

class FrameDisparityNodelet : public nodelet::Nodelet
{
  boost::shared_ptr<image_transport::ImageTransport> it_;
  
  // Subscriptions
  image_transport::SubscriberFilter sub_l_image_, sub_r_image_;
  message_filters::Subscriber<CameraInfo> sub_l_info_, sub_r_info_;
  typedef ExactTime<Image, CameraInfo, Image, CameraInfo> ExactPolicy;
  typedef ApproximateTime<Image, CameraInfo, Image, CameraInfo> ApproximatePolicy;
  typedef message_filters::Synchronizer<ExactPolicy> ExactSync;
  typedef message_filters::Synchronizer<ApproximatePolicy> ApproximateSync;
  boost::shared_ptr<ExactSync> exact_sync_;
  boost::shared_ptr<ApproximateSync> approximate_sync_;

  // Publications
  boost::mutex connect_mutex_;
  ros::Publisher pub_disparity_;
  ros::Publisher pub_frame_;

  // Dynamic reconfigure
  boost::recursive_mutex config_mutex_;
  typedef frame_image_proc::DisparityConfig Config;
  typedef dynamic_reconfigure::Server<Config> ReconfigureServer;
  boost::shared_ptr<ReconfigureServer> reconfigure_server_;
  
  // Processing state (note: only safe because we're single-threaded!)
  image_geometry::StereoCameraModel model_;
  StereoMatcher block_matcher_; // contains scratch buffers for block matching

  /// Feature Detector used for finding features in the image.
  cv::Ptr<cv::FeatureDetector> detector;
  /// Descriptor Extractor used for getting descriptors around image features.
  cv::Ptr<cv::DescriptorExtractor> extractor;

  virtual void onInit();

  void connectCb();

  void imageCb(const ImageConstPtr& l_image_msg, const CameraInfoConstPtr& l_info_msg,
               const ImageConstPtr& r_image_msg, const CameraInfoConstPtr& r_info_msg);

  void publishFrame(const cv::Mat &l_image, const cv::Mat &r_image, const cv::Mat &disp_image,
                    const CameraInfoConstPtr& l_info_msg,
                    const CameraInfoConstPtr& r_info_msg
          );

  void configCb(Config &config, uint32_t level);
};

void FrameDisparityNodelet::onInit()
{
  ros::NodeHandle &nh = getNodeHandle();
  ros::NodeHandle &private_nh = getPrivateNodeHandle();

  it_.reset(new image_transport::ImageTransport(nh));

  // Synchronize inputs. Topic subscriptions happen on demand in the connection
  // callback. Optionally do approximate synchronization.
  int queue_size;
  private_nh.param("queue_size", queue_size, 5);
  bool approx;
  private_nh.param("approximate_sync", approx, false);
  if (approx)
  {
    approximate_sync_.reset( new ApproximateSync(ApproximatePolicy(queue_size),
                                                 sub_l_image_, sub_l_info_,
                                                 sub_r_image_, sub_r_info_) );
    approximate_sync_->registerCallback(boost::bind(&FrameDisparityNodelet::imageCb,
                                                    this, _1, _2, _3, _4));
  }
  else
  {
    exact_sync_.reset( new ExactSync(ExactPolicy(queue_size),
                                     sub_l_image_, sub_l_info_,
                                     sub_r_image_, sub_r_info_) );
    exact_sync_->registerCallback(boost::bind(&FrameDisparityNodelet::imageCb,
                                              this, _1, _2, _3, _4));
  }

  int v;
  private_nh.param("feature_detector_threshold", v, 25);
  // detector (by default, FAST)
  detector = new cv::GridAdaptedFeatureDetector(new cv::FastFeatureDetector(v), 1000);
  // detector = new cv::FastFeatureDetector(v);

  // descriptor (by default, SURF)
  extractor = cv::DescriptorExtractor::create("SURF");

  // Set up dynamic reconfiguration
  ReconfigureServer::CallbackType f = boost::bind(&FrameDisparityNodelet::configCb,
                                                  this, _1, _2);
  reconfigure_server_.reset(new ReconfigureServer(config_mutex_, private_nh));
  reconfigure_server_->setCallback(f);

  // Monitor whether anyone is subscribed to the output
  ros::SubscriberStatusCallback connect_cb = boost::bind(&FrameDisparityNodelet::connectCb, this);
  // Make sure we don't enter connectCb() between advertising and assigning to pub_disparity_
  boost::lock_guard<boost::mutex> lock(connect_mutex_);
  pub_disparity_ = nh.advertise<DisparityImage>("disparity", 1, connect_cb, connect_cb);
  pub_frame_ = nh.advertise<frame_common::Frame>("frame", 1, connect_cb, connect_cb);
}

// Handles (un)subscribing when clients (un)subscribe
void FrameDisparityNodelet::connectCb()
{
  boost::lock_guard<boost::mutex> lock(connect_mutex_);
  if (pub_disparity_.getNumSubscribers() == 0 && pub_frame_.getNumSubscribers() == 0)
  {
    sub_l_image_.unsubscribe();
    sub_l_info_ .unsubscribe();
    sub_r_image_.unsubscribe();
    sub_r_info_ .unsubscribe();
  }
  else if (!sub_l_image_.getSubscriber())
  {
    ros::NodeHandle &nh = getNodeHandle();
    // Queue size 1 should be OK; the one that matters is the synchronizer queue size.
    /// @todo Allow remapping left, right?
    sub_l_image_.subscribe(*it_, "left/image_rect", 1);
    sub_l_info_ .subscribe(nh,   "left/camera_info", 1);
    sub_r_image_.subscribe(*it_, "right/image_rect", 1);
    sub_r_info_ .subscribe(nh,   "right/camera_info", 1);
  }
}

void FrameDisparityNodelet::imageCb(const ImageConstPtr& l_image_msg,
                               const CameraInfoConstPtr& l_info_msg,
                               const ImageConstPtr& r_image_msg,
                               const CameraInfoConstPtr& r_info_msg)
{
  /// @todo Convert (share) with new cv_bridge
  assert(l_image_msg->encoding == sensor_msgs::image_encodings::MONO8);
  assert(r_image_msg->encoding == sensor_msgs::image_encodings::MONO8);

  // Update the camera model
  model_.fromCameraInfo(l_info_msg, r_info_msg);
  
  // Allocate new disparity image message
  DisparityImagePtr disp_msg = boost::make_shared<DisparityImage>();
  disp_msg->header         = l_info_msg->header;
  disp_msg->image.header   = l_info_msg->header;
  disp_msg->image.height   = l_image_msg->height;
  disp_msg->image.width    = l_image_msg->width;
  disp_msg->image.encoding = sensor_msgs::image_encodings::TYPE_32FC1;
  disp_msg->image.step     = disp_msg->image.width * sizeof(float);
  disp_msg->image.data.resize(disp_msg->image.height * disp_msg->image.step);

  // Stereo parameters
  disp_msg->f = model_.right().fx();
  disp_msg->T = model_.baseline();

  // Compute window of (potentially) valid disparities
  int left, top, right, bottom;
  block_matcher_.determineValidWindow(disp_msg->image.width, disp_msg->image.height, left, top, right, bottom);
  disp_msg->valid_window.x_offset = left;
  disp_msg->valid_window.y_offset = top;
  disp_msg->valid_window.width    = right - left;
  disp_msg->valid_window.height   = bottom - top;
  // Disparity search range
  disp_msg->min_disparity = block_matcher_.getMinDisparity();
  disp_msg->max_disparity = block_matcher_.getMinDisparity() + block_matcher_.getDisparityRange();
  disp_msg->delta_d = 1.0 / 16; // OpenCV uses 16 disparities per pixel

  // Create cv::Mat views onto all buffers
  const cv::Mat_<uint8_t> l_image(l_image_msg->height, l_image_msg->width,
                                  const_cast<uint8_t*>(&l_image_msg->data[0]),
                                  l_image_msg->step);
  const cv::Mat_<uint8_t> r_image(r_image_msg->height, r_image_msg->width,
                                  const_cast<uint8_t*>(&r_image_msg->data[0]),
                                  r_image_msg->step);
  cv::Mat_<float> disp_image(disp_msg->image.height, disp_msg->image.width,
                             reinterpret_cast<float*>(&disp_msg->image.data[0]),
                             disp_msg->image.step);

  // Perform block matching to find the disparities
  block_matcher_(l_image, r_image, disp_image, CV_32F);

  // Adjust for any x-offset between the principal points: d' = d - (cx_l - cx_r)
  double cx_l = model_.left().cx();
  double cx_r = model_.right().cx();
  if (cx_l != cx_r)
    cv::subtract(disp_image, cv::Scalar(cx_l - cx_r), disp_image);

  if(pub_frame_.getNumSubscribers()>0)
      publishFrame(l_image, r_image, disp_image, l_info_msg, r_info_msg);

  pub_disparity_.publish(disp_msg);
}

void FrameDisparityNodelet::publishFrame(const cv::Mat &l_image,
                                         const cv::Mat &r_image,
                                         const cv::Mat &disp_image,
                                         const CameraInfoConstPtr& l_info_msg,
                                         const CameraInfoConstPtr& r_info_msg
                                        )
{
    frame_common::FramePtr frame_msg = boost::make_shared<frame_common::Frame>();
    frame_msg->header = l_info_msg->header;
    frame_msg->l_info   = *l_info_msg;
    frame_msg->r_info   = *r_info_msg;
    
    std::vector<cv::KeyPoint> kpts;
    detector->detect(l_image, kpts);

    cv::Mat dtors;
    extractor->compute(l_image, kpts, dtors);

    ROS_INFO("dtors.type: %d", dtors.type());
    ROS_INFO("dtors (rows, cols, channels): (%d, %d, %d)", dtors.rows, dtors.cols, dtors.channels());

    int nkpts = kpts.size();
    frame_msg->keypoints.clear();
    frame_msg->keypoints.resize(nkpts);

    for (int i=0; i<nkpts; i++) {
        double disp = disp_image.at<float>(kpts[i].pt.y, kpts[i].pt.y);
        frame_msg->keypoints[i].x        = kpts[i].pt.x;
        frame_msg->keypoints[i].y        = kpts[i].pt.y;
        frame_msg->keypoints[i].d        = disp;
        frame_msg->keypoints[i].size     = kpts[i].size;
        frame_msg->keypoints[i].angle    = kpts[i].angle;
        frame_msg->keypoints[i].response = kpts[i].response;
        frame_msg->keypoints[i].octave   = kpts[i].octave;
        frame_msg->keypoints[i].class_id = kpts[i].class_id;
        frame_msg->keypoints[i].descriptor.clear();
        std::copy(dtors.row(i).begin<float>(), dtors.row(i).end<float>(),
                std::back_inserter(frame_msg->keypoints[i].descriptor));

        if (disp > 0.0){          // good disparity
            frame_msg->keypoints[i].goodPt = true;
        } else {
            frame_msg->keypoints[i].d = 10.;
            frame_msg->keypoints[i].goodPt = false;
        }
    }
    pub_frame_.publish(frame_msg);
}

/*
void FrameDisparityNodelet::setFrameDetector(const cv::Ptr<cv::FeatureDetector>& new_detector)
{
  detector = new_detector;
}

void FrameDisparityNodelet::setFrameDescriptor(const cv::Ptr<cv::DescriptorExtractor>& new_extractor)
{
  extractor = new_extractor;
}
*/
void FrameDisparityNodelet::configCb(Config &config, uint32_t level)
{
  // Tweak all settings to be valid
  config.prefilter_size |= 0x1; // must be odd
  config.correlation_window_size |= 0x1; // must be odd
  config.disparity_range = (config.disparity_range / 16) * 16; // must be multiple of 16

  // Note: With single-threaded NodeHandle, configCb and imageCb can't be called
  // concurrently, so this is thread-safe.
  block_matcher_.setPreFilterSize(        config.prefilter_size);
  block_matcher_.setPreFilterCap(         config.prefilter_cap);
  block_matcher_.setCorrelationWindowSize(config.correlation_window_size);
  block_matcher_.setMinDisparity(         config.min_disparity);
  block_matcher_.setDisparityRange(       config.disparity_range);
  block_matcher_.setUniquenessRatio(      config.uniqueness_ratio);
  block_matcher_.setTextureThreshold(     config.texture_threshold);
  block_matcher_.setSpeckleSize(          config.speckle_size);
  block_matcher_.setSpeckleRange(         config.speckle_range);
  block_matcher_.use_gpu = config.use_gpu;
}

} // namespace frame_image_proc

// Register nodelet
#include <pluginlib/class_list_macros.h>
PLUGINLIB_DECLARE_CLASS(frame_image_proc, frame_disparity,
                        frame_image_proc::FrameDisparityNodelet, nodelet::Nodelet)
