// Copyright 2019 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "point_cloud.hh"
#include <ignition/common/Event.hh>
#include <ignition/gazebo/components/Name.hh>
#include <ignition/gazebo/components/RgbdCamera.hh>
#include <ignition/gazebo/Util.hh>
#include <ignition/plugin/Register.hh>
#include <ignition/rendering/Camera.hh>
#include <ignition/rendering/DepthCamera.hh>
#include <ignition/rendering/RenderEngine.hh>
#include <ignition/rendering/RenderingIface.hh>
#include <ignition/rendering/Scene.hh>
#include <ignition/sensors/RgbdCameraSensor.hh>

#include <ros/ros.h>
#include <ros/advertise_options.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/fill_image.h>
#include <sensor_msgs/point_cloud2_iterator.h>

IGNITION_ADD_PLUGIN(
    ros1_ign_point_cloud::PointCloud,
    ignition::gazebo::System,
    ros1_ign_point_cloud::PointCloud::ISystemConfigure,
    ros1_ign_point_cloud::PointCloud::ISystemPostUpdate)

using namespace ros1_ign_point_cloud;

//////////////////////////////////////////////////
class ros1_ign_point_cloud::PointCloudPrivate
{
  /// \brief Callback when the depth camera generates a new frame.
  /// This is called in the rendering thread.
  /// \param[in] _scan Depth image data
  /// \param[in] _width Image width in pixels
  /// \param[in] _height Image height in pixels
  /// \param[in] _channels Number of channels in image.
  /// \param[in] _format Image format as string.
  public: void OnNewDepthFrame(const float *_scan,
            unsigned int _width, unsigned int _height,
            unsigned int _channels,
            const std::string &_format);

  /// \brief Get depth camera from rendering.
  /// \param[in] _ecm Immutable reference to ECM.
  public: void LoadDepthCamera(const ignition::gazebo::EntityComponentManager &_ecm);

  /// \brief Get RGB camera from rendering.
  /// \param[in] _ecm Immutable reference to ECM.
  public: void LoadRgbCamera(const ignition::gazebo::EntityComponentManager &_ecm);

  /// \brief Rendering scene which manages the cameras.
  public: ignition::rendering::ScenePtr scene_;

  /// \brief Entity ID for sensor within Gazebo.
  public: ignition::gazebo::Entity entity_;

  /// \brief Rendering depth camera
  public: std::shared_ptr<ignition::rendering::DepthCamera> depth_camera_;

  /// \brief Rendering RGB camera
  public: std::shared_ptr<ignition::rendering::Camera> rgb_camera_;

  /// \brief Keep latest image from RGB camera.
  public: ignition::rendering::Image rgb_image_;

  /// \brief Message populated with latest image from RGB camera.
  public: sensor_msgs::Image rgb_image_msg_;

  /// \brief Connection to depth frame event.
  public: ignition::common::ConnectionPtr depth_connection_;

  /// \brief Node to publish ROS messages.
  public: std::unique_ptr<ros::NodeHandle> rosnode_;

  /// \brief Point cloud ROS publisher.
  public: ros::Publisher pc_pub_;

  /// \brief Current simulation time.
  public: std::chrono::steady_clock::duration current_time_;

  /// \brief Frame ID to put in message header. Defaults to sensor scoped name.
  public: std::string frame_id_;

  /// \brief Render engine name
  public: std::string engine_name_;

  /// \brief Render scene name
  public: std::string scene_name_;
};

//////////////////////////////////////////////////
PointCloud::PointCloud() : dataPtr(std::make_unique<PointCloudPrivate>())
{
}

