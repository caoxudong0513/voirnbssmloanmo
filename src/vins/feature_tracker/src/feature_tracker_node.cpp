#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/Imu.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>

#include "feature_tracker.h"

#define SHOW_UNDISTORTION 0

vector<uchar> r_status;
vector<float> r_err;
queue<sensor_msgs::ImageConstPtr> img_buf;

ros::Publisher pub_img,pub_match;

FeatureTracker trackerData[NUM_OF_CAM];
double first_image_time;
int pub_count = 1;
bool first_image_flag = true;

/**
 * @brief ROS的图像回调函数，对新来的图像进行特征点追踪，发布
 *
 * 使用createCLAHE对图像进行自适应直方图均衡化
 * calcOpticalFlowPyrLK() LK金字塔光流法，生成tracking的特征点
 * undistroted特征点
 * 然后把追踪的特征点发布到名字为pub_img的话题下，图像发布在在pub_match下
 * 被追踪的特征点是有全局唯一的ID的，后面就方便做匹配了
*/
void img_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    if(first_image_flag)
    {
        first_image_flag = false;
        first_image_time = img_msg->header.stamp.toSec();
    }

//    根据配置文件中的freq，确定每隔多久的时候，把检测到的特征点打包成/feature_tracker/featuretopic 发出去，
//    要是没有达到发送的时间，这幅图像的feature就作为下一时刻的
//    KLT追踪的特征点，就是不是每一副图像都要处理的，那样计算时间大了，而且数据感觉冗余，帧与帧之间图像的差距不会那么明显。
    // frequency control   频率控制，freq最小为10
    //! Step1：控制图像输入频率，这个地方是一个平均值
    if (round(1.0 * pub_count / (img_msg->header.stamp.toSec() - first_image_time)) <= FREQ)
    {
        PUB_THIS_FRAME = true;//发布特征点
        // reset the frequency control  确定是否处理当前图片

        if (abs(1.0 * pub_count / (img_msg->header.stamp.toSec() - first_image_time) - FREQ) < 0.01 * FREQ)
        {
            first_image_time = img_msg->header.stamp.toSec();
            pub_count = 0;
        }
    }
    else
        PUB_THIS_FRAME = false;//只做特征跟踪但不发布
     //! Step2:读入图像，并进行KLT跟踪
    //cv_bridge::CvImageConstPtr ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);//cao20180705
    cv_bridge::CvImageConstPtr ptr;
//       if (img_msg->encoding == "8UC1")
//      {
            sensor_msgs::Image img;
            img.header = img_msg->header;
            img.height = img_msg->height;
            img.width = img_msg->width;
            img.is_bigendian = img_msg->is_bigendian;
            img.step = img_msg->step;
            img.data = img_msg->data;
            img.encoding = "mono8";
            ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO8);
