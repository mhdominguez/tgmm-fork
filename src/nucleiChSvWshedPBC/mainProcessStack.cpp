/*
 * Copyright (C) 2011-2012 by  Fernando Amat
 * See license.txt for full license and copyright notice.
 *
 * Authors: Fernando Amat
 *
 * mainTest.cpp
 *
 *  Created on: September 17th, 2012
 *      Author: Fernando Amat
 *
 * \brief Generates a hierachical segmentation of a 3D stack and saves teh result in binary format
 *
 */


#include <string>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <time.h>
#include "watershedPersistanceAgglomeration.h"
#ifndef DO_NOT_USE_CUDA
#include "CUDAmedianFilter2D/medianFilter2D.h"
#include "CUDAmedianFilter2D/gaussianBlur2D.h"
#include "cuda_runtime_api.h"
#else
#include "MedianFilter2D/medianFilter2D.h"
#include "MedianFilter2D/gaussianBlur2D.h"
#endif
#ifdef PICTOOLS_JP2K
#include "ioFunctions.h"
#endif

#include "klb_imageIO.h"

#include "parseConfigFile.h"
#include "autoThreshold.h"

namespace mylib
{
	#include "../temporalLogicalRules/mylib/array.h"
	#include "../temporalLogicalRules/mylib/image.h"
	#include "../temporalLogicalRules/mylib/filters.h"
}


using namespace std;

void generateSegmentationMaskFromHS(string hsFilename, int tau, size_t minSvSize);
void parseImageFilePattern(string& imgRawPath, int frame);
int getKernelRadiusForSigma(float sigma);

template<class imgType>
void subtractImages(const int nDim, imgType* im1,const int* imDim,imgType* im2, float weightBlurredImageSubtract, imgType useBlurredImageForBackgroundDetection);

template<class imgType>
void z_convolve_and_subtractImages(const int nDim, imgType* im1, const int* imDim, imgType* im2, int kradius, float sigmaGaussianBlur, float weightBlurredImageSubtract);

static void writeArrayToKLB(const char* filename,mylib::Array *a);

struct Params {
    imgVoxelType backgroundThr;
    int conn3D;
    imgVoxelType minTau;
    int radiusMedianFilter;
};

static string make_hs_output_filename(const string basename, Params & params)
{
	char suffix[256] = { 0 };
	sprintf(suffix, "_seg_conn%d_rad%d", params.conn3D, params.radiusMedianFilter,(int)params.minTau,(int)params.backgroundThr);
	return basename + string(suffix) + string(".bin");
}

static string make_param_output_filename(const string basename, Params & params) {
	char suffix[256] = { 0 };
	sprintf(suffix, "_seg_conn%d_rad%d", params.conn3D, params.radiusMedianFilter,(int)params.minTau,(int)params.backgroundThr);
	return basename + string(suffix) + string(".txt");
}


#define TEST_OUTPUT_FILE
#ifdef TEST_OUTPUT_FILE
static int test_output_file(const string basename, Params & params )
{
	int ok = 1;
	vector<function<string(const string, Params&)>> gs = {
		make_hs_output_filename,
		make_param_output_filename
	};
	for (auto g : gs) {
		auto fileOutHS = g(basename, params)+string(".tmp");
		ofstream os(fileOutHS.c_str(), ios::binary | ios::out);
		if (!os.is_open())
		{
			cout << "ERROR: could not open file " << fileOutHS << " to save hierarchical segmentation" << endl;
			perror("");
			ok=0;
		}
		os << "test data";
		os.close();
		remove(fileOutHS.c_str());
	}
	if (ok)
		cout << "OPENED TEST OUTPUT FILES JUST FINE.  Removing the test files." << endl;
	return ok;
}
#endif

