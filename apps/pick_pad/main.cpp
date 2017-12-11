//
// Created by yongqi on 17-12-11.
//

// System

// STL
#include <iostream>
#include <chrono>
#include <thread>
#include <condition_variable>

// 3rd Libraries
#include <pcl/io/pcd_io.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

// User Defined
#include "utils/glcloudviewer.hpp"
#include "utils/synctcpserver.hpp"
#include "utils/pointcloudutils.hpp"
#include "camera/realsensecamera.hpp"
#include "camera/virtualcamera.hpp"
#include "vision/vision.hpp"
#include "robot/robot.hpp"
#include "message.hpp"

using namespace std;
using namespace pcl;
using namespace VForce;

typedef PointXYZRGBA PointType;

// command line flags
DEFINE_bool(gui, true, "pointcloud visualization");
DEFINE_bool(tcp, false, "run tcp server");
DEFINE_bool(virtual_camera, false, "read data from file system instead of a really RGB-D camera");
DEFINE_string(virtual_camera_path, "test_data", "data path which virtual camera will read from");

// global mutex and condition_variable
mutex mu;
condition_variable tcp_cv, gui_cv;
bool g_close = false;

// Request Type between threads
enum class RequestType : int {
  NONE = 0, VIP = 111,
  IN1 = 1, OUT1 = -1,
  IN2 = 2, OUT2 = -2
};

/**
 * TCP server thread
 * @param address tcp server address
 * @param port tcp server port
 * @param request client request type
 * @param flag target flag
 * @param target target will send to client
 */
void TCPThread(string address, int port, RequestType &request, int &flag, Pose &target) {
  while (true) {
    if (g_close)
      break;
    Utils::SyncTCPServer<char, Message> tcp_server(address, static_cast<unsigned short>(port));
    while (true) {
      if (g_close)
        break;
      try {
        char recv_msg;
        tcp_server.RecvMsg(recv_msg);
        unique_lock<mutex> lk(mu);
        if (recv_msg == 'a')
          request = RequestType::IN1;
        tcp_cv.wait(lk, [&]{return request == RequestType::OUT1 || request == RequestType::VIP;});
        flag = 0x0; // current client need flag to be 0
        tcp_server.SendMsg({target, flag});
      }
      catch (std::exception& e){
        break;
      }
    }
  }
  LOG(INFO) << "TCP Thread Closed";
}

/**
 * GUI Thread visualize the vision output
 * @param width window width
 * @param height window height
 * @param name window name
 * @param cloud raw cloud from the camera
 * @param target target cloud
 * @param cMo transformation from camera to target
 */
void GUIThread(int width, int height, string name, RequestType &request,
               PointCloud<PointType>::Ptr &cloud,
               PointCloud<PointXYZ>::Ptr &target_cloud,
               Eigen::Matrix4f &cMo) {
  Utils::GLCloudViewer viewer(width, height, name);
  viewer.InitWindow();
//  vector<PointXYZ> origin_box = ();
//  vector<PointXYZ> target_box(origin_box);
  vector<PointXYZ> origin_axis{{0, 0, 0}, {0.1, 0, 0}, {0, 0.1, 0}, {0, 0, 0.1}};
  vector<PointXYZ> target_axis(origin_axis);
  while (viewer.NotStop()) {
    if (g_close) {
      break;
    }
    viewer.ProcessEvents();
    unique_lock<mutex> lk(mu);
    if (request == RequestType::VIP || viewer.ReadyToUpdate()) {
      if (viewer.ReadyToUpdate()) {
        request = RequestType::IN1;
        gui_cv.wait(lk, [&]{return request == RequestType::OUT1 || request == RequestType::VIP;});
      }
      Utils::transform_points(origin_axis, target_axis, cMo);
//      target_box = TransformPoints(cMo, origin_box);
      if (request == RequestType::VIP)
        request = RequestType::NONE;
    }
    viewer.DrawAxis(origin_axis); // camera origin
    viewer.DrawAxis(target_axis);
    viewer.ShowCloud(cloud);
//      viewer.Draw3DBox(target_box);
    viewer.ShowCloud(target_cloud, 1);
    viewer.SwapBuffer();
  }
  viewer.DestoryWindow();
  LOG(INFO) << "GUI Thread Closed";
}

/**
 * Vision Thread detect the target and calcuate the target's pose
 * @param handler_cfg vision config file
 * @param robot_cfg robot config file
 * @param tcp_request tcp request signal
 * @param flag
 * @param target_pose target pose in robot coordinate
 * @param gui_request gui request signal
 * @param cloud raw point cloud from camera
 * @param target_cloud target point cloud
 * @param cMo transfomation from camera coordinate to target
 */
