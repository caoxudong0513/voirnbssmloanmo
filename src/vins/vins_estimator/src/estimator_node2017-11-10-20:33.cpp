#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"
#include "loop-closure/loop_closure.h"
#include "loop-closure/keyframe.h"
#include "loop-closure/keyframe_database.h"
#include "camodocal/camera_models/CameraFactory.h"
#include "camodocal/camera_models/CataCamera.h"
#include "camodocal/camera_models/PinholeCamera.h"

#include "geometry_msgs/PoseWithCovarianceStamped.h"

//#include "HeadingDetector.h"

Estimator estimator;

std::condition_variable con;
double current_time = -1;
double current_time_odom = -1;//add by hao
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<nav_msgs::OdometryConstPtr> odom_buf;//添加odom_buf add by hao
queue<geometry_msgs::PoseWithCovarianceStampedConstPtr> odom_ekf_buf;//添加odom_buf add by hao
std::mutex m_posegraph_buf;
queue<int> optimize_posegraph_buf;
queue<KeyFrame*> keyframe_buf;
queue<RetriveData> retrive_data_buf;

int sum_of_wait = 0;
bool first_KF=true;
double heading = 0;//添加累积航向 add by hao
double dt=0.1;//odom频率 add by hao

Vector3d prev_odom=Vector3d::Zero();//add by hao
Vector3d forw_odom=Vector3d::Zero();//add by hao
Quaterniond forw_odom_q=Quaterniond::Identity();//add by hao
Vector3d prev_odom_euler=Vector3d::Zero();//add by hao
Vector3d forw_odom_euler=Vector3d::Zero();//add by hao

Vector3d last_odom_P;//add by hao
Matrix3d last_odom_R;//add by hao

//nav_msgs::OdometryConstPtr prev_odom,forw_odom;//add by hao
ros::Publisher pub_virtual_imu;//add by hao
ros::Subscriber sub_odom;//add by hao

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_loop_drift;
std::mutex m_keyframedatabase_resample;
std::mutex m_update_visualization;
std::mutex m_keyframe_buf;
std::mutex m_retrive_data_buf;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;

queue<pair<cv::Mat, double>> image_buf;
LoopClosure *loop_closure;
KeyFrameDatabase keyframe_database;

int global_frame_cnt = 0;
//camera param
camodocal::CameraPtr m_camera;
vector<int> erase_index;
std_msgs::Header cur_header;
Eigen::Vector3d relocalize_t{Eigen::Vector3d(0, 0, 0)};
Eigen::Matrix3d relocalize_r{Eigen::Matrix3d::Identity()};


void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba - tmp_Q.inverse() * estimator.g);

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba - tmp_Q.inverse() * estimator.g);

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = relocalize_r * estimator.Ps[WINDOW_SIZE] + relocalize_t;
    tmp_Q = relocalize_r * estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());//预积分imu数据

}

std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        if (!(imu_buf.back()->header.stamp > feature_buf.front()->header.stamp))
        {
            ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;
        }

        if (!(imu_buf.front()->header.stamp < feature_buf.front()->header.stamp))
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();
        feature_buf.pop();

        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp <= img_msg->header.stamp)
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }

        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
    }
}

void raw_image_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    cv_bridge::CvImagePtr _img_ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);
    cv_bridge::CvImagePtr img_ptr=_img_ptr;
    //image_pool[img_msg->header.stamp.toNSec()] = img_ptr->image;

//    //if image is too dark or light, trun on equalize to find enough features. add by hao
//    if (EQUALIZE)
//    {
//        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
//        clahe->apply(_img_ptr->image, img_ptr->image);
//    }
//    else
//        img_ptr = _img_ptr;

    if(LOOP_CLOSURE)
    {
        i_buf.lock();
        image_buf.push(make_pair(img_ptr->image, img_msg->header.stamp.toSec()));
        i_buf.unlock();
    }
}

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