int main( int argc, const char** argv )
{
#ifndef DO_NOT_USE_CUDA
	int devCUDA = 0;
#endif
	cout << "ProcessStack " << GIT_TAG << " " << GIT_HASH << endl;
	time_t start, end;
	if( argc == 4)//we have a .bin file from hierarchical segmentation and we want to output a segmentation for a specific tau
	{
		//Call function with ProcessStack <hsFilename.bin> <tau> <minSupervoxelSize>

		string hsFilename(argv[1]);
		int tau = atoi(argv[2]);
		size_t minSvSize = atoi(argv[3]);

		generateSegmentationMaskFromHS(hsFilename, tau, minSvSize);

		return 0;
	}


	//parse input parameters
	string basename;//filename without extension so we can save our binary hierarchical segmentation
	int radiusMedianFilter = 0;//if radius = 1->3x3 medianFilter
	float sigmaGaussianBlur = 0;
	float weightBlurredImageSubtract = 0;
	int minTau = 0;
	int backgroundThr = 0;
	int conn3D = 0;
	float anisotropyZ = 1;//
	float useBlurredImageForBackgroundDetection = 0;

	if( argc == 3 ) //we call program wiht <configFile> <timePoint>
	{
		configOptionsTrackingGaussianMixture configOptions;
		string configFilename(argv[1]);
		int err = configOptions.parseConfigFileTrackingGaussianMixture(configFilename);
		if( err != 0 )
			return err;

		int frame = atoi( argv[2] );

		#ifndef DO_NOT_USE_CUDA
		{
			int ndevices;
			cudaGetDeviceCount(&ndevices);
			devCUDA = frame%ndevices;
			cout << "Detected " << ndevices << " CUDA devices. Using device " << devCUDA << "." << endl;
		}
		#endif


		basename = configOptions.imgFilePattern;
		parseImageFilePattern(basename, frame);

		radiusMedianFilter = configOptions.radiusMedianFilter;
		minTau = configOptions.minTau;
		backgroundThr = configOptions.backgroundThreshold;
		conn3D = configOptions.conn3D;
		anisotropyZ = configOptions.anisotropyZ;
		
		sigmaGaussianBlur = configOptions.sigmaGaussianBlur;
		weightBlurredImageSubtract = configOptions.weightBlurredImageSubtract;
		useBlurredImageForBackgroundDetection = configOptions.useBlurredImageForBackgroundDetection;

	}else if( argc == 6)
	{
		basename = string(argv[1]);//filename without extension so we can save our binary hierarchical segmentation
		radiusMedianFilter = atoi(argv[2]);//if radius = 1->3x3 medianFilter
		minTau = atoi(argv[3]);
		backgroundThr = atoi(argv[4]);
		conn3D = atoi(argv[5]);

	}else{
		cout<<"Wrong number of parameters. Call function with <imgBasename> <radiusMedianFilter> <minTau> <backgroundThr>  <conn3D>"<<endl;
		cout<<"Wrong number of parameters. Call function with <configFile> <frame>"<<endl;
		cout << "Wrong number of parameters. Call function with <hsBinFileName> <tau> <minSuperVoxelSizePx>" << endl;
		return 2;
	}

  Params params;
  params.conn3D=conn3D;
  params.radiusMedianFilter=radiusMedianFilter;
  params.minTau=minTau;
  params.backgroundThr=backgroundThr;

#ifdef TEST_OUTPUT_FILE
	if (!test_output_file(basename, params)) {
		cout << "Exiting" << endl;
		exit(1);
	}
#endif

	//================================================================================

	//read current image
	string imgFilename(basename + ".tif");
	mylib::Array* img = mylib::Read_Image((char*)(imgFilename.c_str()),0);


	mylib::Value_Type type;
	int ndims;
	mylib::Dimn_Type  *dims = NULL;
	//try to read KLB format
	if (img == NULL)
	{
		imgFilename = string((basename + ".klb").c_str());
		//check if file exists
		FILE* fcheck = fopen(imgFilename.c_str(), "rb");
		if (fcheck != NULL)
		{
			fclose(fcheck);
			klb_imageIO imgKLB(imgFilename);
			int err = imgKLB.readHeader();
			if (err > 0)
				return err;

			void* data = malloc(imgKLB.header.getImageSizeBytes());

#ifdef DO_NOT_USE_CUDA
      // Use a single thread for reading when targeting no-GPU
      // Usually, we're doing no-GPU because we're spawning 
      // ProcessStack jobs on a cluster.  There it's more
      // important to rely on process-level parallelism than
      // multithreading: we wan't to target single node machines.
      // So, we limit to a single thread here to be nice for 
      // cluster environments.
			err = imgKLB.readImageFull((char*)data, 1);  // single thread
#else
			err = imgKLB.readImageFull((char*)data, 0);  // all the threads
#endif
			if (err > 0)
				return err;
			//update parameters for mylib
			switch (imgKLB.header.dataType)//we follow the same convention as mylib
			{
			case KLB_DATA_TYPE::UINT8_TYPE:
				type = mylib::UINT8_TYPE;
				break;
			case KLB_DATA_TYPE::UINT16_TYPE:
				type = mylib::UINT16_TYPE;
				break;
			case KLB_DATA_TYPE::FLOAT32_TYPE:
				type = mylib::FLOAT32_TYPE;
				break;
			default:
				cout << "ERROR: file type stored in KLB file not supported" << endl;
				free(data);
				return 15;
			}
			ndims = 0;
			if (dims == NULL)
				dims = new mylib::Dimn_Type[KLB_DATA_DIMS];
			while (ndims < KLB_DATA_DIMS && imgKLB.header.xyzct[ndims] > 1)
			{
				dims[ndims] = imgKLB.header.xyzct[ndims];
				ndims++;
			}

			img = mylib::Make_Array_Of_Data(mylib::PLAIN_KIND, type, ndims, dims, data);
		}
	}



	if( img == NULL )
	{
#ifdef PICTOOLS_JP2K
		//try to read JP2 image
		imgFilename = string(basename + ".jp2");
		void* data = readJP2Kfile(imgFilename, type, ndims, &dims);


		if( data == NULL)
		{
			cout<<"ERROR: could not open file "<<imgFilename<<" to read input image"<<endl;
			return 5;
		}

		img = mylib::Make_Array_Of_Data(mylib::PLAIN_KIND, type, ndims, dims, data);
#else
		cout<<"ERROR: could not open file "<<imgFilename<<" to read input image"<<endl;
		return 5;
#endif
	}

  cout << "Loaded: " << imgFilename << endl;

	//hack to make the code work for uin8 without changing everything to templates
	//basically, parse the image to uint16, since the code was designed for uint16
	if( img->type == mylib::UINT8_TYPE )
	{
		img = mylib::Convert_Array_Inplace (img, img->kind, mylib::UINT16_TYPE, 16, 0);
	}

	//hack to make the code work for 2D without changing everything to templates
	//basically, add one black slice to the image (you should select conn3D = 4 or 8)
	if( img->ndims == 2 )
	{
		mylib::Dimn_Type dimsAux[dimsImage];
		for(int ii = 0; ii<img->ndims; ii++)
			dimsAux[ii] = img->dims[ii];
		for(int ii = img->ndims; ii<dimsImage; ii++)
			dimsAux[ii] = 2;

		mylib::Array *imgAux = mylib::Make_Array(img->kind, img->type, dimsImage, dimsAux);
		memset(imgAux->data,0, (imgAux->size) * sizeof(mylib::uint16) );
		memcpy(imgAux->data, img->data, img->size * sizeof(mylib::uint16) );

		mylib::Array* imgSwap = imgAux;
		img = imgAux;
		mylib::Free_Array( imgSwap);
	}

	if( img->type != mylib::UINT16_TYPE )
	{
		cout<<"ERROR: code is not ready for this types of images (change imgVoxelType and recompile)"<<endl;
		return 2;
	}

	//calculate background automatically
	if (backgroundThr < 0)
	{
		switch (backgroundThr)
		{
		case -1:
		{
				   //Triangle method on
				   imgVoxelType* imMIP = maximumIntensityProjection<imgVoxelType, dimsImage>((imgVoxelType*)(img->data), img->dims);
				   backgroundThr = (int)(bckgAutoThr_TriangleMethod<imgVoxelType, dimsImage-1>(imMIP, img->dims));
				   backgroundThr -= 20;//be conservative
				   if (backgroundThr < 0)
					   backgroundThr = 1;

				   delete[] imMIP;

				   cout << "Automatic threshold with MIP + triangle method is " << backgroundThr << endl;
		}
			break;
		default:
			cout << "ERROR: code does not recognize option " << backgroundThr << " for automatic background calculation " << endl;
			return 10;
		}
	}
	
	//calculate median filter
	time(&start);
#ifndef DO_NOT_USE_CUDA
	medianFilterCUDASliceBySlice((imgVoxelType*) (img->data), img->dims, radiusMedianFilter,devCUDA);
#else
	medianFilter2DSliceBySlice((imgVoxelType*)(img->data), img->dims, radiusMedianFilter);
#endif
	time(&end);
    //writeArrayToKLB("median.klb",img);
	cout << "Median filtering complete in " << difftime(end,start) << " secs " << endl; // mylib::SUB_OP << endl;		

	//compute Gaussian blur for more complex + and - background subtraction
	if ( sigmaGaussianBlur > 5 && weightBlurredImageSubtract > 0 )
	{
		//perform Gaussian Blur then subtract from median filtered image
		//mylib::Array* img_blurred = Convert_Image(img, img->kind, img->type, img->scale); //copy the image array
		mylib::Array* img_blurred = mylib::Copy_Array(img); //copy the image array since we will convolve in place
		int kradius = getKernelRadiusForSigma(sigmaGaussianBlur);
		
#ifndef DO_NOT_USE_CUDA
		gaussianBlurCUDASliceBySlice((imgVoxelType*) (img_blurred->data), img_blurred->dims, sigmaGaussianBlur,devCUDA);
#else
		gaussianBlur2DSliceBySlice((imgVoxelType*)(img_blurred->data), img_blurred->dims, kradius, sigmaGaussianBlur );
		
		/*DEBUG standard blur kernel printing
		mylib::Double_Vector *xy_kernel = mylib::Gaussian_Filter((double)sigmaGaussianBlur, kradius);//cout << "line B" << endl;
		double *vecPtr = (double*)xy_kernel->data;
		cout << "mylib gb kernel:";
		for ( int ii=0; ii<kradius*2+1; ii++ )
			cout << ii << ":" << vecPtr[ii] << ",";
		cout << endl;*/
		
		//old code for XY separable convolution: single-threaded, using mylib  -- very slow
		/*
		//make XY kernel and convolve
		mylib::Double_Vector *xy_kernel = mylib::Gaussian_Filter((double)sigmaGaussianBlur, kradius);//cout << "line B" << endl;
		//mylib::Scale_Array( xy_kernel, weightBlurredImageSubtract, 0 );//cout << "line C" << endl;
		mylib::Filter_Dimension(img_blurred,xy_kernel,0);//cout << "line D" << endl;
		if ( img->ndims >1 )
			mylib::Filter_Dimension(img_blurred,xy_kernel,1);*/
#endif	
		//writeArrayToKLB("gaussian_blurred_2d.klb",img_blurred); // DEBUG
		time(&start);
		cout << "Gaussian blur xy complete in " << difftime(start,end) << " secs" << endl;
		if ( img->ndims == 3 )
		{
			//change to making Z kernel and convolve
			sigmaGaussianBlur /= anisotropyZ;
			kradius = getKernelRadiusForSigma(sigmaGaussianBlur);			
			
			//for whatever reason, convolution in z is very slow single-threaded in C++; therefore, use mylib to do this and comment out the next line below
			//z_convolve_and_subtractImages( img->ndims, (imgVoxelType*)(img->data), img->dims, (imgVoxelType*)(img_blurred->data), kradius, sigmaGaussianBlur, weightBlurredImageSubtract );
			
			//old code for z convolution: single-threaded, using mylib -- very slow, but faster than single-threaded C++
			mylib::Double_Vector *z_kernel = mylib::Gaussian_Filter((double)sigmaGaussianBlur,kradius);
			
			if ( !(useBlurredImageForBackgroundDetection) ) //if using the blurred image to detect background (i.e. where pixel values are less than backgroundThreshold in the blurred image)
			{
				if ( weightBlurredImageSubtract < 0.95 || weightBlurredImageSubtract > 1.05 ) //if scaling down the blurred image before subtract, save time by scaling down z kernel and applying that to the 3rd dimension
				{
					mylib::Scale_Array( z_kernel, weightBlurredImageSubtract, 0 );
					weightBlurredImageSubtract = 1.0f; //reset so we don't have to multiply again within subtractImages
				}
				
			}
			
			mylib::Filter_Dimension(img_blurred,z_kernel,2);
			//writeArrayToKLB("gaussian_blurred_3d.klb",img_blurred); // DEBUG
			
			//perform subtraction
			subtractImages( img->ndims, (imgVoxelType*)(img->data), img->dims, (imgVoxelType*)(img_blurred->data), weightBlurredImageSubtract, (imgVoxelType)(useBlurredImageForBackgroundDetection * (float)backgroundThr) );
		}
		else if ( img->ndims == 2 ) //2D, so do weighting step there
		{
			subtractImages( img->ndims, (imgVoxelType*)(img->data), img->dims, (imgVoxelType*)(img_blurred->data), weightBlurredImageSubtract, (imgVoxelType)(useBlurredImageForBackgroundDetection * (float)backgroundThr) );
		}
		
		//writeArrayToKLB("gaussian_blur_subtracted.klb",img); //DEBUG
		mylib::Kill_Array(img_blurred);
		
		time(&end);
		cout << "Gaussian blur z (if applicable) and background subtraction complete in " << difftime(end,start) << " secs" << endl; 
	}
	
	//build hierarchical tree
	//cout<<"DEBUGGING: building hierarchical tree"<<endl;
	int64 imgDims[dimsImage];
	for(int ii = 0;ii<dimsImage; ii++)
		imgDims[ii] = img->dims[ii];
	hierarchicalSegmentation* hs =  buildHierarchicalSegmentation((imgVoxelType*) (img->data), imgDims, backgroundThr, conn3D, minTau, 1);

	//save hierarchical histogram
	//cout<<"DEBUGGING: saving hierarchical histogram"<<endl;
	string fileOutHS = make_hs_output_filename(basename, params);
	ofstream os(fileOutHS.c_str(), ios::binary | ios:: out);
	if( !os.is_open() )
	{
		cout<<"ERROR: could not open file "<< fileOutHS<<" to save hierarchical segmentation"<<endl;
		return 3;
	}
  cout<<"Writing to "<<fileOutHS<<endl;
	hs->writeToBinary( os );
	os.close();
  cout<<"Done writing." << endl;

	//save parameters
	fileOutHS = make_param_output_filename(basename, params);
	ofstream osTxt(fileOutHS.c_str());
	if( !osTxt.is_open() )
	{
		cout<<"ERROR: could not open file "<< fileOutHS<<" to save hierarchical segmentation paremeters"<<endl;
		return 3;
	}

	osTxt<<"Image basename = "<<basename<<endl;
	osTxt<<"Radius median filter = "<<radiusMedianFilter<<endl;
	osTxt<<"Min tau ="<<minTau<<endl;
	osTxt<<"Background threshold = "<<backgroundThr<<endl;
	osTxt<<"Conn3D = "<<conn3D<<endl;
	osTxt.close();


	// delete hs;  DON'T release memory!  The tree can get pretty big and we're about to quit anyway


	return 0;
}


