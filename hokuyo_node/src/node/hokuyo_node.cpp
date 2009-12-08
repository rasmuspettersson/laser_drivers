/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/**

@mainpage hokuyo_node

@htmlinclude manifest.html

@b hokuyo_node is a driver for SCIP2.0 compliant Hokuyo laser range-finders.
This driver has been designed, primarliy with the Hokuyo UTM-30LX in mind, also
known as the Hokuyo Top-URG. The driver has been extended to support some SCIP1.0 compliant 
range-finders such as the URG-04LX.

<hr>

@section information Information

Hokuyo scans are taken in a counter-clockwise direction.  Angles are measured
counter clockwise with 0 pointing directly forward.

<hr>

@section usage Usage
@verbatim
$ hokuyo_node [standard ROS args]
@endverbatim

@par Example

@verbatim
$ hokuyo_node
@endverbatim

<hr>

@section topic ROS topics

Subscribes to (name/type):
- None

Publishes to (name / type):
- @b "scan"/<a href="../../sensor_msgs/html/classstd__msgs_1_1LaserScan.html">sensor_msgs/LaserScan</a> : scan data from the laser.
- @b "/diagnostics"/<a href="../../robot_msgs/html/classrobot__msgs_1_1DiagnosticMessage.html">robot_msgs/DiagnosticMessage</a> : diagnostic status information.

<hr>

@section services
 - @b "~self_test"    :  SelfTest service provided by SelfTest helper class

@section parameters ROS parameters

Reads the following parameters from the parameter server

- @b "~min_ang"         : @b [double] the angle of the first range measurement in radians (Default: -pi/2)
- @b "~max_ang"         : @b [double] the angle of the last range measurement in radians (Default: pi/2)
- @b "~intensity"       : @b [bool]   whether or not the hokuyo returns intensity values (Default: true)
- @b "~cluster"         : @b [int]    the number of adjascent range measurements to cluster into a single reading (Default: 1)
- @b "~skip"            : @b [int]    the number of scans to skip between each measured scan (Default: 1)
- @b "~port"            : @b [string] the port where the hokuyo device can be found (Default: "/dev/ttyACM0")
- @b "~autostart"       : @b [bool]   whether the node should automatically start the hokuyo (Default: true)
- @b "~calibrate_time"  : @b [bool]   whether the node should calibrate the hokuyo's time offset (Default: true)
- @b "~hokuyoLaserModel04LX" : @b [bool]	whether the laser is a hokuyo mode 04LX by setting boolean LaserIsHokuyoModel04LX (Default: false)
- @b "~frame_id"        : @b [string] the frame in which laser scans will be returned (Default: "laser")
- @b "~reconfigure"    : @b [bool] set to true to force the node to reread its configuration, the node will reset it to false when it is reconfigured (Default: false)
 **/

#include "driver_base/driver.h"
#include "driver_base/driver_node.h"
#include <diagnostic_updater/publisher.h>

#include <assert.h>
#include <math.h>
#include <iostream>
#include <sstream>
#include <iomanip>

#include "ros/ros.h"

#include "sensor_msgs/LaserScan.h"

#include "hokuyo_node/HokuyoConfig.h"

#include "hokuyo.h"

using namespace std;

class HokuyoDriver : public driver_base::Driver
{
  friend class HokuyoNode;

  typedef boost::function<void(const hokuyo::LaserScan &)> UseScanFunction;
  UseScanFunction useScan_;

  boost::shared_ptr<boost::thread> scan_thread_;

  std::string device_status_;
  std::string device_id_;
  std::string connect_fail_;
  
  hokuyo::LaserScan  scan_;
  hokuyo::Laser laser_;

  bool calibrated_;
  int lost_scan_thread_count_;
  int corrupted_scan_count_;

public:
  hokuyo_node::HokuyoConfig config_;
  typedef hokuyo_node::HokuyoConfig Config;

  HokuyoDriver()
  {
    calibrated_ = false;
    lost_scan_thread_count_ = 0;
    corrupted_scan_count_ = 0;
  }

