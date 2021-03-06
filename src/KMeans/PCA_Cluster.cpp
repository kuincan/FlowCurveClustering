/*
 * @brief The class to perform PCA-based clustering and k-means clustering on the input streamlines/pathlines
 * @author Lieyu Shi
 */


#include "PCA_Cluster.h"


/*
 * @brief The covariance ratio to decide the number of PCs in the PCA clustering
 */
const float& TOR_1 = 0.999;

/*
 * @brief The default cluster
 */
const int& CLUSTER = 8;

/*
 * @brief The initialization option for k-means clustering
 */
extern int initializationOption;

/*
 * @brief The post-process after SVD, either k-means or AHC-average
 */
extern int post_processing;

/*
 * @brief An external value to judge whether it is a PBF or not
 */
extern bool isPBF;


/*
 * @brief Perform PCA-based clustering with input data
 * @details
 * 	It will first perform the PCA clustering, then perform either AHC-average or k-means clustering on the
 * 	dimensionality reduced space
 *
 * @param[in] data The matrix coordinates
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[out] group The labels of all streamlines
 * @param[out] totalNum The size of different clusters
 * @param[out] closest The closest streamline representatives of all the clusters
 * @param[out] furthest The furthest streamline representatives of the clusters
 * @param[out] tr The TimeRecorder object to record the time
 * @param[out] sil The Silhouette class for the clustering evaluation
 */
void PCA_Cluster::performPCA_Clustering(const Eigen::MatrixXf& data, 
										const int& Row, 
										const int& Column, 
										std::vector<MeanLine>& massCenter,
									    std::vector<int>& group, 
									    std::vector<int>& totalNum, 
									    std::vector<ExtractedLine>& closest,
									    std::vector<ExtractedLine>& furthest,
										TimeRecorder& tr,
										Silhouette& sil)
{
	MatrixXf cArray, SingVec;
	VectorXf meanTrajectory(Column);
	int PC_Number;

	performSVD(cArray, data, Row, Column, PC_Number, SingVec, meanTrajectory, tr);

	if(post_processing==1)
		performPC_KMeans(cArray, Row, Column, PC_Number, SingVec, meanTrajectory,
						 massCenter, CLUSTER, group, totalNum, closest, furthest, data, tr, sil);
	else if(post_processing==2)
		perform_AHC(cArray, PC_Number, SingVec, meanTrajectory,
				    massCenter, CLUSTER, group, totalNum, closest, furthest, data, tr, sil);
}


/*
 * @brief Perform the SVD for the input of matrix coordinates
 * @details
 * 	After SVD decomposition, it will select number of dimensions that can add up to 99.9% of the total variance,
 * 	which usually results in 3 or 4 dimensions.
 *
 * @param[out] cArray The reduced-dimension matrix of the coordinates
 * @param[in] data The matrix coordinates of the streamlines
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] PC_Number The number of PCs as output
 * @param[out] SingVec The singular vectors
 * @param[out] meanTrajectory The mean coordinate of the trajectory
 * @param[out] tr The TimeRecorder class object
 */
void PCA_Cluster::performSVD(MatrixXf& cArray, 
							 const Eigen::MatrixXf& data, 
							 const int& Row, 
							 const int& Column,
							 int& PC_Number, 
							 MatrixXf& SingVec, 
							 VectorXf& meanTrajectory,
							 TimeRecorder& tr)
{
	Eigen::MatrixXf temp = data;

#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Column; ++i)
	{
		meanTrajectory(i) = temp.transpose().row(i).mean();
	}
#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		temp.row(i) = temp.row(i)-meanTrajectory.transpose();
	}

	struct timeval start, end;
	gettimeofday(&start, NULL);
	/* perform SVD decomposition for temp */
	JacobiSVD<MatrixXf> svd(temp, ComputeThinU | ComputeThinV);
	//const VectorXf& singValue = svd.singularValues();
	SingVec = svd.matrixV();
	gettimeofday(&end, NULL);
	const double& delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("SVD takes: ");
	tr.timeList.push_back(to_string(delta)+"s");

	/* compute new attribute space based on principal component */
	MatrixXf coefficient = temp*SingVec;
	/*  decide first r dorminant PCs with a threshold */
	const float& varianceSummation = coefficient.squaredNorm();
	float tempSum = 0.0;
	const float& threshold = TOR_1*varianceSummation;
	
	for (int i = 0; i < Column; ++i)
	{
		tempSum+=(coefficient.transpose().row(i)).squaredNorm();
		if(tempSum>threshold)
		{
			PC_Number = i;
			break;
		}
	}

	cArray = MatrixXf(Row, PC_Number);
#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < PC_Number; ++i)
	{
		cArray.transpose().row(i) = coefficient.transpose().row(i);
	}

	std::cout << "SVD completed!" << std::endl;

	SingVec.transposeInPlace();
}