static void writeArrayToKLB(const char* filename, mylib::Array *a) {
	uint32_t shape[KLB_DATA_DIMS] = { 1,1,1,1,1 };
    const uint32_t block[KLB_DATA_DIMS] = { 64,64,16,1,1 };
	const float pixelSize[KLB_DATA_DIMS] = { 1, 1, 1, 1, 1 };

	auto ndims = min(a->ndims, KLB_DATA_DIMS);
	for (auto i = 0; i < ndims; ++i)
		shape[i] = a->dims[i];

	klb_imageIO imgIO(filename);
	imgIO.header.setHeader(shape, KLB_DATA_TYPE::UINT16_TYPE, pixelSize, block, BZIP2, "");
	imgIO.writeImage((char*)a->data, -1);

}

//==================================================================================
void generateSegmentationMaskFromHS(string hsFilename, int tau, size_t minSvSize)
{

	//read hierarchical segmentation
	ifstream fin(hsFilename.c_str(), ios::binary | ios::in );

	if( fin.is_open() == false )
	{
		cout<<"ERROR: at generateSegmentationMaskFromHS: opening file "<<hsFilename<<endl;
		exit(4);
	}

	hierarchicalSegmentation* hs = new hierarchicalSegmentation(fin);
	fin.close();


	//generate a segmentation for the given tau
	hs->segmentationAtTau(tau);


	//generate array with segmentation mask
	mylib::Dimn_Type dimsVec[dimsImage];
	for(int ii = 0; ii <dimsImage; ii++)
		dimsVec[ii] = supervoxel::dataDims[ii];

	mylib::Array *imgL = mylib::Make_Array(mylib::PLAIN_KIND,mylib::UINT16_TYPE, dimsImage, dimsVec);
	mylib::uint16* imgLptr = (mylib::uint16*)(imgL->data);

	memset(imgLptr, 0, sizeof(mylib::uint16) * (imgL->size) );
	mylib::uint16 numLabels = 0;
	for(vector<supervoxel>::iterator iterS = hs->currentSegmentatioSupervoxel.begin(); iterS != hs->currentSegmentatioSupervoxel.end(); ++iterS)
	{
		if( iterS->PixelIdxList.size() < minSvSize )
			continue;
        if(numLabels==maxNumLabels /*65535*/)
		{
			cout<<"ERROR: at generateSegmentationMaskFromHS: more labels than permitted in imgLabelType"<<endl;
			exit(4);
		}

		numLabels++;
		for(vector<uint64>::iterator iter = iterS->PixelIdxList.begin(); iter != iterS->PixelIdxList.end(); ++iter)
		{
			imgLptr[ *iter ] = numLabels;
		}
	}

	cout<<"A total of "<<numLabels<<" labels for tau="<<tau<<endl;

	//write tiff file
	char buffer[128];
	sprintf(buffer,"_tau%d",tau);
	string suffix(buffer);
	string imgLfilename( hsFilename +  suffix + ".klb");
	writeArrayToKLB((char*)(imgLfilename.c_str()),imgL);

	//write jp2 file
#ifdef PICTOOLS_JP2K
	imgLfilename = string( hsFilename +  suffix + ".jp2");
	writeJP2Kfile(imgL, imgLfilename);
#endif
	cout<<"KLB file written to "<<imgLfilename<<endl;

	//release memory -- or don't bc the process is exiting anyway
#if 0
	delete hs;
	mylib::Free_Array(imgL);
#endif
}