//添加odom_callback函数 add by hao
void odom_callback(const nav_msgs::OdometryConstPtr &odom_msg)
{
  //    m_buf.lock();
  //    odom_buf.push(odom_msg);
  //    m_buf.unlock();
  //    con.notify_one();
  double t = odom_msg->header.stamp.toSec();
  if(current_time_odom<0){
    current_time_odom=t;
    dt=1.0/20.0;
  }
  else
    dt=t-current_time_odom;
  current_time_odom=t;

  sensor_msgs::Imu imu;

  forw_odom[0]=odom_msg->pose.pose.position.x;
  forw_odom[1]=odom_msg->pose.pose.position.y;
  forw_odom[2]=odom_msg->pose.pose.position.z;
  forw_odom_q.x()=odom_msg->pose.pose.orientation.x;
  forw_odom_q.y()=odom_msg->pose.pose.orientation.y;
  forw_odom_q.z()=odom_msg->pose.pose.orientation.z;
  forw_odom_q.w()=odom_msg->pose.pose.orientation.w;
  forw_odom_euler=forw_odom_q.toRotationMatrix().eulerAngles(2,1,0);

  double ba[]{0.0, 0.0, 0.0};
  double bg[]{0.0, 0.0, 0.0};

  imu.linear_acceleration.x = 2.0*(forw_odom[0]-prev_odom[0])/dt/dt - ba[0];
  imu.linear_acceleration.y = 2.0*(forw_odom[1]-prev_odom[1])/dt/dt - ba[1];
  imu.linear_acceleration.z = 2.0*(forw_odom[2]-prev_odom[2])/dt/dt - ba[2];

  imu.angular_velocity.x = (forw_odom_euler[0]-prev_odom_euler[0])/dt - bg[0];
  imu.angular_velocity.y = (forw_odom_euler[1]-prev_odom_euler[1])/dt - bg[1];
  imu.angular_velocity.z = (forw_odom_euler[2]-prev_odom_euler[2])/dt - bg[2];
  imu.orientation_covariance.at(0)=-1.0;

  imu.header.stamp=odom_msg->header.stamp;
  pub_virtual_imu.publish(imu);
  prev_odom=forw_odom;
  prev_odom_euler=forw_odom_euler;
}

//add by hao
void virtual_imu(ros::NodeHandle &n){
  pub_virtual_imu=n.advertise<sensor_msgs::Imu>("/odom_imu",1);
  sub_odom = n.subscribe("/Odometry", 2000, odom_callback,ros::TransportHints().tcpNoDelay());//添加odom消息订阅 add by hao

}

//////odom增量替换imu预积分 add by hao
//void fuseodom(Estimator &estimator, const std_msgs::Header &header){
//    if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
////  if (estimator.frame_count == WINDOW_SIZE)
//    {
//      if(!(header.stamp>odom_buf.back()->header.stamp)){
//          double t = header.stamp.toSec();
//          if (current_time_odom < 0)
//              current_time_odom = t;
//          double dt = t - current_time_odom;
//          current_time_odom = t;
//          m_buf.lock();
//          while(odom_buf.front()->header.stamp<header.stamp){
//              prev_odom=odom_buf.front();
//              odom_buf.pop();
//          }
//          m_buf.unlock();
//          if(!(odom_buf.front()->header.stamp<header.stamp)){
//              forw_odom=odom_buf.front();
//          }
//          Vector3d odom_t;
//          Quaterniond prev_q, forw_q, odom_q;
//          Vector3d delta_t=Vector3d::Zero();
//          Matrix3d delta_r=Matrix3d::Identity();
//          Vector3d delta_euler=Vector3d::Zero();
//          double k=(header.stamp.toSec()-prev_odom->header.stamp.toSec())/(forw_odom->header.stamp.toSec()-prev_odom->header.stamp.toSec());
//          odom_t(0)=k*(forw_odom->pose.pose.position.x-prev_odom->pose.pose.position.x)+prev_odom->pose.pose.position.x;
//          odom_t(1)=k*(forw_odom->pose.pose.position.y-prev_odom->pose.pose.position.y)+prev_odom->pose.pose.position.y;
//          odom_t(2)=k*(forw_odom->pose.pose.position.z-prev_odom->pose.pose.position.z)+prev_odom->pose.pose.position.z;
//          prev_q.x()=prev_odom->pose.pose.orientation.x;
//          prev_q.y()=prev_odom->pose.pose.orientation.y;
//          prev_q.z()=prev_odom->pose.pose.orientation.z;
//          prev_q.w()=prev_odom->pose.pose.orientation.w;
//          forw_q.x()=forw_odom->pose.pose.orientation.x;
//          forw_q.y()=forw_odom->pose.pose.orientation.y;
//          forw_q.z()=forw_odom->pose.pose.orientation.z;
//          forw_q.w()=forw_odom->pose.pose.orientation.w;
//          odom_q=prev_q.slerp(k,forw_q);//四元数插值