/*
 * @brief Perform the k-means clustering algorithm on the PCs
 *
 * @param[in] cArray The dimension-reduced matrix coordinates
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[in] PC_Number The number of PCs
 * @param[in] SingVec The singular vector matrix
 * @param[in] meanTrajectory The mean coordinate of the trajectory
 * @param[in] Cluster The number of clusters as input
 * @param[out] group The labels for all the streamlines
 * @param[out] totalNum The size of all the clusters
 * @param[out] closest The coordinates of the closest extracted lines
 * @param[out] furthest The coordinates of the furthest extracted lines
 * @param[in] data The matrix coordinates of all the streamlines
 * @param[out] tr The TimeRecorder class object
 * @param[out] sil The Silhouette class object
 */
void PCA_Cluster::performPC_KMeans(const MatrixXf& cArray, 
								   const int& Row, 
								   const int& Column, 
								   const int& PC_Number, 
				 				   const MatrixXf& SingVec, 
				 				   const VectorXf& meanTrajectory, 
				 				   std::vector<MeanLine>& massCenter, 
				 				   const int& Cluster, 
				 				   std::vector<int>& group, 
				 				   std::vector<int>& totalNum, 
				 				   std::vector<ExtractedLine>& closest,
				 				   std::vector<ExtractedLine>& furthest, 
				 				   const Eigen::MatrixXf& data,
								   TimeRecorder& tr,
								   Silhouette& sil)
{
	MetricPreparation object(Row, Column);
	object.preprocessing(data, Row, Column, 0);
/* perform K-means clustering */
	MatrixXf clusterCenter;

	switch(initializationOption)
	{
	case 1:
		Initialization::generateRandomPos(clusterCenter, PC_Number, cArray, Cluster);
		break;

	case 2:
		Initialization::generateFromSamples(clusterCenter, PC_Number, cArray, Cluster);
		break;

	case 3:
		Initialization::generateFarSamples(clusterCenter, PC_Number, cArray, 
										   Cluster, 0, object);
		break;
	}

	float moving=1000, tempMoving, before;
	int storage[Cluster];

	MatrixXf centerTemp;  //store provisional center coordinate

	int tag = 0;

	std::vector< std::vector<int> > neighborVec(Cluster, std::vector<int>());

	double PCA_KMeans_delta, KMeans_delta;
	struct timeval start, end;

	gettimeofday(&start, NULL);

	std::vector<int> recorder(Row);
	do
	{
		before = moving;
		/* preset cluster number recorder */
		memset(storage,0,sizeof(int)*Cluster);
		centerTemp = MatrixXf::Zero(Cluster, PC_Number);

	#pragma omp parallel for schedule(static) num_threads(8)
		for (int i = 0; i < Cluster; ++i)
		{
			neighborVec[i].clear();
		}

	#pragma omp parallel num_threads(8)
		{
		#pragma omp for nowait
			for (int i = 0; i < Row; ++i)
			{
				float dist = FLT_MAX;
				float temp;
				int clusTemp;
				for (int j = 0; j < Cluster; ++j)
				{
					temp = (cArray.row(i)-clusterCenter.row(j)).norm();
					if(temp<dist)
					{
						dist = temp;
						clusTemp = j;
					}
				}

			#pragma omp critical
				{
					storage[clusTemp]++;
					neighborVec[clusTemp].push_back(i);
					recorder[i] = clusTemp;
					centerTemp.row(clusTemp)+=cArray.row(i);
				}
			}
		}

		moving = FLT_MIN;

	#pragma omp parallel for reduction(max:moving) num_threads(8)
		for (int i = 0; i < Cluster; ++i)
		{
			if(storage[i]>0)
			{
				centerTemp.row(i)/=storage[i];
				tempMoving = (centerTemp.row(i)-clusterCenter.row(i)).norm();
				clusterCenter.row(i) = centerTemp.row(i);
				if(moving<tempMoving)
					moving = tempMoving;
			}
		}
		std::cout << "K-means iteration " << ++tag << " completed, and moving is " << moving << "!" << std::endl;
	}while(abs(moving-before)/before >= 1.0e-2 && tag < 20 && moving>0.01);

	gettimeofday(&end, NULL);
	
	float delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("k-means iteration for PC takes: ");
	tr.timeList.push_back(to_string(delta)+"s");

	std::multimap<int,int> groupMap;

	float entropy = 0.0;
	float probability;


	for (int i = 0; i < Cluster; ++i)
	{
		groupMap.insert(std::pair<int,int>(storage[i],i));
		if(storage[i]>0)
		{
			probability = float(storage[i])/float(Row);
			entropy += probability*log2f(probability);
		}
	}

	int groupNo = 0;
	int increasingOrder[Cluster];
	for (multimap<int,int>::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
	{
		if(it->first>0)
		{
			increasingOrder[it->second] = (groupNo++);
		}
	}

	/* calculate the balanced entropy */
	entropy = -entropy/log2f(groupNo);


#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		group[i] = increasingOrder[recorder[i]];
		totalNum[i] = storage[recorder[i]];
	}

	float shortest, farDist, toCenter;
	int shortestIndex = 0, fartestIndex = 0, tempIndex = 0;
	std::vector<int> neighborTemp;

	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0 && !neighborVec[i].empty())
		{
			neighborTemp = neighborVec[i];
			shortest = FLT_MAX;
			farDist = FLT_MIN;

			for (int j = 0; j < storage[i]; ++j)
			{
				tempIndex = neighborTemp[j];
				toCenter = (clusterCenter.row(i)-cArray.row(tempIndex)).norm();

				if(toCenter<shortest)
				{
					shortest = toCenter;
					shortestIndex = tempIndex;
				}
				if(toCenter>farDist)
				{
					farDist = toCenter;
					fartestIndex = tempIndex;
				}
			}
			closest.push_back(ExtractedLine(shortestIndex,increasingOrder[i]));
			furthest.push_back(ExtractedLine(fartestIndex,increasingOrder[i]));
		}
	}
	MatrixXf pcSing(PC_Number,Column);