//////////////////////////////////////////////////
void PointCloud::Configure(const ignition::gazebo::Entity &_entity,
    const std::shared_ptr<const sdf::Element> &_sdf,
    ignition::gazebo::EntityComponentManager &_ecm,
    ignition::gazebo::EventManager &)
{
  this->dataPtr->entity_ = _entity;

  // Initialize ROS
  if (!ros::isInitialized())
  {
    int argc = 0;
    char** argv = NULL;
    ros::init(argc, argv, "ignition", ros::init_options::NoSigintHandler);
    ROS_INFO_NAMED("ros1_ign_point_cloud", "Initialized ROS");
  }

  // Sensor scoped name
  auto scoped_name = ignition::gazebo::scopedName(this->dataPtr->entity_, _ecm, "/", false);

  // ROS node
  auto ns = _sdf->Get<std::string>("namespace", scoped_name).first;
  this->dataPtr->rosnode_ = std::make_unique<ros::NodeHandle>(ns);

  // Publisher
  auto topic = _sdf->Get<std::string>("topic", "points").first;
  this->dataPtr->pc_pub_ = this->dataPtr->rosnode_->advertise<sensor_msgs::PointCloud2>(topic, 1);

  // TF frame ID
  this->dataPtr->frame_id_ = _sdf->Get<std::string>("frame_id", scoped_name).first;

  // Rendering engine and scene
  this->dataPtr->engine_name_ = _sdf->Get<std::string>("engine", "ogre2").first;
  this->dataPtr->scene_name_ = _sdf->Get<std::string>("scene", "scene").first;
}

//////////////////////////////////////////////////
void PointCloud::PostUpdate(const ignition::gazebo::UpdateInfo &_info,
    const ignition::gazebo::EntityComponentManager &_ecm)
{
  this->dataPtr->current_time_ = _info.simTime;

  // Find engine / scene
  if (!this->dataPtr->scene_)
  {
    auto engine = ignition::rendering::engine(this->dataPtr->engine_name_);
    if (!engine)
      return;

    this->dataPtr->scene_ = engine->SceneByName(this->dataPtr->scene_name_);
    if (!this->dataPtr->scene_)
      return;
  }

  // Get rendering cameras
  if (!this->dataPtr->depth_camera_)
  {
    this->dataPtr->LoadDepthCamera(_ecm);
  }
  if (!this->dataPtr->rgb_camera_)
  {
    this->dataPtr->LoadRgbCamera(_ecm);
  }
}

//////////////////////////////////////////////////
void PointCloudPrivate::LoadDepthCamera(
    const ignition::gazebo::EntityComponentManager &_ecm)
{
  // Sensor name scoped from the model
  auto sensor_name =
      ignition::gazebo::scopedName(this->entity_, _ecm, "::", false);
  sensor_name = sensor_name.substr(sensor_name.find("::") + 2) + "_depth";

  // Get sensor
  auto sensor = this->scene_->SensorByName(sensor_name);
  if (!sensor)
  {
    return;
  }

  this->depth_camera_ =
    std::dynamic_pointer_cast<ignition::rendering::DepthCamera>(sensor);
  if (!this->depth_camera_)
  {
    ROS_ERROR_NAMED("ros1_ign_point_cloud",
        "Rendering sensor named [%s] is not a depth camera", sensor_name.c_str());
    return;
  }

  this->depth_connection_ = this->depth_camera_->ConnectNewDepthFrame(
      std::bind(&PointCloudPrivate::OnNewDepthFrame, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
      std::placeholders::_4, std::placeholders::_5));
}

//////////////////////////////////////////////////
void PointCloudPrivate::LoadRgbCamera(
    const ignition::gazebo::EntityComponentManager &_ecm)
{
  // Sensor name scoped from the model
  auto sensor_name =
      ignition::gazebo::scopedName(this->entity_, _ecm, "::", false);
  sensor_name = sensor_name.substr(sensor_name.find("::") + 2);

  // Get sensor
  auto sensor = this->scene_->SensorByName(sensor_name);
  if (!sensor)
  {
    return;
  }

  this->rgb_camera_ = std::dynamic_pointer_cast<ignition::rendering::Camera>(sensor);
  if (!this->rgb_camera_)
  {
    ROS_ERROR_NAMED("ros1_ign_point_cloud",
        "Rendering sensor named [%s] is not an RGB camera", sensor_name.c_str());
    return;
  }

  this->rgb_image_ = this->rgb_camera_->CreateImage();
}