void VisionThread(string camera_name, string vision_cfg, string robot_cfg,
                  RequestType &tcp_request, int &flag, Pose &target_pose,
                  RequestType &gui_request, PointCloud<PointType>::Ptr &cloud,
                  PointCloud<PointXYZ>::Ptr &target_cloud,
                  Eigen::Matrix4f &cMo) {
  shared_ptr<Camera> camera;
  if (FLAGS_virtual_camera)
    camera = shared_ptr<Camera>(new VirtualCamera(FLAGS_virtual_camera_path));
  else {
    if (camera_name == "Realsense") {
      camera = shared_ptr<Camera>(new RealSenseCamera);
//    camera->LoadCalibration(camera_param);
    }
    else {
      LOG(ERROR) << "Unrecognized Camera name(should be RealSense): " << camera_name;
    }
  }
  camera->Start();
  cv::Mat color, depth;

  Vision vision(vision_cfg);
  Robot robot(robot_cfg);

  while (true) {
    if (g_close)
      break;
    unique_lock<mutex> lk(mu);
    if (tcp_request == RequestType::IN1 || gui_request == RequestType::IN1) {
      // update frame
      camera->Update();
      camera->FetchDepth(depth);
      camera->FetchColor(color);
      camera->FetchPointCloud(cloud);
      if (cloud->empty()) {
        LOG(WARNING) << "cloud is empty";
        continue;
      }
      if (vision.Process(color, depth, cloud)) {
        cMo = vision.GetObjectTransfomation();
        target_cloud = vision.GetTransformedModel();
        robot.CalculatePose(cMo, target_pose, flag);
        if (tcp_request == RequestType::IN1) {
          tcp_request = RequestType::OUT1;
          gui_request = RequestType::VIP;
          lk.unlock();
          tcp_cv.notify_all();
          gui_cv.notify_all();
        } else if (gui_request == RequestType::IN1) {
          gui_request = RequestType::OUT1;
          lk.unlock();
          gui_cv.notify_all();
        }
      }
    }
  }
  LOG(INFO) << "Vision Thread Closed";
}


/////////////////////////////////////////////////////////////////////////////
// Main function
/////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // read conifg
  cv::FileStorage fs("../config/vision_3d.yml", cv::FileStorage::READ);
  int width, height;
  fs["gui"]["width"] >> width;
  fs["gui"]["height"] >> height;
  string server_address;
  int server_port;
  fs["tcp"]["ip"] >> server_address;
  fs["tcp"]["port"] >> server_port;
  string camera_name, vision_cfg, robot_cfg;
  fs["core"]["camera_name"] >> camera_name;
  fs["core"]["vision_cfg"] >> vision_cfg;
  fs["core"]["robot_cfg"] >> robot_cfg;

  LOG(INFO) << "begin to init..." << endl;

  // share variable between TCPThread and VisionThread
  RequestType tcp_request = RequestType::NONE;
  int flag;
  Pose target_pose;
  // share variable between GUIThread and VisionThread
  RequestType gui_request = RequestType::NONE;
  PointCloud<PointType>::Ptr cloud(new PointCloud<PointType>);
  PointCloud<PointXYZ>::Ptr target_cloud;
  Eigen::Matrix4f cMo;

  shared_ptr<thread> vision_thread(new thread(VisionThread, camera_name, vision_cfg, robot_cfg,
                                              std::ref(tcp_request), std::ref(flag), std::ref(target_pose),
                                              std::ref(gui_request), std::ref(cloud), std::ref(target_cloud),
                                              std::ref(cMo)));
  shared_ptr<thread> gui_thread, tcp_thread;
  if (FLAGS_gui)
    gui_thread = shared_ptr<thread>(new thread(GUIThread, width, height, argv[0], std::ref(gui_request),
                                               std::ref(cloud), std::ref(target_cloud), std::ref(cMo)));

  if (FLAGS_tcp)
    tcp_thread = shared_ptr<thread>(new thread(TCPThread, server_address, server_port,
                                               std::ref(tcp_request), std::ref(flag), std::ref(target_pose)));

  if (FLAGS_gui)
    gui_thread->detach();
  if (FLAGS_tcp)
    tcp_thread->detach();
  vision_thread->detach();

  char key;
  while (cin >> key) {
    if (key == 'q') {
      g_close = true;
      LOG(INFO) << "closing......";
      this_thread::sleep_for(chrono::seconds(1));
      break;
    }
  }

  return 0;
}