#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < PC_Number; ++i)
	{
		pcSing.row(i) = SingVec.row(i);
	}

	MatrixXf massPos = clusterCenter*pcSing;

	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0)
		{
			massPos.row(i) += meanTrajectory.transpose();
			std::vector<float> vecTemp;
			for (int j = 0; j < Column; ++j)
			{
				vecTemp.push_back(massPos(i,j));
			}
			massCenter.push_back(MeanLine(vecTemp,increasingOrder[i]));
		}
	}

	ValidityMeasurement vm;
	vm.computeValue(cArray, group);

	tr.eventList.push_back("PCA Validity measure is: ");
	stringstream fc_ss;
	fc_ss << vm.f_c;
	tr.timeList.push_back(fc_ss.str());

	/* Silhouette effect */
	gettimeofday(&start, NULL);

	sil.computeValue(cArray,group,groupNo,isPBF);

	gettimeofday(&end, NULL);
	delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("Clustering evaluation computing takes: ");
	tr.timeList.push_back(to_string(delta)+"s");

	/* write value of the silhouette class */
	IOHandler::writeReadme(entropy, sil, "");

}


/*
 * @brief Perform k-means clustering with input data
 *
 * @param[in] data The matrix coordinates
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[out] group The labels of all streamlines
 * @param[out] totalNum The size of different clusters
 * @param[out] closest The closest streamline representatives of all the clusters
 * @param[out] furthest The furthest streamline representatives of the clusters
 * @param[in] normOption The norm option as input
 * @param[out] tr The TimeRecorder object to record the time
 * @param[out] sil The Silhouette class for the clustering evaluation
 */
void PCA_Cluster::performDirectK_Means(const Eigen::MatrixXf& data, 
									   const int& Row, 
									   const int& Column, 
									   std::vector<MeanLine>& massCenter,
									   std::vector<int>& group, 
									   std::vector<int>& totalNum, 
									   std::vector<ExtractedLine>& closest,
									   std::vector<ExtractedLine>& furthest, 
									   const int& normOption,
									   TimeRecorder& tr,
									   Silhouette& sil)
{

	performFullK_MeansByClusters(data, Row, Column, massCenter, CLUSTER, group, 
								 totalNum, closest, furthest, normOption, tr, sil);
}


/*
 * @brief Perform PCA-based clustering with input data and number of clusters as input
 * @details
 * 	It will first perform the PCA clustering, then perform either AHC-average or k-means clustering on the
 * 	dimensionality reduced space
 *
 * @param[in] data The matrix coordinates
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[out] group The labels of all streamlines
 * @param[out] totalNum The size of different clusters
 * @param[out] closest The closest streamline representatives of all the clusters
 * @param[out] furthest The furthest streamline representatives of the clusters
 * @param[in] Cluster Number of clusters as input
 * @param[out] tr The TimeRecorder object to record the time
 * @param[out] sil The Silhouette class for the clustering evaluation
 */
void PCA_Cluster::performPCA_Clustering(const Eigen::MatrixXf& data, 
										const int& Row, 
										const int& Column, 
										std::vector<MeanLine>& massCenter,
										std::vector<int>& group, 
										std::vector<int>& totalNum, 
										std::vector<ExtractedLine>& closest, 
										std::vector<ExtractedLine>& furthest, 
										const int& Cluster,
										TimeRecorder& tr,
										Silhouette& sil)
{
	MatrixXf cArray, SingVec;
	VectorXf meanTrajectory(Column);
	int PC_Number;

	performSVD(cArray, data, Row, Column, PC_Number, SingVec, meanTrajectory, tr);
	if(post_processing==1)
		performPC_KMeans(cArray, Row, Column, PC_Number, SingVec, meanTrajectory,
						 massCenter, Cluster, group, totalNum, closest, furthest, data, tr, sil);
	else if(post_processing==2)
		perform_AHC(cArray, PC_Number, SingVec, meanTrajectory,
					massCenter, Cluster, group, totalNum, closest, furthest, data, tr, sil);
}


