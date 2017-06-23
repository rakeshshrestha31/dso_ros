/**
* This file is part of DSO.
* 
* Copyright 2016 Technical University of Munich and Intel.
* Developed by Jakob Engel <engelj at in dot tum dot de>,
* for more information see <http://vision.in.tum.de/dso>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* DSO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* DSO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with DSO. If not, see <http://www.gnu.org/licenses/>.
*/





#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "util/settings.h"
#include "FullSystem/FullSystem.h"
#include "util/Undistort.h"
#include "IOWrapper/Pangolin/PangolinDSOViewer.h"
#include "IOWrapper/OutputWrapper/SampleOutputWrapper.h"

#include "GroundTruthIterator/GroundTruthIterator.h"

#include "IMU/configparam.h"
#include "IMU/imudata.h"

#include "MsgSync/MsgSynchronizer.h"

#include <ros/ros.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <geometry_msgs/PoseStamped.h>
#include "cv_bridge/cv_bridge.h"

// GTSAM related includes.
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/slam/dataset.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/inference/Symbol.h>

std::string calib = "";
std::string vignetteFile = "";
std::string gammaFile = "";
std::string configFile = "";
std::string groundTruthFile = "";

bool useSampleOutput=false;

using namespace dso;

void parseArgument(char* arg)
{
	int option;
	char buf[1000];

	if(1==sscanf(arg,"sampleoutput=%d",&option))
	{
		if(option==1)
		{
			useSampleOutput = true;
			printf("USING SAMPLE OUTPUT WRAPPER!\n");
		}
		return;
	}

	if(1==sscanf(arg,"quiet=%d",&option))
	{
		if(option==1)
		{
			setting_debugout_runquiet = true;
			printf("QUIET MODE, I'll shut up!\n");
		}
		return;
	}


	if(1==sscanf(arg,"nolog=%d",&option))
	{
		if(option==1)
		{
			setting_logStuff = false;
			printf("DISABLE LOGGING!\n");
		}
		return;
	}

	if(1==sscanf(arg,"nogui=%d",&option))
	{
		if(option==1)
		{
			disableAllDisplay = true;
			printf("NO GUI!\n");
		}
		return;
	}
	if(1==sscanf(arg,"nomt=%d",&option))
	{
		if(option==1)
		{
			multiThreading = false;
			printf("NO MultiThreading!\n");
		}
		return;
	}
	if(1==sscanf(arg,"calib=%s",buf))
	{
		calib = buf;
		printf("loading calibration from %s!\n", calib.c_str());
		return;
	}
	if(1==sscanf(arg,"vignette=%s",buf))
	{
		vignetteFile = buf;
		printf("loading vignette from %s!\n", vignetteFile.c_str());
		return;
	}

	if(1==sscanf(arg,"gamma=%s",buf))
	{
		gammaFile = buf;
		printf("loading gammaCalib from %s!\n", gammaFile.c_str());
		return;
	}

	if(1==sscanf(arg,"config=%s",buf))
	{
		configFile = buf;
		printf("loading config from %s!\n", configFile.c_str());
		return;
	}

	if(1==sscanf(arg,"groundtruth=%s",buf))
	{
		groundTruthFile = buf;
		printf("loading groundTruth from %s!\n", groundTruthFile.c_str());
		return;
	}

	printf("could not parse argument \"%s\"!!\n", arg);
}




FullSystem* fullSystem = 0;
Undistort* undistorter = 0;
int frameID = 0;

void vidCb(const sensor_msgs::ImageConstPtr img)
{
	cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO8);
	assert(cv_ptr->image.type() == CV_8U);
	assert(cv_ptr->image.channels() == 1);


	if(setting_fullResetRequested)
	{
		std::vector<IOWrap::Output3DWrapper*> wraps = fullSystem->outputWrapper;
		delete fullSystem;
		for(IOWrap::Output3DWrapper* ow : wraps) ow->reset();
		fullSystem = new FullSystem();
		fullSystem->linearizeOperation=false;
		fullSystem->outputWrapper = wraps;
	    if(undistorter->photometricUndist != 0)
	    	fullSystem->setGammaFunction(undistorter->photometricUndist->getG());
		setting_fullResetRequested=false;
	}

	MinimalImageB minImg((int)cv_ptr->image.cols, (int)cv_ptr->image.rows,(unsigned char*)cv_ptr->image.data);
	ImageAndExposure* undistImg = undistorter->undistort<unsigned char>(&minImg, 1,0, 1.0f);
	fullSystem->addActiveFrame(undistImg, frameID);
	frameID++;
	delete undistImg;

}


