/*
Copyright 2016 Krzysztof Lis

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "AugmentedUnreality.h"
#include "AURDriverOpenCV.h"
#include "AURSmoothingFilter.h"

#include <sstream>
#include <utility> // swap

#define _USE_MATH_DEFINES
#include <math.h>

UAURDriverOpenCV::UAURDriverOpenCV()
	: CameraIndex(0)
	, bNewFrameReady(false)
	, bNewOrientationReady(false)
{
}

void UAURDriverOpenCV::Initialize()
{
	Super::Initialize();

	this->Status = EAURDriverStatus::DS_Disconnected;
	this->bNewFrameReady = false;
	this->bNewOrientationReady = false;

	this->LoadCalibration();
	this->Tracker.SetSettings(this->TrackerSettings);

	this->WorkerFrame = &this->FrameInstances[0];
	this->AvailableFrame = &this->FrameInstances[1];
	this->PublishedFrame = &this->FrameInstances[2];

	this->InitializeWorker();
}

void UAURDriverOpenCV::StoreNewOrientation(FTransform const & measurement)
{
	// Mutex of orientation vars
	FScopeLock(&this->OrientationLock);

	Super::StoreNewOrientation(measurement);
	this->bNewOrientationReady = true;
}

void UAURDriverOpenCV::LoadCalibration()
{
	FString calib_file_path = this->GetCalibrationFileFullPath();
	if (this->CameraProperties.LoadFromFile(calib_file_path))
	{
		UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Calibration loaded from %s"), *calib_file_path)
	}
	else
	{
		UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Failed to load calibration from %s"), *calib_file_path)
	}

	calib_file_path = this->GetCalibrationFallbackFileFullPath();
	if (this->CameraProperties.LoadFromFile(calib_file_path))
	{
		UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Fallback calibration loaded from %s"), *calib_file_path)
	}
	else
	{
		UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Failed to load fallback calibration from %s"), *calib_file_path)
	}

	this->CameraProperties.PrintToLog();

	this->Tracker.SetCameraProperties(this->CameraProperties);

	this->CameraFov = this->CameraProperties.FOV.X;
	this->CameraAspectRatio = this->Resolution.X / this->Resolution.Y;
}

void UAURDriverOpenCV::InitializeWorker()
{
	this->Worker.Reset(new FWorkerRunnable(this));
	this->WorkerThread.Reset(FRunnableThread::Create(this->Worker.Get(), TEXT("a"), 0, TPri_Normal));
}

void UAURDriverOpenCV::Shutdown()
{
	if (this->Worker.IsValid())
	{
		this->Worker->Stop();

		if (this->WorkerThread.IsValid())
		{
			this->WorkerThread->WaitForCompletion();
		}

		// Destroy
		this->WorkerThread.Reset(nullptr);
		this->Worker.Reset(nullptr);
	}
}

FAURVideoFrame* UAURDriverOpenCV::GetFrame()
{
	// If there is a new frame produced
	if(this->bNewFrameReady)
	{
		// Manipulating frame pointers
		FScopeLock lock(&this->FrameLock);

		// Put the new ready frame in PublishedFrame
		std::swap(this->AvailableFrame, this->PublishedFrame);

		this->bNewFrameReady = false;
	}
	// if there is no new frame, return the old one again

	return this->PublishedFrame;
}

bool UAURDriverOpenCV::IsNewFrameAvailable() const
{
	return this->bNewFrameReady;
}

bool UAURDriverOpenCV::IsNewOrientationAvailable() const
{
	return this->bNewOrientationReady;
}

FTransform UAURDriverOpenCV::GetOrientation()
{
	// Accessing orientation
	FScopeLock lock(&this->OrientationLock);

	this->bNewOrientationReady = false;

	return this->CurrentOrientation;
}

FString UAURDriverOpenCV::GetDiagnosticText() const
{
	return this->DiagnosticText;
}

UAURDriverOpenCV::FWorkerRunnable::FWorkerRunnable(UAURDriverOpenCV * driver)
	: Driver(driver)
{
	FIntPoint res = driver->Resolution;
	CapturedFrame = cv::Mat(res.X, res.Y, CV_8UC3, cv::Scalar(0, 0, 255));
}

bool UAURDriverOpenCV::FWorkerRunnable::Init()
{
	this->bContinue = true;
	UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Worker init"))
	return true;
}

uint32 UAURDriverOpenCV::FWorkerRunnable::Run()
{
	// Start the video capture

	UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Trying to open camera with index %d"), this->Driver->CameraIndex);

	this->VideoCapture = cv::VideoCapture(this->Driver->CameraIndex);
	if (VideoCapture.isOpened())
	{
		// Use the resolution specified
		VideoCapture.set(CV_CAP_PROP_FRAME_WIDTH, this->Driver->Resolution.X);
		VideoCapture.set(CV_CAP_PROP_FRAME_HEIGHT, this->Driver->Resolution.Y);

		// Find the resolution used by the camera
		this->Driver->Resolution.X = VideoCapture.get(CV_CAP_PROP_FRAME_WIDTH);
		this->Driver->Resolution.Y = VideoCapture.get(CV_CAP_PROP_FRAME_HEIGHT);
		this->Driver->CameraAspectRatio = (float)this->Driver->Resolution.X / (float)this->Driver->Resolution.Y;// / this->Driver->CameraPixelRatio;

		this->Driver->WorkerFrame->SetResolution(this->Driver->Resolution);
		this->Driver->AvailableFrame->SetResolution(this->Driver->Resolution);
		this->Driver->PublishedFrame->SetResolution(this->Driver->Resolution);

		UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Using camera resolution %d x %d"), this->Driver->Resolution.X, this->Driver->Resolution.Y)
	}
	else
	{
		UE_LOG(LogAUR, Error, TEXT("AURDriverOpenCV: Failed to open VideoCapture"))
		this->bContinue = false;
	}

	UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Worker thread start"))

		Driver->DiagnosticText = "START";

	//cv::Size calibration_board_size(11, 8);
	//cv::Size calibration_board_size(9, 6);
	cv::Size calibration_board_size(4, 11); //11 rows with 4 circles each
	std::vector<cv::Point2d> calibration_image_points;
	std::vector<cv::Point2d> calibration_detected_points;

	while (this->bContinue)
	{
		// get a new frame from camera - this blocks untill the next frame is available
		VideoCapture >> CapturedFrame;

		// compare the frame size to the size we expect from capture parameters
		auto frame_size = CapturedFrame.size();
		if (frame_size.width != Driver->Resolution.X || frame_size.height != Driver->Resolution.Y) 
		{
			UE_LOG(LogAUR, Error, TEXT("AURDriverOpenCV: Camera returned a frame with unexpected size: %dx%d instead of %dx%d"),
				frame_size.width, frame_size.height, Driver->Resolution.X, Driver->Resolution.Y);
		}
		else
		{			
			/**
			 * Tracking markers and relative position with respect to them
			 */
			if (this->Driver->bPerformOrientationTracking)
			{
				FTransform camera_transform;
				bool markers_detected = Driver->Tracker.DetectMarkers(CapturedFrame, camera_transform);

				if(markers_detected)
				{
					// Report the rotation and location to the driver.
					// mutex locking performed by driver
					Driver->StoreNewOrientation(camera_transform);
				}
			}

			// ---------------------------
			// Create the frame to publish

			// Camera image is in BGR format
			BGR_Color* src_pixels = (BGR_Color*)CapturedFrame.data;

			// Frame to fill is in RGBA format
			FColor* dest_pixels = Driver->WorkerFrame->Image.GetData();

			int32 pixel_count = Driver->Resolution.X * Driver->Resolution.Y;
			for (int32 pixel_idx = 0; pixel_idx < pixel_count; pixel_idx++)
			{
				BGR_Color* src_pix = &src_pixels[pixel_idx];
				FColor* dest_pix = &dest_pixels[pixel_idx];

				dest_pix->R = src_pix->R;
				dest_pix->G = src_pix->G;
				dest_pix->B = src_pix->B;
			}

			{
				FScopeLock(&Driver->FrameLock);

				// Put the generated frame as available frame
				std::swap(Driver->WorkerFrame, Driver->AvailableFrame);
				Driver->bNewFrameReady = true;
			}
		}
	}

	// Exiting the loop means the program ends, so release camera
	this->VideoCapture.release();

	UE_LOG(LogAUR, Log, TEXT("AURDriverOpenCV: Worker thread ends"))

	return 0;
}

void UAURDriverOpenCV::FWorkerRunnable::Stop()
{
	this->bContinue = false;
}