/*
 * @brief Perform k-means clustering with input data and the number of clusters as input
 *
 * @param[in] data The matrix coordinates
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[out] group The labels of all streamlines
 * @param[out] totalNum The size of different clusters
 * @param[out] closest The closest streamline representatives of all the clusters
 * @param[out] furthest The furthest streamline representatives of the clusters
 * @param[in] Cluster The number of clusters as input
 * @param[in] normOption The norm option as input
 * @param[out] tr The TimeRecorder object to record the time
 * @param[out] sil The Silhouette class for the clustering evaluation
 */
void PCA_Cluster::performDirectK_Means(const Eigen::MatrixXf& data, 
									   const int& Row, 
									   const int& Column, 
									   std::vector<MeanLine>& massCenter,
									   std::vector<int>& group, 
									   std::vector<int>& totalNum, 
									   std::vector<ExtractedLine>& closest, 
									   std::vector<ExtractedLine>& furthest, 
									   const int& Cluster, 
									   const int& normOption,
									   TimeRecorder& tr,
									   Silhouette& sil)
{
	performFullK_MeansByClusters(data, Row, Column, massCenter, Cluster, group, 
								 totalNum, closest, furthest, normOption, tr, sil);
}


/*
 * @brief Perform the k-means directly on similarity measures
 *
 * @param[in] data The matrix coordinates of the streamlines
 * @param[in] Row The row size
 * @param[in] Column The column size
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[in] Cluster The number of clusters
 * @param[in] totalNum The size of clusters
 * @param[out] closest The closest extracted lines of the clusters
 * @param[out] furthest The furthest extracted lines of the clusters
 * @param[in] normOption The norm option
 * @param[out] tr The TimeRecorder object
 * @param[out] sil The Silhouette object
 */
