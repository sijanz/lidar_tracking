#include <iostream>
#include <string.h>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/video.hpp>
#include "opencv2/video/tracking.hpp"
#include <ros/ros.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include "pcl_ros/point_cloud.h"
#include <geometry_msgs/Point.h>
#include <std_msgs/Float32MultiArray.h>
#include <std_msgs/Int32MultiArray.h>

#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/geometry.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/common/centroid.h>

#include <visualization_msgs/MarkerArray.h>
#include <visualization_msgs/Marker.h>
#include <limits>
#include <utility>
#include <body_tracker_msgs/Skeleton.h>


// FIXME: get rid of globals

int g_cluster_min{50};
int g_cluster_max{3000};
double g_cluster_tolerance{0.2};

// filter initialization
int stateDim{4};// [x,y,v_x,v_y]//,w,h]
int measDim{2};// [z_x,z_y,z_w,z_h]
int ctrlDim{0};

cv::KalmanFilter kf0(stateDim, measDim, ctrlDim, CV_32F);
cv::KalmanFilter kf1(stateDim, measDim, ctrlDim, CV_32F);
cv::KalmanFilter kf2(stateDim, measDim, ctrlDim, CV_32F);

ros::Publisher markerPub;
ros::Publisher skeletonPub;

std::vector<geometry_msgs::Point> g_prev_cluster_centers;


std::vector<int> g_object_IDs;// Output of the data association using KF

bool g_first_frame = true;

// calculate euclidean distance of two points
double euclidean_distance(geometry_msgs::Point& p1, geometry_msgs::Point& p2)
{
    return sqrt((p1.x - p2.x) * (p1.x - p2.x) + (p1.y - p2.y) * (p1.y - p2.y) + (p1.z - p2.z) * (p1.z - p2.z));
}

std::pair<int, int> findIndexOfMin(std::vector<std::vector<float> > distMat)
{
    std::pair<int, int> minIndex;
    float minEl = std::numeric_limits<float>::max();
    for (int i = 0; i < distMat.size(); i++)
        for (int j = 0; j < distMat.at(0).size(); j++) {
            if (distMat[i][j] < minEl) {
                minEl = distMat[i][j];
                minIndex = std::make_pair(i, j);

            }

        }
    return minIndex;
}