//        }
//        else
//            ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);
    cv::Mat show_img = ptr->image;
    TicToc t_r;
    for (int i = 0; i < NUM_OF_CAM; i++)  //读取图像
    {
        ROS_DEBUG("processing camera %d", i);
        //! 针对单目相机读入图像，进入KLT跟踪阶段
        if (i != 1 || !STEREO_TRACK)
//11.6cadd  STEREO_TRACK置1，相机０和相机１是双目,相机０调用FeatureTracker进行跟踪，相机１通过LK光流跟踪相机０中的特征点，处理逻辑和前后帧跟踪逻辑一致，只是不再提取新的特征点。
            trackerData[i].readImage(ptr->image.rowRange(ROW * i, ROW * (i + 1)));
        //! 针对双目相机
        else
        {
          //双目
          //! 是否补偿
            if (EQUALIZE)
            {
                cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
                clahe->apply(ptr->image.rowRange(ROW * i, ROW * (i + 1)), trackerData[i].cur_img);
            }
            else
                trackerData[i].cur_img = ptr->image.rowRange(ROW * i, ROW * (i + 1));
        }

#if SHOW_UNDISTORTION   
        trackerData[i].showUndistortion("undistrotion_" + std::to_string(i));    //去畸变
#endif
    }

     //双目
    if ( PUB_THIS_FRAME && STEREO_TRACK && trackerData[0].cur_pts.size() > 0)
    {
        pub_count++;
        r_status.clear();
        r_err.clear();
        TicToc t_o;
        cv::calcOpticalFlowPyrLK(trackerData[0].cur_img, trackerData[1].cur_img, trackerData[0].cur_pts, trackerData[1].cur_pts, r_status, r_err, cv::Size(21, 21), 3);
        ROS_DEBUG("spatial optical flow costs: %fms", t_o.toc());
        vector<cv::Point2f> ll, rr;
        vector<int> idx;
        for (unsigned int i = 0; i < r_status.size(); i++)
        {
            if (!inBorder(trackerData[1].cur_pts[i]))
                r_status[i] = 0;

            if (r_status[i])
            {
                idx.push_back(i);

                Eigen::Vector3d tmp_p;
                trackerData[0].m_camera->liftProjective(Eigen::Vector2d(trackerData[0].cur_pts[i].x, trackerData[0].cur_pts[i].y), tmp_p);
                tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
                tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
                ll.push_back(cv::Point2f(tmp_p.x(), tmp_p.y()));

                trackerData[1].m_camera->liftProjective(Eigen::Vector2d(trackerData[1].cur_pts[i].x, trackerData[1].cur_pts[i].y), tmp_p);
                tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
                tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
                rr.push_back(cv::Point2f(tmp_p.x(), tmp_p.y()));
            }
        }
        if (ll.size() >= 8)
        {
            vector<uchar> status;
            TicToc t_f;
            cv::findFundamentalMat(ll, rr, cv::FM_RANSAC, 1.0, 0.5, status);
            ROS_DEBUG("find f cost: %f", t_f.toc());
            int r_cnt = 0;
            for (unsigned int i = 0; i < status.size(); i++)
            {
                if (status[i] == 0)
                    r_status[idx[i]] = 0;
                r_cnt += r_status[idx[i]];
            }
        }
    }

    //更新全局ID//! Step3://更新feature的ID
    for (unsigned int i = 0;; i++)   //更新ID
    {
        bool completed = false;
        for (int j = 0; j < NUM_OF_CAM; j++)
            if (j != 1 || !STEREO_TRACK)
                completed |= trackerData[j].updateID(i);
        if (!completed)
            break;
    }

     //发布当前帧，包括id和undistorted后的点，和u,v点
     //! Step4：对Features进行畸变恢复之后发布到VIO估计器，将没有经过畸变矫正的Features在图像上标出
   if (PUB_THIS_FRAME)  //发布topic  存储图片相关信息
   {
        pub_count++;
        sensor_msgs::PointCloudPtr feature_points(new sensor_msgs::PointCloud);
        //特征点的id，图像的(u,v)坐标
        sensor_msgs::ChannelFloat32 id_of_point;
        sensor_msgs::ChannelFloat32 u_of_point;
        sensor_msgs::ChannelFloat32 v_of_point;

        feature_points->header = img_msg->header;
        feature_points->header.frame_id = "world";

         //! Buyi
        vector<set<int>> hash_ids(NUM_OF_CAM);
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            if (i != 1 || !STEREO_TRACK)
            {
                auto un_pts = trackerData[i].undistortedPoints();
                auto &cur_pts = trackerData[i].cur_pts;
                auto &ids = trackerData[i].ids;
                for (unsigned int j = 0; j < ids.size(); j++)
                {
                    int p_id = ids[j];
                    hash_ids[i].insert(p_id);
                    geometry_msgs::Point32 p;
                    p.x = un_pts[j].x;
                    p.y = un_pts[j].y;
                    p.z = 1;

                    feature_points->points.push_back(p);
                    id_of_point.values.push_back(p_id * NUM_OF_CAM + i);
                    u_of_point.values.push_back(cur_pts[j].x);
                    v_of_point.values.push_back(cur_pts[j].y);
                    ROS_ASSERT(inBorder(cur_pts[j]));
                }
            }
            else if (STEREO_TRACK)
            {
               //双目
                auto r_un_pts = trackerData[1].undistortedPoints();
                auto &ids = trackerData[0].ids;
                for (unsigned int j = 0; j < ids.size(); j++)
                {
                    if (r_status[j])
                    {
                        int p_id = ids[j];
                        hash_ids[i].insert(p_id);
                        geometry_msgs::Point32 p;
                        p.x = r_un_pts[j].x;
                        p.y = r_un_pts[j].y;
                        p.z = 1;

                        feature_points->points.push_back(p);
                        id_of_point.values.push_back(p_id * NUM_OF_CAM + i);
                    }
                }
            }
        }
        feature_points->channels.push_back(id_of_point);
        feature_points->channels.push_back(u_of_point);
        feature_points->channels.push_back(v_of_point);
        ROS_DEBUG("publish %f, at %f", feature_points->header.stamp.toSec(), ros::Time::now().toSec());
        pub_img.publish(feature_points);

        //! 在当前图像上标出Features，这个地方标出的是畸变之后的图像坐标
        if (SHOW_TRACK)  //根据特征点被追踪的次数，显示他的颜色，越红表示这个特征点看到的越久
        {
            ptr = cv_bridge::cvtColor(ptr, sensor_msgs::image_encodings::BGR8);

            //cv::Mat stereo_img(ROW * NUM_OF_CAM, COL, CV_8UC3);
            cv::Mat stereo_img = ptr->image;

            for (int i = 0; i < NUM_OF_CAM; i++)
            {
              //显示追踪状态，越红越好，越蓝越不行
                cv::Mat tmp_img = stereo_img.rowRange(i * ROW, (i + 1) * ROW);
                cv::cvtColor(show_img, tmp_img, CV_GRAY2RGB);
                if (i != 1 || !STEREO_TRACK)
                {
                    for (unsigned int j = 0; j < trackerData[i].cur_pts.size(); j++)
                    {
                        double len = std::min(1.0, 1.0 * trackerData[i].track_cnt[j] / WINDOW_SIZE);
                        cv::circle(tmp_img, trackerData[i].cur_pts[j], 2, cv::Scalar(255 * (1 - len), 0, 255 * len), 2);
                        //char name[10];
                        //sprintf(name, "%d", trackerData[i].ids[j]);
                        //cv::putText(tmp_img, name, trackerData[i].cur_pts[j], cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
                    }
                }
                else
                {
                   //双目
                    for (unsigned int j = 0; j < trackerData[i].cur_pts.size(); j++)
                    {
                        if (r_status[j])
                        {
                            cv::circle(tmp_img, trackerData[i].cur_pts[j], 2, cv::Scalar(0, 255, 0), 2);
                            cv::line(stereo_img, trackerData[i - 1].cur_pts[j], trackerData[i].cur_pts[j] + cv::Point2f(0, ROW), cv::Scalar(0, 255, 0));
                        }
                    }
                }
            }
            /*
            cv::imshow("vis", stereo_img);
            cv::waitKey(5);
            */
            pub_match.publish(ptr->toImageMsg());
        }
    }
    ROS_INFO("whole feature tracker processing costs: %f", t_r.toc());
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "feature_tracker");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);

    for (int i = 0; i < NUM_OF_CAM; i++)
        trackerData[i].readIntrinsicParameter(CAM_NAMES[i]);

    //鱼眼相机的mask,追踪时候会用到
    if(FISHEYE)
    {
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            trackerData[i].fisheye_mask = cv::imread(FISHEYE_MASK, 0);
            if(!trackerData[i].fisheye_mask.data)
            {
                ROS_INFO("load mask fail");
                ROS_BREAK();
            }
            else
                ROS_INFO("load mask success");
        }
    }

    ros::Subscriber sub_img = n.subscribe(IMAGE_TOPIC, 100, img_callback);

    //在名为feature的话题下发布一条类型为PointCloud的消息
    pub_img = n.advertise<sensor_msgs::PointCloud>("feature", 1000);
    //在名为feature_img的话题下发布一条类型为Image的消息
    pub_match = n.advertise<sensor_msgs::Image>("feature_img",1000);
    /*
    if (SHOW_TRACK)
        cv::namedWindow("vis", cv::WINDOW_NORMAL);
    */
    ros::spin();
    return 0;
}