  void doOpen()
  {
    try
    {
      device_id_ = "unknown";
      device_status_ =  "unknown";
      
      laser_.open(config_.port.c_str(), config_.model_04LX);
      
      device_id_ = getID();
      device_status_ = laser_.getStatus();
      
      ROS_INFO("Connected to device with ID: %s", device_id_.c_str());

      if (config_.calibrate_time && !calibrated_)
      {
        laser_.laserOn();

       // first parameter false when 04LX laser used because 04LX sensor only accepts MD commands, not ME commands
        ROS_INFO("Starting calibration");
        laser_.calcLatency(!config_.model_04LX && config_.intensity, config_.min_ang, config_.max_ang, config_.cluster, config_.skip);
        calibrated_ = true; // This is a slow step that we only want to do once.
        ROS_INFO("Calibration finished");
      }

      state_ = OPENED;
      // first parameter false when 04LX laser used because 04LX sensor only accepts MD commands, not ME commands
    } 
    catch (hokuyo::Exception& e) 
    {
      ROS_WARN("Exception thrown while opening Hokuyo.\n%s", e.what());
      connect_fail_ = e.what();
      doClose();
      return;
    }
  }

  void doClose()
  {
    try
    {
      laser_.close();
    } catch (hokuyo::Exception& e) {
      ROS_WARN("Exception thrown while trying to close:\n%s",e.what());
    }
    
    state_ = CLOSED; // If we can't close, we are done for anyways.
  }

  void doStart()
  {
    try
    {
      laser_.laserOn();
      
      int status = laser_.requestScans(!config_.model_04LX && config_.intensity, config_.min_ang, config_.max_ang, config_.cluster, config_.skip);

      if (status != 0) {
        ROS_WARN("Failed to request scans from device.  Status: %d.", status);
        corrupted_scan_count_++;
        return;
      }
    
      state_ = RUNNING;
      scan_thread_.reset(new boost::thread(boost::bind(&HokuyoDriver::scanThread, this)));
    } 
    catch (hokuyo::Exception& e) 
    {
      ROS_WARN("Exception thrown while starting Hokuyo.\n%s", e.what());
      connect_fail_ = e.what();
      doClose();
      return;
    }
  }

  void doStop()
  {
    if (state_ != RUNNING) // RUNNING can exit asynchronously.
      return;

    state_ = OPENED;

    if (scan_thread_ && !scan_thread_->timed_join((boost::posix_time::milliseconds) 2000))
    {
      ROS_ERROR("scan_thread_ did not die after two seconds. Pretending that it did. This is probably a bad sign.");
      lost_scan_thread_count_++;
    }
    scan_thread_.reset();
  }

  virtual std::string getID()
  {
    std::string id = laser_.getID();
    if (id == std::string("H0000000"))
      return "unknown";
    return id;
  }

  void config_update(Config &new_config, int level = 0)
  {
    config_ = new_config;
  }

  void scanThread()
  {
    while (state_ == RUNNING)
    {
      try
      {
        int status = laser_.serviceScan(scan_);

        if(status != 0)
        {
          ROS_WARN("Error getting scan: %d", status);
          break;
        }
      } catch (hokuyo::CorruptedDataException &e) {
        ROS_WARN("Skipping corrupted data");
        continue;
      } catch (hokuyo::Exception& e) {
        ROS_WARN("Exception thrown while trying to get scan.\n%s", e.what());
        doClose();
        return;
      }

      useScan_(scan_);
    }

    laser_.stopScanning(); // This actually just calls laser Off internally.
    state_ = OPENED;
  }
};

class HokuyoNode : public driver_base::DriverNode<HokuyoDriver>
{
private:   
  string connect_fail_;
  
  double desired_freq_;