//          if(first_KF){
//              first_KF=false;
//          }
//          else{
//              delta_t=odom_t-last_odom_P;
//              delta_r=last_odom_R.inverse()*odom_q.toRotationMatrix();
//          }

//          delta_euler=delta_r.eulerAngles(2,1,0);

//          double ba[]{0.0, 0.0, 0.0};
//          double bg[]{0.0, 0.0, 0.0};

//          double dx = 2.0*delta_t[0]/dt/dt - ba[0];
//          double dy = 2.0*delta_t[1]/dt/dt - ba[1];
//          double dz = 2.0*delta_t[2]/dt/dt - ba[2];

//          double rx = delta_euler[0]/dt - bg[0];
//          double ry = delta_euler[1]/dt - bg[1];
//          double rz = delta_euler[2]/dt - bg[2];

//          estimator.acc_0 = Vector3d(dx,dy,dz);
//          estimator.gyr_0 = Vector3d(rx,ry,rz);

//          estimator.Ps[WINDOW_SIZE]=estimator.Ps[WINDOW_SIZE-1]+delta_t;
//          estimator.Rs[WINDOW_SIZE]=estimator.Rs[WINDOW_SIZE-1]*delta_r;
//          estimator.Vs[WINDOW_SIZE]=estimator.acc_0*dt;
//          last_odom_P=odom_t;
//          last_odom_R=odom_q.toRotationMatrix();
//      }
//   }
//}

void send_imu(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (current_time < 0)
        current_time = t;
    double dt = t - current_time;
    current_time = t;

    double ba[]{0.0, 0.0, 0.0};
    double bg[]{0.0, 0.0, 0.0};

    double dx = imu_msg->linear_acceleration.x - ba[0];
    double dy = imu_msg->linear_acceleration.y - ba[1];
    double dz = imu_msg->linear_acceleration.z - ba[2];

    double rx = imu_msg->angular_velocity.x - bg[0];
    double ry = imu_msg->angular_velocity.y - bg[1];
    double rz = imu_msg->angular_velocity.z - bg[2];
    //ROS_DEBUG("IMU %f, dt: %f, acc: %f %f %f, gyr: %f %f %f", t, dt, dx, dy, dz, rx, ry, rz);

    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
}