int main( int argc, char** argv )
{
	ros::init(argc, argv, "dso_live");



	for(int i=1; i<argc;i++) parseArgument(argv[i]);

	if (configFile.empty())
	{
		printf("Config file location missing\n");
		return 1;
	}

	if (groundTruthFile.empty())
	{
		printf("Groundtruth file location missing\n");
		return 1;
	}

	setting_desiredImmatureDensity = 1000;
	setting_desiredPointDensity = 1200;
	setting_minFrames = 5;
	setting_maxFrames = 7;
	setting_maxOptIterations=4;
	setting_minOptIterations=1;
	setting_logStuff = false;
	setting_kfGlobalWeight = 1.3;


	printf("MODE WITH CALIBRATION, but without exposure times!\n");
	setting_photometricCalibration = 2;
	setting_affineOptModeA = 0;
	setting_affineOptModeB = 0;



    undistorter = Undistort::getUndistorterForFile(calib, gammaFile, vignetteFile);

    setGlobalCalib(
            (int)undistorter->getSize()[0],
            (int)undistorter->getSize()[1],
            undistorter->getK().cast<float>());


    fullSystem = new FullSystem();
    fullSystem->linearizeOperation=false;


    if(!disableAllDisplay)
	    fullSystem->outputWrapper.push_back(new IOWrap::PangolinDSOViewer(
	    		 (int)undistorter->getSize()[0],
	    		 (int)undistorter->getSize()[1]));


    if(useSampleOutput)
        fullSystem->outputWrapper.push_back(new IOWrap::SampleOutputWrapper());


    if(undistorter->photometricUndist != 0)
    	fullSystem->setGammaFunction(undistorter->photometricUndist->getG());

    ros::NodeHandle nh;

    // --------------------------------- Configs --------------------------------- //
    dso_vi::ConfigParam config(configFile);
    // 3dm imu output per g. 1g=9.80665 according to datasheet
    const double g3dm = 9.80665;
    const bool bAccMultiply98 = config.GetAccMultiply9p8();
    const double nAccMultiplier = config.GetAccMultiply9p8() ? g3dm : 1;

    dso_vi::MsgSynchronizer msgsync( config.GetImageDelayToIMU() );

    ros::Subscriber imgSub = nh.subscribe(config._imageTopic, 2, &dso_vi::MsgSynchronizer::imageCallback, &msgsync);
    ros::Subscriber imuSub = nh.subscribe(config._imuTopic, 200, &dso_vi::MsgSynchronizer::imuCallback, &msgsync);

    dso_vi::GroundTruthIterator groundtruthIterator(groundTruthFile);

    sensor_msgs::ImageConstPtr imageMsg;
    std::vector<sensor_msgs::ImuConstPtr> vimuMsg;

    while (ros::ok())
    {
    	static double nPreviousImageTimestamp = -1;
    	bool bdata = msgsync.getRecentMsgs(imageMsg, vimuMsg);
    	
    	if (bdata)
    	{
    		std::vector<dso_vi::IMUData> vimuData;
    		for (sensor_msgs::ImuConstPtr &imuMsg: vimuMsg)
    		{
    			vimuData.push_back(
    				dso_vi::IMUData(
    					imuMsg->angular_velocity.x, imuMsg->angular_velocity.y, imuMsg->angular_velocity.z,
    					imuMsg->linear_acceleration.x * nAccMultiplier,
    					imuMsg->linear_acceleration.y * nAccMultiplier,
    					imuMsg->linear_acceleration.z * nAccMultiplier,
    					imuMsg->header.stamp.toSec()
    				)
    			);
    		}
    		ROS_INFO("%ld IMU message between the images", vimuData.size());

    		if (nPreviousImageTimestamp > 0)
    		{
    			// read the groundtruth pose between the two camera poses
    			// the groundtruth timestamp are in nano seconds
    			gtsam::Pose3 relativePose = groundtruthIterator.getPoseBetween(
    				round(nPreviousImageTimestamp*1e9), 
    				round(imageMsg->header.stamp.toSec()*1e9)
    			);
    			ROS_INFO("%f - %f t: %f, %f, %f", 
    				nPreviousImageTimestamp,
    				imageMsg->header.stamp.toSec(),
    				relativePose.translation().x(), 
    				relativePose.translation().y(), 
    				relativePose.translation().z()
    			);
    		}
    		nPreviousImageTimestamp = imageMsg->header.stamp.toSec();
    	}
    	ros::spinOnce();
    }

    for(IOWrap::Output3DWrapper* ow : fullSystem->outputWrapper)
    {
        ow->join();
        delete ow;
    }

    delete undistorter;
    delete fullSystem;

	return 0;
}