  ros::NodeHandle node_handle_;
  diagnostic_updater::DiagnosedPublisher<sensor_msgs::LaserScan> scan_pub_;
  sensor_msgs::LaserScan scan_msg_;
  hokuyo::LaserConfig laser_config_;

public:
  HokuyoNode(ros::NodeHandle &nh) :
    driver_base::DriverNode<HokuyoDriver>(nh),
    node_handle_(nh),
    scan_pub_(node_handle_.advertise<sensor_msgs::LaserScan>("scan", 100),
        diagnostic_,
        diagnostic_updater::FrequencyStatusParam(&desired_freq_, &desired_freq_, 0.05),
        diagnostic_updater::TimeStampStatusParam())
  {
    desired_freq_ = 0;
    driver_.useScan_ = boost::bind(&HokuyoNode::publishScan, this, _1);
    driver_.setPostOpenHook(boost::bind(&HokuyoNode::postOpenHook, this));
  }

  void postOpenHook()
  {
    driver_.laser_.getConfig(laser_config_);
     
    private_node_handle_.setParam("min_ang_limit", (double) (laser_config_.min_angle));
    private_node_handle_.setParam("max_ang_limit", (double) (laser_config_.max_angle));
    private_node_handle_.setParam("min_range", (double) (laser_config_.min_range));
    private_node_handle_.setParam("max_range", (double) (laser_config_.max_range));
  }

  virtual void addOpenedTests()
  {
    self_test_.add( "Status Test", this, &HokuyoNode::statusTest );
    self_test_.add( "Laser Test", this, &HokuyoNode::laserTest );
    self_test_.add( "Polled Data Test", this, &HokuyoNode::polledDataTest );
    self_test_.add( "Streamed Data Test", this, &HokuyoNode::streamedDataTest );
    self_test_.add( "Streamed Intensity Data Test", this, &HokuyoNode::streamedIntensityDataTest );
    self_test_.add( "Laser Off Test", this, &HokuyoNode::laserOffTest );
  }

  virtual void addStoppedTests()
  { 
  }

  virtual void addRunningTests()
  { 
  }

  virtual void addDiagnostics()
  {
    diagnostic_.add("Connection Status", this, &HokuyoNode::connectionStatus );
  }
  
  void reconfigureHook(int level)
  {
    if (private_node_handle_.hasParam("frameid"))
    {
      ROS_WARN("~frameid is deprecated, please use ~frame_id instead");
      private_node_handle_.getParam("frameid", driver_.config_.frame_id);
    }

    if (private_node_handle_.hasParam("min_ang_degrees"))
    {
      ROS_WARN("~min_ang_degrees is deprecated, please use ~min_ang instead");
      private_node_handle_.getParam("min_ang_degrees", driver_.config_.min_ang);
      driver_.config_.min_ang *= M_PI/180;
    }

    if (private_node_handle_.hasParam("max_ang_degrees"))
    {
      ROS_WARN("~max_ang_degrees is deprecated, please use ~max_ang instead");
      private_node_handle_.getParam("max_ang_degrees", driver_.config_.max_ang);
      driver_.config_.max_ang *= M_PI/180;
    }

    if (private_node_handle_.hasParam("hokuyoLaserModel04LX"))
    {
      ROS_WARN("~hokuyoLaserModel04LX is deprecated, please use ~model_04LX instead");
      bool tmp = driver_.config_.model_04LX;
      private_node_handle_.getParam("hokuyoLaserModel04LX", tmp);
      driver_.config_.model_04LX = tmp;
    }
      
    diagnostic_.force_update();   
  
    scan_pub_.clear_window(); // Reduce glitches in the frequency diagnostic.
  }

  int publishScan(const hokuyo::LaserScan &scan)
  {
    //ROS_DEBUG("publishScan");

    scan_msg_.angle_min = scan.config.min_angle;
    scan_msg_.angle_max = scan.config.max_angle;
    scan_msg_.angle_increment = scan.config.ang_increment;
    scan_msg_.time_increment = scan.config.time_increment;
    scan_msg_.scan_time = scan.config.scan_time;
    scan_msg_.range_min = scan.config.min_range;
    scan_msg_.range_max = scan.config.max_range;
    scan_msg_.ranges = scan.ranges;
    scan_msg_.intensities = scan.intensities;
    scan_msg_.header.stamp = ros::Time().fromNSec((uint64_t)scan.system_time_stamp);
    scan_msg_.header.frame_id = driver_.config_.frame_id;
  
    desired_freq_ = (1. / scan.config.scan_time);

    scan_pub_.publish(scan_msg_);

    //ROS_DEBUG("publishScan done");

    return(0);
  }