//thread:loop detection
void process_loop_detection()
{
    if(loop_closure == NULL)
    {
        const char *voc_file = VOC_FILE.c_str();
        TicToc t_load_voc;
        ROS_DEBUG("loop start loop");
        cout << "voc file: " << voc_file << endl;
        loop_closure = new LoopClosure(voc_file, IMAGE_COL, IMAGE_ROW);
        ROS_DEBUG("loop load vocbulary %lf", t_load_voc.toc());
        loop_closure->initCameraModel(CAM_NAMES);
    }

    while(LOOP_CLOSURE)
    {
        KeyFrame* cur_kf = NULL;
        m_keyframe_buf.lock();
        while(!keyframe_buf.empty())
        {
            if(cur_kf!=NULL)
                delete cur_kf;
            cur_kf = keyframe_buf.front();
            keyframe_buf.pop();
        }
        m_keyframe_buf.unlock();
        if (cur_kf != NULL)
        {
            cur_kf->global_index = global_frame_cnt;
            m_keyframedatabase_resample.lock();
            keyframe_database.add(cur_kf);
            m_keyframedatabase_resample.unlock();

            cv::Mat current_image;
            current_image = cur_kf->image;

            bool loop_succ = false;
            int old_index = -1;
            vector<cv::Point2f> cur_pts;
            vector<cv::Point2f> old_pts;
            TicToc t_brief;
            cur_kf->extractBrief(current_image);//add keypoints;keypoints.size()==descriptors.size();
            cout<<"cur_kf keypoints size:"<<cur_kf->keypoints.size()<<endl;
            //printf("loop extract %d feature using %lf\n", cur_kf->keypoints.size(), t_brief.toc());
            TicToc t_loopdetect;
            loop_succ = loop_closure->startLoopClosure(cur_kf->keypoints, cur_kf->descriptors, cur_pts, old_pts, old_index);
            double t_loop = t_loopdetect.toc();
            ROS_DEBUG("t_loopdetect %f ms", t_loop);
            if(loop_succ)
            {
                KeyFrame* old_kf = keyframe_database.getKeyframe(old_index);
                if (old_kf == NULL)
                {
                    ROS_WARN("NO such frame in keyframe_database");
                    ROS_BREAK();
                }
                ROS_DEBUG("loop succ %d with %drd image", global_frame_cnt, old_index);
                assert(old_index!=-1);

                Vector3d T_w_i_old, PnP_T_old;
                Matrix3d R_w_i_old, PnP_R_old;

                old_kf->getPose(T_w_i_old, R_w_i_old);
                std::vector<cv::Point2f> measurements_old;
                std::vector<cv::Point2f> measurements_old_norm;
                std::vector<cv::Point2f> measurements_cur;
                std::vector<int> features_id_matched;
                cur_kf->findConnectionWithOldFrame(old_kf, measurements_old, measurements_old_norm, PnP_T_old, PnP_R_old, m_camera);
                measurements_cur = cur_kf->measurements_matched;
                features_id_matched = cur_kf->features_id_matched;
                // send loop info to VINS relocalization
                int loop_fusion = 0;
                if( (int)measurements_old_norm.size() > MIN_LOOP_NUM && global_frame_cnt - old_index > 35 && old_index > 30)
                {

                    Quaterniond PnP_Q_old(PnP_R_old);
                    RetriveData retrive_data;
                    retrive_data.cur_index = cur_kf->global_index;
                    retrive_data.header = cur_kf->header;
                    retrive_data.P_old = T_w_i_old;
                    retrive_data.R_old = R_w_i_old;
                    retrive_data.relative_pose = false;
                    retrive_data.relocalized = false;
                    retrive_data.measurements = measurements_old_norm;
                    retrive_data.features_ids = features_id_matched;
                    retrive_data.loop_pose[0] = PnP_T_old.x();
                    retrive_data.loop_pose[1] = PnP_T_old.y();
                    retrive_data.loop_pose[2] = PnP_T_old.z();
                    retrive_data.loop_pose[3] = PnP_Q_old.x();
                    retrive_data.loop_pose[4] = PnP_Q_old.y();
                    retrive_data.loop_pose[5] = PnP_Q_old.z();
                    retrive_data.loop_pose[6] = PnP_Q_old.w();
                    m_retrive_data_buf.lock();
                    retrive_data_buf.push(retrive_data);
                    m_retrive_data_buf.unlock();
                    cur_kf->detectLoop(old_index);
                    old_kf->is_looped = 1;
                    loop_fusion = 1;

                    m_update_visualization.lock();
                    keyframe_database.addLoop(old_index);
                    CameraPoseVisualization* posegraph_visualization = keyframe_database.getPosegraphVisualization();
                    pubPoseGraph(posegraph_visualization, cur_header);
                    m_update_visualization.unlock();
                }


                // visualization loop info
                if(0 && loop_fusion)
                {
                    int COL = current_image.cols;
                    //int ROW = current_image.rows;
                    cv::Mat gray_img, loop_match_img;
                    cv::Mat old_img = old_kf->image;
                    cv::hconcat(old_img, current_image, gray_img);
                    cvtColor(gray_img, loop_match_img, CV_GRAY2RGB);
                    cv::Mat loop_match_img2;
                    loop_match_img2 = loop_match_img.clone();
                    /*
                    for(int i = 0; i< (int)cur_pts.size(); i++)
                    {
                        cv::Point2f cur_pt = cur_pts[i];
                        cur_pt.x += COL;
                        cv::circle(loop_match_img, cur_pt, 5, cv::Scalar(0, 255, 0));
                    }
                    for(int i = 0; i< (int)old_pts.size(); i++)
                    {
                        cv::circle(loop_match_img, old_pts[i], 5, cv::Scalar(0, 255, 0));
                    }
                    for (int i = 0; i< (int)old_pts.size(); i++)
                    {
                        cv::Point2f cur_pt = cur_pts[i];
                        cur_pt.x += COL ;
                        cv::line(loop_match_img, old_pts[i], cur_pt, cv::Scalar(0, 255, 0), 1, 8, 0);
                    }
                    ostringstream convert;
                    convert << "/home/tony-ws/raw_data/loop_image/"
                            << cur_kf->global_index << "-"
                            << old_index << "-" << loop_fusion <<".jpg";
                    cv::imwrite( convert.str().c_str(), loop_match_img);
                    */
                    for(int i = 0; i< (int)measurements_cur.size(); i++)
                    {
                        cv::Point2f cur_pt = measurements_cur[i];
                        cur_pt.x += COL;
                        cv::circle(loop_match_img2, cur_pt, 5, cv::Scalar(0, 255, 0));
                    }
                    for(int i = 0; i< (int)measurements_old.size(); i++)
                    {
                        cv::circle(loop_match_img2, measurements_old[i], 5, cv::Scalar(0, 255, 0));
                    }
                    for (int i = 0; i< (int)measurements_old.size(); i++)
                    {
                        cv::Point2f cur_pt = measurements_cur[i];
                        cur_pt.x += COL ;
                        cv::line(loop_match_img2, measurements_old[i], cur_pt, cv::Scalar(0, 255, 0), 1, 8, 0);
                    }

                    ostringstream convert2;
                    convert2 << "/home/tony-ws/raw_data/loop_image/"
                            << cur_kf->global_index << "-"
                            << old_index << "-" << loop_fusion <<"-2.jpg";
                    cv::imwrite( convert2.str().c_str(), loop_match_img2);
                }

            }
            //release memory
            cur_kf->image.release();
            global_frame_cnt++;

            if (t_loop > 1000 || keyframe_database.size() > MAX_KEYFRAME_NUM)
            {
                m_keyframedatabase_resample.lock();
                erase_index.clear();
                keyframe_database.downsample(erase_index);
                m_keyframedatabase_resample.unlock();
                if(!erase_index.empty())
                    loop_closure->eraseIndex(erase_index);
            }
        }
        std::chrono::milliseconds dura(10);
        std::this_thread::sleep_for(dura);
    }
}