void PCA_Cluster::performFullK_MeansByClusters(const Eigen::MatrixXf& data, 
											   const int& Row, 
											   const int& Column, 
											   std::vector<MeanLine>& massCenter,
											   const int& Cluster, 
											   std::vector<int>& group, 
											   std::vector<int>& totalNum, 
											   std::vector<ExtractedLine>& closest, 
											   std::vector<ExtractedLine>& furthest, 
											   const int& normOption,
											   TimeRecorder& tr,
											   Silhouette& sil)
{	
	MetricPreparation object(Row, Column);
	object.preprocessing(data, Row, Column, normOption);

	MatrixXf clusterCenter;

	switch(initializationOption)
	{
	case 1:
		Initialization::generateRandomPos(clusterCenter, Column, data, Cluster);
		break;

	case 2:
		Initialization::generateFromSamples(clusterCenter, Column, data, Cluster);
		break;

	case 3:
		Initialization::generateFarSamples(clusterCenter, Column, data, Cluster, 
										   normOption, object);
		break;
	}

	float moving=1000, tempMoving,/* dist, tempDist, */before;
	int *storage = new int[Cluster]; // used to store number inside each cluster
	MatrixXf centerTemp;
	int tag = 0;
	std::vector< std::vector<int> > neighborVec(Cluster, std::vector<int>());

/* perform K-means with different metrics */
	std::cout << "K-means start!" << std::endl;	
	struct timeval start, end;
	gettimeofday(&start, NULL);
	std::vector<int> recorder(Row); //use to record which cluster the row belongs to

	do
	{
	/* reset storage number and weighted mean inside each cluster*/
		before=moving;
		memset(storage,0,sizeof(int)*Cluster);
		centerTemp = MatrixXf::Zero(Cluster,Column);

	/* clear streamline indices for each cluster */
	#pragma omp parallel for schedule(static) num_threads(8)
		for (int i = 0; i < Cluster; ++i)
		{
			neighborVec[i].clear();
		}

	#pragma omp parallel num_threads(8)
		{
		#pragma omp for nowait
			for (int i = 0; i < Row; ++i)
			{
				int clusTemp;
				float dist = FLT_MAX;
				float tempDist;
				for (int j = 0; j < Cluster; ++j)
				{
					tempDist = getDisimilarity(clusterCenter.row(j),data,i,normOption,object);
					if(tempDist<dist)
					{
						dist = tempDist;
						clusTemp = j;
					}
				}
				recorder[i] = clusTemp;

			#pragma omp critical
				{
					storage[clusTemp]++;
					neighborVec[clusTemp].push_back(i);
					centerTemp.row(clusTemp)+=data.row(i);
				}
			}
		}
		moving = FLT_MIN;

	/* measure how much the current center moves from original center */	
	#pragma omp parallel for reduction(max:moving) num_threads(8)
		for (int i = 0; i < Cluster; ++i)
		{
			if(storage[i]>0)
			{
				centerTemp.row(i)/=storage[i];
				tempMoving = (centerTemp.row(i)-clusterCenter.row(i)).norm();
				clusterCenter.row(i) = centerTemp.row(i);
				if(moving<tempMoving)
					moving = tempMoving;
			}
		}
		std::cout << "K-means iteration " << ++tag << " completed, and moving is " << moving << "!" << std::endl;
	}while(abs(moving-before)/before >= 1.0e-2 && tag < 20 && moving > 0.01);
	
	double delta;

	std::multimap<int,int> groupMap;

	float entropy = 0.0, probability;
	int increasingOrder[Cluster];

	int nonZero = 0;
	for (int i = 0; i < Cluster; ++i)
	{
		groupMap.insert(std::pair<int,int>(storage[i],i));
		if(storage[i]>0)
		{
			probability=float(storage[i])/float(Row);
			entropy+=probability*log2f(probability);
			++nonZero;
		}
	}
	entropy = -entropy/log2f(nonZero);

	int groupNo = 0;
	for (std::multimap<int,int>::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
	{
		if(it->first>0)
		{
			increasingOrder[it->second] = (groupNo++);
		}
	}
	std::cout << "There are " << groupNo << " groups generated!" << std::endl;
	/* finish tagging for each group */

	/* record labeling information */
	// IOHandler::generateGroups(neighborVec);

	// set cluster group number and size number 
#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		group[i] = increasingOrder[recorder[i]];
		totalNum[i] = storage[recorder[i]];
	}

	float shortest, toCenter, farDist;
	int shortestIndex = 0, tempIndex = 0, furthestIndex = 0;
	std::vector<int> neighborTemp;

	/* choose cloest and furthest streamlines to centroid streamlines */
	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0)
		{

			neighborTemp = neighborVec[i];
			shortest = FLT_MAX;
			farDist = FLT_MIN;

			for (int j = 0; j < storage[i]; ++j)
			{
				// j-th internal streamlines 
				tempIndex = neighborTemp[j];
				toCenter = getDisimilarity(clusterCenter.row(i),data,tempIndex,normOption,object);

				/* update the closest index to centroid */
				if(toCenter<shortest)
				{
					shortest = toCenter;
					shortestIndex = tempIndex;
				}

				/* update the farthest index to centroid */
				if(toCenter>farDist)
				{
					farDist = toCenter;
					furthestIndex = tempIndex;
				}
			}
			closest.push_back(ExtractedLine(shortestIndex,increasingOrder[i]));
			furthest.push_back(ExtractedLine(furthestIndex,increasingOrder[i]));
			//distFile << std::endl;
		}
	}
	//distFile.close();

	std::vector<float> closeSubset;
	/* based on known cluster centroid, save them as vector for output */
	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0)
		{
			for (int j = 0; j < Column; ++j)
			{
				closeSubset.push_back(clusterCenter(i,j));
			}
			massCenter.push_back(MeanLine(closeSubset,increasingOrder[i]));
			closeSubset.clear();
		}
	}
	delete[] storage;

	//groupNo record group numbers */

	if(groupNo<=1)
		return;

	/* if the dataset is not PBF, then should record distance matrix for Gamma matrix compution */
	if(!isPBF)
	{
		deleteDistanceMatrix(data.rows());

		std::ifstream distFile(("../dataset/"+to_string(normOption)).c_str(), ios::in);
		if(distFile.fail())
		{
			distFile.close();
			getDistanceMatrix(data, normOption, object);
			std::ofstream distFileOut(("../dataset/"+to_string(normOption)).c_str(), ios::out);
			for(int i=0;i<data.rows();++i)
			{
				for(int j=0;j<data.rows();++j)
				{
					distFileOut << distanceMatrix[i][j] << " ";
				}
				distFileOut << std::endl;
			}
			distFileOut.close();
		}
		else
		{
			std::cout << "read distance matrix..." << std::endl;

			distanceMatrix = new float*[data.rows()];
		#pragma omp parallel for schedule(static) num_threads(8)
			for (int i = 0; i < data.rows(); ++i)
			{
				distanceMatrix[i] = new float[data.rows()];
			}
			int i=0, j;
			string line;
			stringstream ss;
			while(getline(distFile, line))
			{
				j=0;
				ss.str(line);
				while(ss>>line)
				{
					if(i==j)
						distanceMatrix[i][j]=0;
					else
						distanceMatrix[i][j] = std::atof(line.c_str());
					++j;
				}
				++i;
				ss.str("");
				ss.clear();
			}
			distFile.close();
		}

		std::cout << "Distance between 0 and 1 is " << distanceMatrix[0][1] << std::endl;
	}

	gettimeofday(&start, NULL);

	sil.computeValue(normOption,data,Row,Column,group,object,groupNo,isPBF);

	gettimeofday(&end, NULL);
	delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("For norm ");
	tr.timeList.push_back(to_string(normOption)+"\n");

	tr.eventList.push_back("Clustering evaluation computing takes: ");
	tr.timeList.push_back(to_string(delta)+"s");

	ValidityMeasurement vm;
	vm.computeValue(normOption, data, group, object, isPBF);

	tr.eventList.push_back("kmeans Validity measure is: ");
	stringstream fc_ss;
	fc_ss << vm.f_c;
	tr.timeList.push_back(fc_ss.str());

	/* write value of the silhouette class */
	IOHandler::writeReadme(entropy, sil, "For norm "+to_string(normOption));

}


