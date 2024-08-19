//
// Created by rubato on 19/8/24.
//
#include <iostream>
#include <torch/torch.h>
#include <CANDY.h>
#include <Utils/UtilityFunctions.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

using namespace INTELLI;
static inline CANDY::AbstractIndexPtr indexPtr = nullptr;
static inline std::vector<INTELLI::IntelliTimeStampPtr> timeStamps;
static inline timer_t timerid;
bool fileExists(const std::string &filename) {
    std::ifstream file(filename);
    return file.good(); // Returns true if the file is open and in a good state
}

int main(int argc, char **argv){

    /**
     * @brief 1. load the configs
     */
    INTELLI::ConfigMapPtr inMap = newConfigMap();
    //size_t incrementalBuildTime = 0, incrementalSearchTime = 0;
    if (inMap->fromCArg(argc, argv) == false) {
        if (argc >= 2) {
            std::string fileName = "";
            fileName += argv[1];
            if (inMap->fromFile(fileName)) { std::cout << "load config from file " + fileName << endl; }
        }
    }
    /**
     * @brief 2. create the data and query, and prepare initialTensor
     */
    CANDY::DataLoaderTable dataLoaderTable;
    std::string dataLoaderTag = inMap->tryString("dataLoaderTag", "random", true);
    int64_t cutOffTimeSeconds = inMap->tryI64("cutOffTimeSeconds", -1, true);
    int64_t waitPendingWrite = inMap->tryI64("waitPendingWrite", 0, true);
    auto dataLoader = dataLoaderTable.findDataLoader(dataLoaderTag);
    INTELLI_INFO("2.Data loader =" + dataLoaderTag);
    if (dataLoader == nullptr) {
        return -1;
    }
    dataLoader->setConfig(inMap);
    int64_t initialRows = inMap->tryI64("initialRows", 0, true);
    auto dataTensorAll = dataLoader->getData().nan_to_num(0);
    //auto dataTensorAll = dataLoader->getData();
    auto dataTensorInitial = dataTensorAll.slice(0, 0, initialRows);
    auto dataTensorStream = dataTensorAll.slice(0, initialRows, dataTensorAll.size(0));
    //auto queryTensor = dataLoader->getQuery();
    auto queryTensor = dataLoader->getQuery().nan_to_num(0);
    INTELLI_INFO(
            "Initial tensor: Demension =" + std::to_string(dataTensorInitial.size(1)) + ",#data="
            + std::to_string(dataTensorInitial.size(0)));
    INTELLI_INFO(
            "Streaming tensor: Demension =" + std::to_string(dataTensorStream.size(1)) + ",#data="
            + std::to_string(dataTensorStream.size(0)) + ",#query="
            + std::to_string(queryTensor.size(0)));

    /**
    * @brief 3. create the timestamps
    */
    INTELLI::IntelliTimeStampGenerator timeStampGen;
    inMap->edit("streamingTupleCnt", (int64_t) dataTensorStream.size(0));
    timeStampGen.setConfig(inMap);
    timeStamps = timeStampGen.getTimeStamps();
    INTELLI_INFO("3.TimeStampSize =" + std::to_string(timeStamps.size()));
    int64_t batchSize = inMap->tryI64("batchSize", dataTensorStream.size(0), true);
    /**
     * @brief 4. creat index
     */
    CANDY::IndexTable indexTable;
    std::string indexTag = inMap->tryString("indexTag", "flat", true);
    indexPtr = indexTable.getIndex(indexTag);
    if (indexPtr == nullptr) {
        return -1;
    }
    indexPtr->setConfig(inMap);
    indexPtr->startHPC();
    indexPtr->isHPCStarted = true;
    /**
     * @brief 5. streaming feed
     */
    uint64_t startRow = 0;
    uint64_t endRow = startRow + batchSize;
    uint64_t tNow = 0;
    uint64_t tEXpectedArrival = timeStamps[endRow - 1]->arrivalTime;
    uint64_t tp = 0;
    uint64_t tDone = 0;
    uint64_t aRows = dataTensorStream.size(0);
    if (cutOffTimeSeconds > 0) {
        setEarlyTerminateTimer(cutOffTimeSeconds);
        s_timeOutSeconds = cutOffTimeSeconds;
        INTELLI_WARNING(
                "Allow up to" + std::to_string(cutOffTimeSeconds) + "seconds before termination");
    }
    INTELLI_INFO("3.0 Load initial tensor!");
    if (initialRows > 0) {

        /*auto qTemp=queryTensor.clone();
        if(inMap->tryI64("loadQueryDistribution",0, false)==1) {
          INTELLI_WARNING("The distribution of query is also loaded");
          for (; qTemp.size(0) < dataTensorInitial.size(0);) {
            INTELLI::IntelliTensorOP::appendRows(&qTemp, &queryTensor);
          }
        }*/
        indexPtr->loadInitialTensor(dataTensorInitial);
    }
    auto start = std::chrono::high_resolution_clock::now();
    int64_t frozenLevel = inMap->tryI64("frozenLevel", 1, true);
    indexPtr->setFrozenLevel(frozenLevel);
    INTELLI_INFO("3.1 STREAMING NOW!!!");
    double prossedOld = 0;
    while (startRow < aRows) {
        tNow = chronoElapsedTime(start);
        //index++;
        while (tNow < tEXpectedArrival) {
            tNow = chronoElapsedTime(start);
        }
        double prossed = endRow;
        prossed = prossed * 100.0 / aRows;

        /**
         * @brief now, the whole batch has arrived, compute
         */
        auto subA = dataTensorStream.slice(0, startRow, endRow);
        indexPtr->insertTensor(subA);
        tp = chronoElapsedTime(start);
        /**
         * @brief the new arrived A will be no longer probed, so we can assign the processed time now
         */
        for (size_t i = startRow; i < endRow; i++) {
            timeStamps[i]->processedTime = tp;
        }
        /**
         * @brief update the indexes
         */

        startRow += batchSize;
        endRow += batchSize;
        if (endRow >= aRows) {
            endRow = aRows;
        }
        if (prossed - prossedOld >= 10.0) {
            INTELLI_INFO("Done" + to_string(prossed) + "%(" + to_string(startRow) + "/" + to_string(aRows) + ")");
            prossedOld = prossed;
        }
        tEXpectedArrival = timeStamps[endRow - 1]->arrivalTime;

    }
    tDone = chronoElapsedTime(start);
    int64_t ANNK = inMap->tryI64("ANNK", 5, true);
    int64_t pendingWriteTime = 0;
    if (waitPendingWrite) {
        INTELLI_WARNING("There is pending write, wait first");
        auto startWP = std::chrono::high_resolution_clock::now();
        indexPtr->waitPendingOperations();
        pendingWriteTime = chronoElapsedTime(startWP);
        INTELLI_INFO("Wait " + std::to_string(pendingWriteTime / 1000) + " ms for pending writings");
    }
    INTELLI_INFO("4 Insert is done, let us validate the results first");
    auto startQuery = std::chrono::high_resolution_clock::now();
    auto indexResults = indexPtr->searchTensor(queryTensor, ANNK);
    tNow = chronoElapsedTime(startQuery);
    INTELLI_INFO("Query done in " + to_string(tNow / 1000) + "ms");
    uint64_t queryLatency = tNow;
    indexPtr->endHPC();
    indexPtr->isHPCStarted = false;
    std::string groundTruthPrefix = inMap->tryString("groundTruthPrefix", "onlineInsert_GroundTruth", true);

    std::string probeName = groundTruthPrefix + "/" + std::to_string(indexResults.size() - 1) + ".rbt";
    double recall = 0.0;
    int64_t groundTruthRedo = inMap->tryI64("groundTruthRedo", 1, true);

    if (fileExists(probeName) && (groundTruthRedo == 0)) {
        INTELLI_INFO("Ground truth exists, so I load it");
        auto gdResults = UtilityFunctions::tensorListFromFile(groundTruthPrefix, indexResults.size());
        INTELLI_INFO("Ground truth is loaded");
        recall = UtilityFunctions::calculateRecall(gdResults, indexResults);
    } else {
        INTELLI_INFO("Ground truth does not exist, so I'll create it");
        auto gdMap = newConfigMap();
        gdMap->loadFrom(*inMap);
        gdMap->edit("faissIndexTag", "flat");
        CANDY::IndexTable indexTable2;
        auto gdIndex = indexTable2.getIndex("faiss");
        gdIndex->setConfig(gdMap);
        if (initialRows > 0) {
            gdIndex->loadInitialTensor(dataTensorInitial);
        }
        gdIndex->insertTensor(dataTensorStream);

        auto gdResults = gdIndex->searchTensor(queryTensor, ANNK);
        INTELLI_INFO("Ground truth is done");
        recall = UtilityFunctions::calculateRecall(gdResults, indexResults);
        UtilityFunctions::tensorListToFile(gdResults, groundTruthPrefix);
        //std::cout<<"index results"<<indexResults[0]<<std::endl;
        //std::cout<<"ground truth results"<<gdResults[0]<<std::endl;
    }

    double throughput = aRows * 1e6 / tDone;
    double throughputByElements = throughput * dataTensorStream.size(1);
    double latency95 = UtilityFunctions::getLatencyPercentage(0.95, timeStamps);
    auto briefOutCfg = newConfigMap();
    briefOutCfg->edit("insertionThroughput", throughput);
    briefOutCfg->edit("insertionRecall", recall);
    briefOutCfg->edit("insertionThroughputByElements", throughputByElements);
    briefOutCfg->edit("insertion95%latency(Insert)", latency95);
    briefOutCfg->edit("insertionPendingWrite", pendingWriteTime);
    briefOutCfg->edit("insertionLatencyOfQuery", queryLatency);
    briefOutCfg->edit("insertionNormalExit", (int64_t) 1);

    std::cout << "brief results\n" << briefOutCfg->toString() << std::endl;
    UtilityFunctions::saveTimeStampToFile("onlineInsert_timestamps.csv", timeStamps);

    INTELLI_INFO("5 First search is done, let us delete!");
    double deleteProp = inMap->tryDouble("deleteProportion", 0.05, true);
    int64_t deleteRows = std::floor(deleteProp*dataTensorAll.size(0));

    int64_t numberCycles = inMap->tryI64("numberCycles", 1, true);
    int64_t cycle=0;
    INTELLI_INFO("5.1 Starting deleteing and re-insertion "+std::to_string(deleteRows) +" rows for " +std::to_string(numberCycles)+" cycles");
    std::mt19937_64 rng(0);
    std::uniform_int_distribution<int64_t> dist(0, dataTensorAll.size(0)-1);
    while(cycle<numberCycles){
        std::vector<int64_t> toDeleteIdx(deleteRows);
        //randomly pick idxes to delete
        for(int j=0; j<deleteRows; j++) {
            rng.seed(std::time(nullptr));
            toDeleteIdx[j] = dist(rng);
        }
        auto delete_start = std::chrono::high_resolution_clock::now();
        if(!indexPtr->deleteTensorByIndex(toDeleteIdx)){
            INTELLI_WARNING("UNSUPPORTED DELETE ACTION FOR INDEX!!!");
            break;
        }
        auto delete_end = chronoElapsedTime(delete_start);

        auto reinsert_start = std::chrono::high_resolution_clock::now();
        for(int j=0; j<deleteRows; j++){
            indexPtr->insertTensor(dataTensorALl.slice(0, toDeleteIdx[j], toDeleteIdx[j]+1));
        }
        auto reinsert_end = chronoElapsedTime(reinsert_start);

        
        auto update_search_start=std::chrono::high_resolution_clock::now();


        // Now for the recall
        gdIndex->deleteTensorByIndex(toDeleteIdx);
        for(int j=0; j<deleteRows; j++){
            gdIndex->insertTensor(dataTensorALl.slice(0, toDeleteIdx[j], toDeleteIdx[j]+1));
        }




        cycle++;
    }



    briefOutCfg->toFile("onlineInsert_result.csv");
}