//==================================================================================
//=======================================================================
void parseImageFilePattern(string& imgRawPath, int frame)
{

	size_t found=imgRawPath.find_first_of("?");
	while(found != string::npos)
	{
		int intPrecision = 0;
		while ((imgRawPath[found] == '?') && found != string::npos)
		{
			intPrecision++;
			found++;
			if( found >= imgRawPath.size() )
				break;

		}


		char bufferTM[16];
		switch( intPrecision )
		{
		case 2:
			sprintf(bufferTM,"%.2d",frame);
			break;
		case 3:
			sprintf(bufferTM,"%.3d",frame);
			break;
		case 4:
			sprintf(bufferTM,"%.4d",frame);
			break;
		case 5:
			sprintf(bufferTM,"%.5d",frame);
			break;
		case 6:
			sprintf(bufferTM,"%.6d",frame);
			break;
		}
		string itoaTM(bufferTM);

		found=imgRawPath.find_first_of("?");
		imgRawPath.replace(found, intPrecision,itoaTM);


		//find next ???
		found=imgRawPath.find_first_of("?");
	}

}

//=======================================================================
int getKernelRadiusForSigma(float sigma) {
  int size = int(ceilf(sigma * 3)); //3 sigmas plus/minus should be decent
  if (size < 3) {
    size = 3;
  }
  if (size >=  255) { //255 is max Kernel radius
    size = 255;
  }
  return size;
}