/*
 * @brief Perform the AHC-average on the dimensionality-reduced space coordinates
 *
 * @param[in] cArray The dimension-reduced matrix coordinates
 * @param[in] PC_Number The number of PCs
 * @param[in] SingVec The singular vector matrix
 * @param[in] meanTrajectory The mean coordinate of the trajectory
 * @param[out] massCenter The centroid coordinates of the clusters
 * @param[in] Cluster The number of clusters as input
 * @param[out] group The labels for all the streamlines
 * @param[out] totalNum The size of all the clusters
 * @param[out] closest The coordinates of the closest extracted lines
 * @param[out] furthest The coordinates of the furthest extracted lines
 * @param[in] data The matrix coordinates of all the streamlines
 * @param[out] tr The TimeRecorder class object
 * @param[out] sil The Silhouette class object
 */
void PCA_Cluster::perform_AHC(const Eigen::MatrixXf& cArray, const int& PC_Number, const Eigen::MatrixXf& SingVec,
		const VectorXf& meanTrajectory, std::vector<MeanLine>& massCenter, const int& Cluster,
		std::vector<int>& group, std::vector<int>& totalNum, std::vector<ExtractedLine>& closest,
		std::vector<ExtractedLine>& furthest, const Eigen::MatrixXf& data, TimeRecorder& tr, Silhouette& sil)
{
	std::unordered_map<int, AHC_node> nodeMap;
	std::vector<DistNode> dNodeVec;
	std::vector<AHC_node> nodeVec;
	const int& Row = cArray.rows();
	const int& Column = cArray.cols();

	/* compute distance matrix for reduced_space */
	Eigen::MatrixXf reduced_dist_matrix = Eigen::MatrixXf::Zero(Row, Row);
#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		for (int j = 0; j < Row; ++j)
		{
			/* don't wish to waste computation on diagonal element */
			if(i==j)
				continue;
			else
				reduced_dist_matrix(i,j) = (cArray.row(i)-cArray.row(j)).norm();
		}
	}
	/* set the ditNode vector */
	setValue(dNodeVec, cArray, reduced_dist_matrix);

	/* perform hirarchical clustering where within each step would merge two nodes */
	hierarchicalMerging(nodeMap, dNodeVec, nodeVec, reduced_dist_matrix, cArray, Cluster, tr);

	vector<vector<int> > neighborVec(Cluster);

	// element size for all groups
	vector<int> storage(Cluster);

	// geometric center
	Eigen::MatrixXf centroid = Eigen::MatrixXf::Zero(Cluster,Column);

	std::vector<int> recorder(Row);
	// set label information
	setLabel(nodeVec, neighborVec, storage, centroid, cArray, recorder);

	nodeVec.clear();

	struct timeval start, end;
	double delta;
	std::multimap<int,int> groupMap;

	float entropy = 0.0;
	float probability;

	for (int i = 0; i < Cluster; ++i)
	{
		groupMap.insert(std::pair<int,int>(storage[i],i));
		if(storage[i]>0)
		{
			probability = float(storage[i])/float(Row);
			entropy += probability*log2f(probability);
		}
	}

	int groupNo = 0;
	int increasingOrder[Cluster];
	for (multimap<int,int>::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
	{
		if(it->first>0)
		{
			increasingOrder[it->second] = (groupNo++);
		}
	}

	/* calculate the balanced entropy */
	entropy = -entropy/log2f(groupNo);
	Eigen::MatrixXf clusterCenter(Cluster, Column);

#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		group[i] = increasingOrder[recorder[i]];
		totalNum[i] = storage[recorder[i]];
	}

#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Cluster; ++i)
	{
		clusterCenter.row(increasingOrder[i]) = centroid.row(i);
	}

	float shortest, farDist, toCenter;
	int shortestIndex = 0, fartestIndex = 0, tempIndex = 0;
	std::vector<int> neighborTemp;

	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0 && !neighborVec[i].empty())
		{
			neighborTemp = neighborVec[i];
			shortest = FLT_MAX;
			farDist = FLT_MIN;

			for (int j = 0; j < storage[i]; ++j)
			{
				tempIndex = neighborTemp[j];
				toCenter = (clusterCenter.row(i)-cArray.row(tempIndex)).norm();

				if(toCenter<shortest)
				{
					shortest = toCenter;
					shortestIndex = tempIndex;
				}
				if(toCenter>farDist)
				{
					farDist = toCenter;
					fartestIndex = tempIndex;
				}
			}
			closest.push_back(ExtractedLine(shortestIndex,increasingOrder[i]));
			furthest.push_back(ExtractedLine(fartestIndex,increasingOrder[i]));
		}
	}
	MatrixXf pcSing(PC_Number,Column);