//thread: pose_graph optimization
void process_pose_graph()
{
    while(true)
    {
        m_posegraph_buf.lock();
        int index = -1;
        while (!optimize_posegraph_buf.empty())
        {
            index = optimize_posegraph_buf.front();
            optimize_posegraph_buf.pop();
        }
        m_posegraph_buf.unlock();
        if(index != -1)
        {
            Vector3d correct_t = Vector3d::Zero();
            Matrix3d correct_r = Matrix3d::Identity();
            TicToc t_posegraph;
            keyframe_database.optimize4DoFLoopPoseGraph(index,
                                                    correct_t,
                                                    correct_r);
            ROS_DEBUG("t_posegraph %f ms", t_posegraph.toc());
            m_loop_drift.lock();
            relocalize_r = correct_r;
            relocalize_t = correct_t;
            m_loop_drift.unlock();
            m_update_visualization.lock();
            keyframe_database.updateVisualization();
            CameraPoseVisualization* posegraph_visualization = keyframe_database.getPosegraphVisualization();
            m_update_visualization.unlock();
            pubOdometry(estimator, cur_header, relocalize_t, relocalize_r);
            pubPoseGraph(posegraph_visualization, cur_header);
            nav_msgs::Path refine_path = keyframe_database.getPath();
            updateLoopPath(refine_path);
        }

        std::chrono::milliseconds dura(5000);
        std::this_thread::sleep_for(dura);
    }
}