void applyKalmanFiltering(const std_msgs::Float32MultiArray& ccs)
{

    // First predict, to update the internal statePre variable

    std::vector<cv::Mat> pred{kf0.predict(), kf1.predict(), kf2.predict()};

    std::vector<geometry_msgs::Point> clusterCenters;//clusterCenters

    for (auto it = ccs.data.begin(); it != ccs.data.end(); it += 3) {
        geometry_msgs::Point pt;
        pt.x = *it;
        pt.y = *(it + 1);
        pt.z = *(it + 2);

        clusterCenters.push_back(pt);

    }

    std::vector<geometry_msgs::Point> KFpredictions;
    for (auto it = pred.begin(); it != pred.end(); it++) {
        geometry_msgs::Point pt;
        pt.x = (*it).at<float>(0);
        pt.y = (*it).at<float>(1);
        pt.z = (*it).at<float>(2);

        KFpredictions.push_back(pt);

    }

    // Find the cluster that is more probable to be belonging to a given KF.
    g_object_IDs.clear();//Clear the objID vector
    g_object_IDs.resize(3);//Allocate default elements so that [i] doesnt segfault. Should be done better

    // Copy clusterCentres for modifying it and preventing multiple assignments of the same ID
    std::vector<geometry_msgs::Point> copyOfClusterCenters(clusterCenters);
    std::vector<std::vector<float> > distMat;

    for (int filterN = 0; filterN < 3; filterN++) {
        std::vector<float> distVec;
        for (int n = 0; n < 3; n++) {
            distVec.push_back(euclidean_distance(KFpredictions[filterN], copyOfClusterCenters[n]));
        }

        distMat.push_back(distVec);
    }


    for (int clusterCount = 0; clusterCount < 3; clusterCount++) {
        // 1. Find min(distMax)==> (i,j);
        std::pair<int, int> minIndex(findIndexOfMin(distMat));
        // 2. objID[i]=clusterCenters[j]; counter++
        g_object_IDs[minIndex.first] = minIndex.second;

        // 3. distMat[i,:]=10000; distMat[:,j]=10000
        distMat[minIndex.first] = std::vector<float>(3, 10000.0);// Set the row to a high number.
        for (int row = 0; row < distMat.size(); row++)//set the column to a high number
        {
            distMat[row][minIndex.second] = 10000.0;
        }
        // 4. if(counter<6) got to 1.
    }

    visualization_msgs::MarkerArray clusterMarkers;
    std::vector<body_tracker_msgs::Skeleton> skeletons;

    for (int i = 0; i < 3; i++) {

        auto skeleton{body_tracker_msgs::Skeleton{}};

        skeleton.body_id = i;

        geometry_msgs::Point clusterC(KFpredictions[i]);
        skeleton.centerOfMass.x = clusterC.x * 1000;
        skeleton.centerOfMass.y = clusterC.y * 1000;

        skeletons.emplace_back(skeleton);


        visualization_msgs::Marker m;

        m.id = i;
        m.type = visualization_msgs::Marker::CUBE;
        m.header.frame_id = "/velodyne";
        m.scale.x = 0.3;
        m.scale.y = 0.3;
        m.scale.z = 0.3;
        m.action = visualization_msgs::Marker::ADD;
        m.color.a = 1.0;
        m.color.r = i % 2 ? 1 : 0;
        m.color.g = i % 3 ? 1 : 0;
        m.color.b = i % 4 ? 1 : 0;

        m.pose.position.x = clusterC.x;
        m.pose.position.y = clusterC.y;
        m.pose.position.z = clusterC.z;

        clusterMarkers.markers.push_back(m);
    }

    for (const auto& s : skeletons)
        skeletonPub.publish(s);

    g_prev_cluster_centers = clusterCenters;

    markerPub.publish(clusterMarkers);


    std_msgs::Int32MultiArray obj_id;
    for (const auto& it : g_object_IDs)
        obj_id.data.push_back(it);
    // Publish the object IDs

    // convert clusterCenters from geometry_msgs::Point to floats
    std::vector<std::vector<float> > cc;
    for (int i = 0; i < 3; i++) {
        std::vector<float> pt;
        pt.push_back(clusterCenters[g_object_IDs[i]].x);
        pt.push_back(clusterCenters[g_object_IDs[i]].y);
        pt.push_back(clusterCenters[g_object_IDs[i]].z);

        cc.push_back(pt);
    }

    float meas0[2] = {cc[0].at(0), cc[0].at(1)};
    float meas1[2] = {cc[1].at(0), cc[1].at(1)};
    float meas2[2] = {cc[2].at(0), cc[2].at(1)};


    // The update phase 
    cv::Mat meas0Mat = cv::Mat(2, 1, CV_32F, meas0);
    cv::Mat meas1Mat = cv::Mat(2, 1, CV_32F, meas1);
    cv::Mat meas2Mat = cv::Mat(2, 1, CV_32F, meas2);

    if (!(meas0Mat.at<float>(0, 0) == 0.0f || meas0Mat.at<float>(1, 0) == 0.0f))
        cv::Mat estimated0 = kf0.correct(meas0Mat);
    if (!(meas1[0] == 0.0f || meas1[1] == 0.0f))
        cv::Mat estimated1 = kf1.correct(meas1Mat);
    if (!(meas2[0] == 0.0f || meas2[1] == 0.0f))
        cv::Mat estimated2 = kf2.correct(meas2Mat);
}

bool hasHumanSize(const pcl::PointCloud<pcl::PointXYZ>::Ptr& cluster)
{
    if (!cluster->points.empty()) {
        double min_x = cluster->points.at(0).x;
        double max_x = cluster->points.at(0).x;
        double min_y = cluster->points.at(0).y;
        double max_y = cluster->points.at(0).y;

        std::for_each(cluster->points.begin(), cluster->points.end(), [&](const pcl::PointXYZ& p) {
            if (p.x < min_x)
                min_x = p.x;
            if (p.x > max_x)
                max_x = p.x;
            if (p.y < min_y)
                min_y = p.y;
            if (p.y > max_y)
                max_y = p.y;
        });

        double area = (max_x - min_x) * (max_y - min_y);

        return area < 1.5;

    } else
        return false;
}