//////////////////////////////////////////////////
void PointCloudPrivate::OnNewDepthFrame(const float *_scan,
                    unsigned int _width, unsigned int _height,
                    unsigned int _channels,
                    const std::string &_format)
{
  if (this->pc_pub_.getNumSubscribers() <= 0 || _height == 0 || _width == 0)
    return;

  // Just sanity checks, but don't prevent publishing
  if (_channels != 1)
  {
    ROS_WARN_NAMED("ros1_ign_point_cloud",
        "Expected depth image to have 1 channel, but it has [%i]", _channels);
  }
  if (_format != "FLOAT32")
  {
    ROS_WARN_NAMED("ros1_ign_point_cloud",
        "Expected depth image to have [FLOAT32] format, but it has [%s]", _format.c_str());
  }

  // Fill message
  // Logic borrowed from
  // https://github.com/ros-simulation/gazebo_ros_pkgs/blob/kinetic-devel/gazebo_plugins/src/gazebo_ros_depth_camera.cpp
  auto sec_nsec = ignition::math::durationToSecNsec(this->current_time_);

  sensor_msgs::PointCloud2 msg;
  msg.header.frame_id = this->frame_id_;
  msg.header.stamp.sec = sec_nsec.first;
  msg.header.stamp.nsec = sec_nsec.second;
  msg.width = _width;
  msg.height = _height;
  msg.row_step = msg.point_step * _width;
  msg.is_dense = true;

  sensor_msgs::PointCloud2Modifier modifier(msg);
  modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");
  modifier.resize(_width*_height);

  sensor_msgs::PointCloud2Iterator<float> iter_x(msg, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(msg, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(msg, "z");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_r(msg, "r");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_g(msg, "g");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_b(msg, "b");

  if (this->rgb_camera_)
  {
    this->rgb_camera_->Capture(this->rgb_image_);
    fillImage(this->rgb_image_msg_, sensor_msgs::image_encodings::RGB8, _height,
        _width, 3 * _width, this->rgb_image_.Data<unsigned char>());
  }

  double hfov = this->depth_camera_->HFOV().Radian();
  double fl = _width / (2.0 * tan(hfov / 2.0));
  int index{0};
  uint8_t * image_src = (uint8_t *)(&(this->rgb_image_msg_.data[0]));

  // Convert depth to point cloud
  for (uint32_t j = 0; j < _height; ++j)
  {
    double p_angle;
    if (_height>1)
      p_angle = atan2( (double)j - 0.5*(double)(_height-1), fl);
    else
      p_angle = 0.0;

    for (uint32_t i=0; i<_width; i++, ++iter_x, ++iter_y, ++iter_z, ++iter_r, ++iter_g, ++iter_b)
    {
      double y_angle;
      if (_width>1)
        y_angle = atan2( (double)i - 0.5*(double)(_width-1), fl);
      else
        y_angle = 0.0;

      double depth = _scan[index++];

      // in optical frame
      // hardcoded rotation rpy(-M_PI/2, 0, -M_PI/2) is built-in
      // to urdf, where the *_optical_frame should have above relative
      // rotation from the physical camera *_frame
      *iter_x = depth * tan(y_angle);
      *iter_y = depth * tan(p_angle);
      if (depth > this->depth_camera_->FarClipPlane())
      {
        *iter_z = ignition::math::INF_D;
        msg.is_dense = false;
      }
      if (depth < this->depth_camera_->NearClipPlane())
      {
        *iter_z = -ignition::math::INF_D;
        msg.is_dense = false;
      }
      else
      {
        *iter_z = depth;
      }

      // Put image color data for each point
      // \TODO(anyone) RGB image seems offset from depth image by 3~4 pixels in both directions
      if (this->rgb_image_msg_.data.size() == _height * _width * 3)
      {
        // color
        *iter_r = image_src[i*3+j*_width*3+0];
        *iter_g = image_src[i*3+j*_width*3+1];
        *iter_b = image_src[i*3+j*_width*3+2];
      }
      else if (this->rgb_image_msg_.data.size() == _height*_width)
      {
        // mono?
        *iter_r = image_src[i+j*_width];
        *iter_g = image_src[i+j*_width];
        *iter_b = image_src[i+j*_width];
      }
      else
      {
        // no image
        *iter_r = 0;
        *iter_g = 0;
        *iter_b = 0;
      }
    }
  }

  this->pc_pub_.publish(msg);
}