#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < PC_Number; ++i)
	{
		pcSing.row(i) = SingVec.row(i);
	}

	MatrixXf massPos = clusterCenter*pcSing;

	for (int i = 0; i < Cluster; ++i)
	{
		if(storage[i]>0)
		{
			massPos.row(i) += meanTrajectory.transpose();
			std::vector<float> vecTemp;
			for (int j = 0; j < Column; ++j)
			{
				vecTemp.push_back(massPos(i,j));
			}
			massCenter.push_back(MeanLine(vecTemp,increasingOrder[i]));
		}
	}

	ValidityMeasurement vm;
	vm.computeValue(cArray, group);

	tr.eventList.push_back("PCA Validity measure is: ");
	stringstream fc_ss;
	fc_ss << vm.f_c;
	tr.timeList.push_back(fc_ss.str());

	/* Silhouette effect */
	gettimeofday(&start, NULL);

	sil.computeValue(cArray,group,groupNo,isPBF);

	gettimeofday(&end, NULL);
	delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("Clustering evaluation computing takes: ");
	tr.timeList.push_back(to_string(delta)+"s");

	/* write value of the silhouette class */
	IOHandler::writeReadme(entropy, sil, "");

}


/*
 * @brief Perform AHC merging by given an input number of clusters
 *
 * @param[out] nodeMap The hash map for nodes
 * @param[out] dNodeVec The node vector for nodes
 * @param[out] nodeVec The vector of AHC hierarchical clustering node
 * @param[in] reduced_dist_matrix The distance matrix of the dimensionality reduced space coordinates
 * @param[in] cArray The dimensionality reduce coordinates
 * @param[in] numberOfClusters The number of clusters as input
 * @param[out] tr The TimeRecorder object
 */
void PCA_Cluster::hierarchicalMerging(std::unordered_map<int, AHC_node>& nodeMap, std::vector<DistNode>& dNodeVec,
		std::vector<AHC_node>& nodeVec, const Eigen::MatrixXf& reduced_dist_matrix, const Eigen::MatrixXf& cArray,
		const int& numberOfClusters, TimeRecorder& tr)
{
	/* would store distance matrix instead because it would save massive time */
	struct timeval start, end;
	double timeTemp;
	gettimeofday(&start, NULL);

	const int Row = cArray.rows();

	for(int i=0;i<Row;++i)
	{
		nodeMap[i].element.push_back(i);
	}

	DistNode poped;

	/* find node-pair with minimal distance */
	float minDist = FLT_MAX;
	int target = -1;
	for (int i = 0; i < dNodeVec.size(); ++i)
	{
		if(dNodeVec[i].distance<minDist)
		{
			target = i;
			minDist = dNodeVec[i].distance;
		}
	}
	poped = dNodeVec[target];

	int index = Row, currentNumber;
	do
	{
		//create new node merged and input it into hash map
		vector<int> first = (nodeMap[poped.first]).element;
		vector<int> second = (nodeMap[poped.second]).element;

		/* index would be starting from Row */
		AHC_node newNode(index);
		newNode.element = first;
		newNode.element.insert(newNode.element.end(), second.begin(), second.end());
		nodeMap.insert(make_pair(index, newNode));

		//delete two original nodes
		nodeMap.erase(poped.first);
		nodeMap.erase(poped.second);

		/* the difficulty lies how to update the min-heap with linkage
		 * This would take 2NlogN.
		 * Copy all node-pairs that are not relevant to merged nodes to new vec.
		 * For relevant, would update the mutual distance by linkage
		 */

		/* how many clusters exist */
		currentNumber = nodeMap.size();

		target = -1, minDist = FLT_MAX;

		std::vector<DistNode> tempVec(currentNumber*(currentNumber-1)/2);
		int current = 0, i_first, i_second;
		for(int i=0;i<dNodeVec.size();++i)
		{
			i_first=dNodeVec[i].first, i_second=dNodeVec[i].second;
			/* not relevant, directly copied to new vec */
			if(i_first!=poped.first&&i_first!=poped.second&&i_second!=poped.first&&i_second!=poped.second)
			{
				tempVec[current]=dNodeVec[i];
				if(tempVec[current].distance<minDist)
				{
					target = current;
					minDist = tempVec[current].distance;
				}
				++current;
			}
		}

		for (auto iter=nodeMap.begin();iter!=nodeMap.end();++iter)
		{
			if((*iter).first!=newNode.index)
			{
				tempVec[current].first = (*iter).first;
				tempVec[current].second = newNode.index;
				tempVec[current].distance=getDistAtNodes(newNode.element,(*iter).second.element, reduced_dist_matrix);
				if(tempVec[current].distance<minDist)
				{
					target = current;
					minDist = tempVec[current].distance;
				}
				++current;
			}
		}
		poped = tempVec[target];

		/* judge whether current is assigned to right value */
		assert(current==tempVec.size());
		dNodeVec.clear();
		dNodeVec = tempVec;
		tempVec.clear();
		++index;
	}while(nodeMap.size()!=numberOfClusters);	//merging happens whenever requested cluster is not met

	nodeVec=std::vector<AHC_node>(nodeMap.size());
	int tag = 0;
	for(auto iter=nodeMap.begin();iter!=nodeMap.end();++iter)
		nodeVec[tag++]=(*iter).second;

	gettimeofday(&end, NULL);
	timeTemp = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("Hirarchical clustering for "+to_string(numberOfClusters)+" groups takes: ");
	tr.timeList.push_back(to_string(timeTemp)+" s");
	/* task completed, would delete memory contents */
	dNodeVec.clear();
	nodeMap.clear();
	/* use alpha function to sort the group by its size */
	std::sort(nodeVec.begin(), nodeVec.end(), [](const AHC_node& e1, const AHC_node& e2)
	{return e1.element.size()<e2.element.size()||(e1.element.size()==e2.element.size()&&e1.index<e2.index);});
}