//===============================================
template<class imgType>
void subtractImages(const int nDim, imgType* im1, const int* imDim, imgType* im2, float weightBlurredImageSubtract, imgType useBlurredImageForBackgroundDetection )
{
	uint64_t sliceSize = imDim[0];
	for (int ii = 1; ii < nDim; ii++)
		sliceSize *= imDim[ii];
	
	//excessive braching and duplication of code here to optimize so that looping only takes place twice, not three times
	if ( useBlurredImageForBackgroundDetection > 0 )
	{	
		if ( weightBlurredImageSubtract < 0.95 || weightBlurredImageSubtract > 1.05 )
		{
			for ( uint64_t ii = 0; ii<sliceSize; ii++)
			{	if( im2[ii] < useBlurredImageForBackgroundDetection )
					im1[ii] = 0;
				im2[ii] *= weightBlurredImageSubtract;
			}
		}
		else
		{
			for ( uint64_t ii = 0; ii<sliceSize; ii++)
				if( im2[ii] < useBlurredImageForBackgroundDetection )
					im1[ii] = 0;
		}			
	}
	else if ( weightBlurredImageSubtract < 0.95 || weightBlurredImageSubtract > 1.05 )
	{
		for ( uint64_t ii = 0; ii<sliceSize; ii++)
			im2[ii] *= weightBlurredImageSubtract;
	}
	
	//okay, actually do subtraction here
	for ( uint64_t ii = 0; ii<sliceSize; ii++)
		if( im2[ii] >= im1[ii] )
			im1[ii] = 0;
		else
			im1[ii] -= im2[ii];
}