void cloud_cb(const sensor_msgs::PointCloud2ConstPtr& input)
{
    // If this is the first frame, initialize kalman filters for the clustered objects
    if (g_first_frame) {
        // Initialize 6 Kalman Filters; Assuming 6 max objects in the dataset.
        // Could be made generic by creating a Kalman Filter only when a new object is detected

        auto dvx{0.01};
        auto dvy{0.01};
        auto dx{1.0};
        auto dy{1.0};
        kf0.transitionMatrix = (cv::Mat_<float>(4, 4) << dx, 0, 1, 0, 0, dy, 0, 1, 0, 0, dvx, 0, 0, 0, 0, dvy);
        kf1.transitionMatrix = (cv::Mat_<float>(4, 4) << dx, 0, 1, 0, 0, dy, 0, 1, 0, 0, dvx, 0, 0, 0, 0, dvy);
        kf2.transitionMatrix = (cv::Mat_<float>(4, 4) << dx, 0, 1, 0, 0, dy, 0, 1, 0, 0, dvx, 0, 0, 0, 0, dvy);

        cv::setIdentity(kf0.measurementMatrix);
        cv::setIdentity(kf1.measurementMatrix);
        cv::setIdentity(kf2.measurementMatrix);

        // Process Noise Covariance Matrix Q
        auto sigma_p{0.01};
        auto sigma_q{0.1};
        setIdentity(kf0.processNoiseCov, cv::Scalar::all(sigma_p));
        setIdentity(kf1.processNoiseCov, cv::Scalar::all(sigma_p));
        setIdentity(kf2.processNoiseCov, cv::Scalar::all(sigma_p));

        // Meas noise cov matrix R
        cv::setIdentity(kf0.measurementNoiseCov, cv::Scalar(sigma_q));//1e-1
        cv::setIdentity(kf1.measurementNoiseCov, cv::Scalar(sigma_q));
        cv::setIdentity(kf2.measurementNoiseCov, cv::Scalar(sigma_q));

        // Process the point cloud
        pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr clustered_cloud(new pcl::PointCloud<pcl::PointXYZ>);

        /* Creating the KdTree from input point cloud*/
        pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);

        pcl::fromROSMsg(*input, *input_cloud);
        tree->setInputCloud(input_cloud);


        auto cluster_indices{std::vector<pcl::PointIndices>{}};
        auto ec{pcl::EuclideanClusterExtraction<pcl::PointXYZ>{}};
        ec.setClusterTolerance(g_cluster_tolerance);
        ec.setMinClusterSize(g_cluster_min);
        ec.setMaxClusterSize(g_cluster_max);
        ec.setSearchMethod(tree);
        ec.setInputCloud(input_cloud);

        /* Extract the clusters out of pc and save indices in cluster_indices.*/
        ec.extract(cluster_indices);


        std::vector<pcl::PointIndices>::const_iterator it;
        std::vector<int>::const_iterator pit;

        // Vector of cluster pointclouds
        auto cluster_vec{std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>{}};

        // Cluster centroids
        std::vector<pcl::PointXYZ> cluster_centroids;

        for (it = cluster_indices.begin(); it != cluster_indices.end(); ++it) {

            pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster(new pcl::PointCloud<pcl::PointXYZ>);
            float x = 0.0;
            float y = 0.0;
            int numPts = 0;
            for (pit = it->indices.begin(); pit != it->indices.end(); pit++) {

                cloud_cluster->points.push_back(input_cloud->points[*pit]);
                x += input_cloud->points[*pit].x;
                y += input_cloud->points[*pit].y;
                numPts++;
            }


            auto centroid{pcl::PointXYZ{}};
            centroid.x = x / numPts;
            centroid.y = y / numPts;
            centroid.z = 0.0;

            cluster_vec.push_back(cloud_cluster);

            //Get the centroid of the cluster
            cluster_centroids.push_back(centroid);
        }

        // Ensure at least 3 clusters exist to publish (later clusters may be empty)
        while (cluster_vec.size() < 3) {
            pcl::PointCloud<pcl::PointXYZ>::Ptr empty_cluster(new pcl::PointCloud<pcl::PointXYZ>);
            empty_cluster->points.emplace_back(pcl::PointXYZ(0, 0, 0));
            cluster_vec.push_back(empty_cluster);
        }

        while (cluster_centroids.size() < 3) {
            auto centroid{pcl::PointXYZ{}};
            centroid.x = 0.0;
            centroid.y = 0.0;
            centroid.z = 0.0;

            cluster_centroids.push_back(centroid);
        }


        // Set initial state
        kf0.statePre.at<float>(0) = cluster_centroids.at(0).x;
        kf0.statePre.at<float>(1) = cluster_centroids.at(0).y;
        kf0.statePre.at<float>(2) = 0;// initial v_x
        kf0.statePre.at<float>(3) = 0;//initial v_y

        // Set initial state
        kf1.statePre.at<float>(0) = cluster_centroids.at(1).x;
        kf1.statePre.at<float>(1) = cluster_centroids.at(1).y;
        kf1.statePre.at<float>(2) = 0;// initial v_x
        kf1.statePre.at<float>(3) = 0;//initial v_y

        // Set initial state
        kf2.statePre.at<float>(0) = cluster_centroids.at(2).x;
        kf2.statePre.at<float>(1) = cluster_centroids.at(2).y;
        kf2.statePre.at<float>(2) = 0;// initial v_x
        kf2.statePre.at<float>(3) = 0;//initial v_y

        g_first_frame = false;

        for (int i = 0; i < 6; i++) {
            auto pt{geometry_msgs::Point{}};
            pt.x = cluster_centroids.at(i).x;
            pt.y = cluster_centroids.at(i).y;
            g_prev_cluster_centers.push_back(pt);
        }

    } else {
        pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr clustered_cloud(new pcl::PointCloud<pcl::PointXYZ>);

        /* Creating the KdTree from input point cloud*/
        pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);

        pcl::fromROSMsg(*input, *input_cloud);

        tree->setInputCloud(input_cloud);

        /* Here we are creating a vector of PointIndices, which contains the actual index
         * information in a vector<int>. The indices of each detected cluster are saved here.
         * Cluster_indices is a vector containing one instance of PointIndices for each detected
         * cluster. Cluster_indices[0] contain all indices of the first cluster in input point cloud.
         */
        std::vector<pcl::PointIndices> cluster_indices;
        auto ec{pcl::EuclideanClusterExtraction<pcl::PointXYZ>{}};
        ec.setClusterTolerance(g_cluster_tolerance);
        ec.setMinClusterSize(g_cluster_min);
        ec.setMaxClusterSize(g_cluster_max);
        ec.setSearchMethod(tree);
        ec.setInputCloud(input_cloud);

        /* Extract the clusters out of pc and save indices in cluster_indices.*/
        ec.extract(cluster_indices);

        /* To separate each cluster out of the vector<PointIndices> we have to
         * iterate through cluster_indices, create a new PointCloud for each
         * entry and write all points of the current cluster in the PointCloud.
         */
        std::vector<pcl::PointIndices>::const_iterator it;
        std::vector<int>::const_iterator pit;

        // Vector of cluster pointclouds
        std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> cluster_vec;

        // Cluster centroids
        std::vector<pcl::PointXYZ> cluster_centroids;


        for (it = cluster_indices.begin(); it != cluster_indices.end(); ++it) {
            float x = 0.0;
            float y = 0.0;
            int numPts = 0;
            pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster(new pcl::PointCloud<pcl::PointXYZ>);

            for (pit = it->indices.begin(); pit != it->indices.end(); pit++) {

                cloud_cluster->points.push_back(input_cloud->points[*pit]);


                x += input_cloud->points[*pit].x;
                y += input_cloud->points[*pit].y;
                numPts++;
            }

            if (hasHumanSize(cloud_cluster)) {
                auto centroid{pcl::PointXYZ{}};
                centroid.x = x / numPts;
                centroid.y = y / numPts;
                centroid.z = 0.0;

                cluster_vec.push_back(cloud_cluster);

                //Get the centroid of the cluster
                cluster_centroids.push_back(centroid);
            }
        }

        //Ensure at least 6 clusters exist to publish (later clusters may be empty)
        while (cluster_vec.size() < 3) {
            pcl::PointCloud<pcl::PointXYZ>::Ptr empty_cluster(new pcl::PointCloud<pcl::PointXYZ>);
            empty_cluster->points.emplace_back(pcl::PointXYZ(0, 0, 0));
            cluster_vec.push_back(empty_cluster);
        }

        while (cluster_centroids.size() < 3) {
            auto centroid{pcl::PointXYZ{}};
            centroid.x = 0.0;
            centroid.y = 0.0;
            centroid.z = 0.0;

            cluster_centroids.push_back(centroid);
        }

        auto cc{std_msgs::Float32MultiArray{}};
        for (int i = 0; i < 3; i++) {
            cc.data.push_back(cluster_centroids.at(i).x);
            cc.data.push_back(cluster_centroids.at(i).y);
            cc.data.push_back(cluster_centroids.at(i).z);
        }

        applyKalmanFiltering(cc);
    }

}


int main(int argc, char** argv)
{
    ros::init(argc, argv, "lidar_tracking");
    ros::NodeHandle nh;

    ros::Subscriber sub = nh.subscribe("/velodyne_points", 1, cloud_cb);

    markerPub = nh.advertise<visualization_msgs::MarkerArray>("/markers", 1);
    skeletonPub = nh.advertise<body_tracker_msgs::Skeleton>("/body_tracker/skeleton", 10);

    ros::spin();
}