/*
 * @brief Get the distance between two nodes by a given linkage type
 *
 * @param[in] firstList The first node that contains the candidates
 * @param[in] secondList The second node that contains the candidates
 * @param[in] reduced_dist_matrix The distance matrix of the dimensionality reduced coordinates
 * @return The float value between two nodes
 */
float PCA_Cluster::getDistAtNodes(const vector<int>& firstList, const vector<int>& secondList,
		const Eigen::MatrixXf& reduced_dist_matrix)
{
	const int& m = firstList.size();
	const int& n = secondList.size();
	assert(m!=0);
	assert(n!=0);

	float result, value;
	result = 0;
#pragma omp parallel for reduction(+:result) num_threads(8)
	for(int i=0;i<m;++i)
	{
		for(int j=0;j<n;++j)
		{
			value = reduced_dist_matrix(i,j);
			result+=value;
		}
	}
	result/=m*n;
	return result;
}


/*
 * @brief Set the nodes and perform necessary merges for nodes before the start of AHC clustering
 *
 * @param[out] dNodeVec The vector of nodes with distance
 * @param[in] reduced_data The matrix coordinates of the dimensionality reduce coordinates
 * @param[in] reduced_dist_matrix The distance matrix of the dimensionality reduced coordinates
 */
void PCA_Cluster::setValue(std::vector<DistNode>& dNodeVec, const Eigen::MatrixXf& reduced_data,
						   const Eigen::MatrixXf& reduced_dist_matrix)
{
	const int& Row = reduced_data.rows();
	dNodeVec = std::vector<DistNode>(Row*(Row-1)/2);
	int tag = 0;
	for(int i=0;i<Row-1;++i)
	{
		for(int j=i+1;j<Row;++j)
		{
			dNodeVec[tag].first = i;
			dNodeVec[tag].second = j;
			dNodeVec[tag].distance = reduced_dist_matrix(i, j);
			++tag;
		}
	}
	assert(tag==dNodeVec.size());
}


/*
 * @brief Set the labels for streamlines from the clustering results
 *
 * @param[in] nodeVec The vector of AHC nodes for the AHC clustering results
 * @param[out] neighborVec The candidates that belongs to the clusters
 * @param[out] storage The size of clusters
 * @param[out] centroid The centroid streamline coordinates of all the clusters
 * @param[in] cArray The coordinates of the dimensionality reduced space
 * @param[out] recorder The recorder vector
 */
void PCA_Cluster::setLabel(const std::vector<AHC_node>& nodeVec, vector<vector<int> >& neighborVec,
		vector<int>& storage, Eigen::MatrixXf& centroid, const Eigen::MatrixXf& cArray, std::vector<int>& recorder)
{
// group tag by increasing order
	int groupID = 0;

	// element list for each group
	vector<int> eachContainment;

	// find group id and neighboring vec
	for(auto iter = nodeVec.begin(); iter!=nodeVec.end();++iter)
	{
		eachContainment = (*iter).element;
		neighborVec[groupID] = eachContainment;
	#pragma omp parallel num_threads(8)
		{
		#pragma omp for nowait
			for(int i=0;i<eachContainment.size();++i)
			{
				recorder[eachContainment[i]] = groupID;
			#pragma omp critical
				centroid.row(groupID) += cArray.row(eachContainment[i]);
			}
		}
		storage[groupID] = (*iter).element.size();
		centroid.row(groupID)/=eachContainment.size();
		++groupID;
		eachContainment.clear();
	}
}