//declare all the possible types so template compiles properly
template void subtractImages<unsigned char>(const int nDim, unsigned char* im1, const  int* imDim, unsigned char* im2, float weightBlurredImageSubtract, unsigned char useBlurredImageForBackgroundDetection);
template void subtractImages<unsigned short int>(const int nDim, unsigned short int* im1, const  int* imDim, unsigned short int* im2, float weightBlurredImageSubtract, unsigned short int useBlurredImageForBackgroundDetection);
template void subtractImages<float>(const int nDim, float* im1, const int* imDim, float* im2, float weightBlurredImageSubtract, float useBlurredImageForBackgroundDetection);


//---------------------------------------------------
//z_convolve_and_subtractImages -- this function convolves in z on im2 and subtract from im1 in one loop, storing result in im1; however, it is much slower than using mylib
template<class imgType>
void z_convolve_and_subtractImages(const int nDim, imgType* im1, const int* imDim, imgType* im2, int kradius, float sigmaGaussianBlur, float weightBlurredImageSubtract)
{
	//fill kernel
	float *kernel = new float[kradius*2+1];
	gaussianBlurKernel(sigmaGaussianBlur, kradius*2+1, kernel);
	
	//adjust for our user-configured weighting
	if ( weightBlurredImageSubtract < 0.95 || weightBlurredImageSubtract > 1.05 )
	{
		for ( int ii=0; ii<=kradius*2; ii++ )
			kernel[ii] *= weightBlurredImageSubtract;
	}
	
	//convolve in z on im2 and subtract from im1 in one loop, storing result in im1
	double sum;
	imgType sum_imgType;
	unsigned int x,y,z, y_pos,im1_pos, im1_pos_test = 0;
	const unsigned int xy_dim = imDim[0] * imDim[1];
	int k,d;
	for ( z = 0, im1_pos=0; z<imDim[2]; z++)
	{
		for(y = 0,y_pos=0; y < imDim[1]; y++)
		{
			for(x = 0; x < imDim[0]; x++)
			{
				sum = 0;
				for (k = -kradius; k <= kradius; k++)
				{
					d = z + k;

					if ( d >= 0 && d < imDim[2] )
					{
						sum += im2[d * xy_dim + y_pos] * kernel[kradius - k];
					}
				}

				sum_imgType = (imgType)sum;
				//im1_pos =  z * imDim[0] * imDim[1] + y * imDim[0] + x;
				//
				//if ( im1_pos_test != im1_pos )
				//	cout << "DEBUG: im1_pos_test != im1_pos: " << im1_pos_test << " vs. " << im1_pos << endl;
				
				if ( sum_imgType >= im1[im1_pos] )
					im1[im1_pos] = 0;
				else
					im1[im1_pos] -= sum_imgType;
				im1_pos++;
				y_pos++;
			}
		}
		//cout << "Z: " << z << endl;
	}
	
	delete kernel;
}

//declare all the possible types so template compiles properly
template void z_convolve_and_subtractImages<unsigned char>(const int nDim, unsigned char* im1, const  int* imDim, unsigned char* im2, int kradius, float sigmaGaussianBlur, float weightBlurredImageSubtract);
template void z_convolve_and_subtractImages<unsigned short int>(const int nDim, unsigned short int* im1, const  int* imDim, unsigned short int* im2, int kradius, float sigmaGaussianBlur, float weightBlurredImageSubtract);
template void z_convolve_and_subtractImages<float>(const int nDim, float* im1, const int* imDim, float* im2, int kradius, float sigmaGaussianBlur, float weightBlurredImageSubtract);