// thread: visual-inertial odometry
void process()
{
    while (true)
    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {
            return (measurements = getMeasurements()).size() != 0;//可以参考结合imu将odom与img组合
                 });
        lk.unlock();

        for (auto &measurement : measurements)
        {
            for (auto &imu_msg : measurement.first)
                send_imu(imu_msg);

            auto img_msg = measurement.second;
            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;
            map<int, vector<pair<int, Vector3d>>> image;
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                ROS_ASSERT(z == 1);
                image[feature_id].emplace_back(camera_id, Vector3d(x, y, z));
            }

//            fuseodom(estimator, img_msg->header);//odom增量替换imu预积分 add by hao

            estimator.processImage(image, img_msg->header);

//            //use line to detect heading. add by hao.
//            cout<<"Heading detector!"<<endl;
//            cv::Mat im;
//            auto tmp_image_buf=image_buf;
//            i_buf.lock();
//            while(!tmp_image_buf.empty() && tmp_image_buf.front().second < img_msg->header.stamp.toSec())
//            {
//                tmp_image_buf.pop();
//            }
//            i_buf.unlock();
//            im=tmp_image_buf.front().first;
//            double heading=HeadingDetector(im);
//            cout<<"Heading is : "<<heading<<endl;

            /**
            *** start build keyframe database for loop closure
            **/
            if(LOOP_CLOSURE)
            {
                // remove previous loop
                vector<RetriveData>::iterator it = estimator.retrive_data_vector.begin();
                for(; it != estimator.retrive_data_vector.end(); )
                {
                    if ((*it).header < estimator.Headers[0].stamp.toSec())
                    {
                        it = estimator.retrive_data_vector.erase(it);
                    }
                    else
                        it++;
                }
                m_retrive_data_buf.lock();
                while(!retrive_data_buf.empty())
                {
                    RetriveData tmp_retrive_data = retrive_data_buf.front();
                    retrive_data_buf.pop();
                    estimator.retrive_data_vector.push_back(tmp_retrive_data);
                }
                m_retrive_data_buf.unlock();
                //WINDOW_SIZE - 2 is key frame
                if(estimator.marginalization_flag == 0 && estimator.solver_flag == estimator.NON_LINEAR)
                {
                    Vector3d vio_T_w_i = estimator.Ps[WINDOW_SIZE - 2];//当前最新关键帧位姿信息
                    Matrix3d vio_R_w_i = estimator.Rs[WINDOW_SIZE - 2];
                    i_buf.lock();
                    while(!image_buf.empty() && image_buf.front().second < estimator.Headers[WINDOW_SIZE - 2].stamp.toSec())
                    {
                        image_buf.pop();
                    }
                    i_buf.unlock();
                    //assert(estimator.Headers[WINDOW_SIZE - 1].stamp.toSec() == image_buf.front().second);
                    // relative_T   i-1_T_i relative_R  i-1_R_i
                    cv::Mat KeyFrame_image;
                    KeyFrame_image = image_buf.front().first;

                    const char *pattern_file = PATTERN_FILE.c_str();
                    Vector3d cur_T;//闭环更新后当前最新关键帧位姿信息
                    Matrix3d cur_R;
                    cur_T = relocalize_r * vio_T_w_i + relocalize_t;
                    cur_R = relocalize_r * vio_R_w_i;
                    KeyFrame* keyframe = new KeyFrame(estimator.Headers[WINDOW_SIZE - 2].stamp.toSec(), vio_T_w_i, vio_R_w_i, cur_T, cur_R, image_buf.front().first, pattern_file);
                    keyframe->setExtrinsic(estimator.tic[0], estimator.ric[0]);
                    keyframe->buildKeyFrameFeatures(estimator, m_camera);
                    m_keyframe_buf.lock();
                    keyframe_buf.push(keyframe);
                    m_keyframe_buf.unlock();
                    // update loop info
                    if (!estimator.retrive_data_vector.empty() && estimator.retrive_data_vector[0].relative_pose)
                    {
                        if(estimator.Headers[0].stamp.toSec() == estimator.retrive_data_vector[0].header)
                        {
                            KeyFrame* cur_kf = keyframe_database.getKeyframe(estimator.retrive_data_vector[0].cur_index);
                            if (abs(estimator.retrive_data_vector[0].relative_yaw) > 30.0 || estimator.retrive_data_vector[0].relative_t.norm() > 20.0)
                            {
                                ROS_DEBUG("Wrong loop");
                                cur_kf->removeLoop();
                            }
                            else
                            {
                                cur_kf->updateLoopConnection( estimator.retrive_data_vector[0].relative_t,
                                                              estimator.retrive_data_vector[0].relative_q,
                                                              estimator.retrive_data_vector[0].relative_yaw);
                                m_posegraph_buf.lock();
                                optimize_posegraph_buf.push(estimator.retrive_data_vector[0].cur_index);
                                m_posegraph_buf.unlock();
                            }
                        }
                    }
                }
            }
            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            cur_header = header;
            m_loop_drift.lock();
            if (estimator.relocalize)
            {
                relocalize_t = estimator.relocalize_t;
                relocalize_r = estimator.relocalize_r;
            }