  void connectionStatus(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    if (driver_.state_ == driver_.CLOSED)
      status.summary(2, "Not connected. " + connect_fail_);
    else if (driver_.device_status_ != std::string("Sensor works well."))
      status.summary(2, "Sensor not operational");
    else if (driver_.state_ == driver_.RUNNING)
      status.summary(0, "Sensor streaming.");
    else if (driver_.state_ == driver_.OPENED)
      status.summary(0, "Sensor open.");
    else 
      status.summary(2, "Unknown sensor state.");

    status.add("Port", driver_.config_.port);
    status.add("Device ID", driver_.device_id_);
    status.add("Device Status", driver_.device_status_);
    status.add("Scan Thread Lost Count", driver_.lost_scan_thread_count_);
    status.add("Corrupted Scan Count", driver_.corrupted_scan_count_);
  }

  void statusTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    std::string stat = driver_.laser_.getStatus();

    if (stat != std::string("Sensor works well."))
    {
      status.level = 2;
    } else {
      status.level = 0;
    }

    status.message = stat;
  }

  void laserTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    driver_.laser_.laserOn();

    status.level = 0;
    status.message = "Laser turned on successfully.";
  }

  void polledDataTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    hokuyo::LaserScan  scan;

    int res = driver_.laser_.pollScan(scan, laser_config_.min_angle, laser_config_.max_angle, 1, 1000);

    if (res != 0)
    {
      status.level = 2;
      ostringstream oss;
      oss << "Hokuyo error code: " << res << ". Consult manual for meaning.";
      status.message = oss.str();

    } else {
      status.level = 0;
      status.message = "Polled Hokuyo for data successfully.";
    }
  }

  void streamedDataTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    hokuyo::LaserScan  scan;

    int res = driver_.laser_.requestScans(false, laser_config_.min_angle, laser_config_.max_angle, 1, 1, 99, 1000);

    if (res != 0)
    {
      status.level = 2;
      ostringstream oss;
      oss << "Hokuyo error code: " << res << ". Consult manual for meaning.";
      status.message = oss.str();

    } else {

      for (int i = 0; i < 99; i++)
      {
        driver_.laser_.serviceScan(scan, 1000);
      }

      status.level = 0;
      status.message = "Streamed data from Hokuyo successfully.";

    }
  }

  void streamedIntensityDataTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    hokuyo::LaserScan  scan;

    int res = driver_.laser_.requestScans(false, laser_config_.min_angle, laser_config_.max_angle, 1, 1, 99, 1000);

    if (res != 0)
    {
      status.level = 2;
      ostringstream oss;
      oss << "Hokuyo error code: " << res << ". Consult manual for meaning.";
      status.message = oss.str();

    } else {

      int corrupted_data = 0;

      for (int i = 0; i < 99; i++)
      {
        try {
          driver_.laser_.serviceScan(scan, 1000);
        } catch (hokuyo::CorruptedDataException &e) {
          corrupted_data++;
        }
      }
      if (corrupted_data == 1)
      {
        status.level = 1;
        status.message = "Single corrupted message.  This is acceptable and unavoidable";
      } else if (corrupted_data > 1)
      {
        status.level = 2;
        ostringstream oss;
        oss << corrupted_data << " corrupted messages.";
        status.message = oss.str();
      } else
      {
        status.level = 0;
        status.message = "Stramed data with intensity from Hokuyo successfully.";
      }
    }
  }

  void laserOffTest(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    driver_.laser_.laserOff();

    status.level = 0;
    status.message = "Laser turned off successfully.";
  }
};

int main(int argc, char **argv)
{ 
  return driver_base::main<HokuyoNode>(argc, argv, "hokuyo_node");
}