//            //添加时间戳,结合odom判断VINS是否漂移,若发生漂移，启动初始化标志位，后续信息均无法发布 add by hao
//            adjustodom(estimator, header, relocalize_t, relocalize_r);

            pubOdometry(estimator, header, relocalize_t, relocalize_r);
            pubKeyPoses(estimator, header, relocalize_t, relocalize_r);
            pubCameraPose(estimator, header, relocalize_t, relocalize_r);
            pubPointCloud(estimator, header, relocalize_t, relocalize_r);
            pubTF(estimator, header, relocalize_t, relocalize_r);
            m_loop_drift.unlock();
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_buf.lock();
        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);
    estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    registerPub(n);

    virtual_imu(n);

    ros::Subscriber sub_imu = n.subscribe("/odom_imu", 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_raw_image = n.subscribe(IMAGE_TOPIC, 2000, raw_image_callback);
//    ros::Subscriber sub_odom_ekf = n.subscribe("/robot_pose_ekf/odom_combined", 2000, odom_ekf_callback,ros::TransportHints().tcpNoDelay());//添加odom消息订阅 add by hao

    std::thread measurement_process{process};
    std::thread loop_detection, pose_graph;
    if (LOOP_CLOSURE)
    {
        ROS_WARN("LOOP_CLOSURE true");
        loop_detection = std::thread(process_loop_detection);
        pose_graph = std::thread(process_pose_graph);
        m_camera = CameraFactory::instance()->generateCameraFromYamlFile(CAM_NAMES);
    }
    ros::spin();

    return 0;
}