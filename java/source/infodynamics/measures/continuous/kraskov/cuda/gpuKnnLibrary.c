#include "gpuKnnBF_kernel.cu"
#include <stdio.h>
#include "helperfunctions.cu"
#include "ctimer.h"

#include "cub/cub.cuh"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate all necessary memory for the whole calculation in a single call
 * to cudaMalloc, and point the pointers to the right place.
 */
int allocateDeviceMemory(int signalLength, int kth, int dimx, int dimy,
    float **source, float **dest, float **distances, int **indexes,
    float **radii, int **nx, int **ny, float **digammas, float *pointset) {

  float *d_pointset;
  int dims = dimx + dimy;
  size_t mempointset = signalLength * dims * sizeof(float);
  size_t memdistances = signalLength * kth * sizeof(float);
  size_t memindexes = signalLength * kth * sizeof(int);
  size_t memcounts = 2 * signalLength * sizeof(int);
  size_t memdigammas = signalLength * sizeof(float);
  size_t memtotal = mempointset + memdistances + memindexes + memcounts + memdigammas;

  checkCudaErrors( cudaMalloc((void **) &d_pointset, memtotal) );

  cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  checkCudaErrors( cudaMemcpy(d_pointset, pointset, mempointset, cudaMemcpyHostToDevice) );

  *source = d_pointset;
  *dest = *source + signalLength*dimx;
  *distances = *dest + signalLength*dimy;
  *radii = *distances + (kth-1)*signalLength;
  *indexes = (int *) (*distances + kth*signalLength);
  *nx = *indexes + signalLength;
  *ny = *nx + signalLength;
  *digammas = (float *) (*ny + signalLength);

  return 1;
}

/**
 * Free all the memory used in GPU.
 */
int freeDeviceMemory(float *d_pointset) {
  checkCudaErrors( cudaFree(d_pointset) );

  cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  return 1;
}

int d_cudaFindKnn(int* d_bf_indexes, float* d_bf_distances, float* d_bf_pointset,
    float* d_bf_query, int kth, int thelier, int nchunks, int pointdim,
    int signalLength, int useMaxNorm) {

  // Kernel parameters
  dim3 threads(1,1,1);
  dim3 grid(1,1,1);
  threads.x = 512;
  grid.x = (signalLength-1)/threads.x + 1;
  int memkernel = kth*sizeof(float)*threads.x+\
          kth*sizeof(int)*threads.x;
  int triallength = signalLength / nchunks;

  // Pointer to the function used to calculate norms
  normFunction_t *normFunction;
  cudaMalloc( (void **) &normFunction, sizeof(normFunction_t) );
  if (useMaxNorm) {
    cudaMemcpyFromSymbol(normFunction, pMaxNorm, sizeof(normFunction_t));
  } else {
    cudaMemcpyFromSymbol(normFunction, pSquareNorm, sizeof(normFunction_t));
  }

  // Launch kernel
  kernelKNNshared<<<grid.x, threads.x, memkernel>>>(d_bf_query, d_bf_pointset,
      d_bf_indexes, d_bf_distances, pointdim, triallength, signalLength, kth,
      thelier, normFunction);

  checkCudaErrors( cudaDeviceSynchronize() );

  cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  return 1;
}


int d_cudaFindRSAll(int* d_bf_npointsrange, float* d_bf_pointset, float* d_bf_query,
    float* d_bf_vecradius, int thelier, int nchunks, int pointdim,
    int signalLength, int useMaxNorm) {

  // Kernel parameters
  dim3 threads(1,1,1);
  dim3 grid(1,1,1);
  threads.x = 512;
  grid.x = (signalLength-1)/threads.x + 1;
  int memkernel = sizeof(int)*threads.x;
  int triallength = signalLength / nchunks;

  // Pointer to the function used to calculate norms
  normFunction_t *normFunction;
  cudaMalloc( (void **) &normFunction, sizeof(normFunction_t) );
  if (useMaxNorm) {
    cudaMemcpyFromSymbol(normFunction, pMaxNorm, sizeof(normFunction_t));
  } else {
    cudaMemcpyFromSymbol(normFunction, pSquareNorm, sizeof(normFunction_t));
  }

  // Launch kernel
  kernelBFRSAllshared<<< grid.x, threads.x, memkernel>>>(
          d_bf_query, d_bf_pointset, d_bf_npointsrange, pointdim,
          triallength, signalLength, thelier, d_bf_vecradius, normFunction);

  checkCudaErrors(cudaDeviceSynchronize());

  cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  checkCudaErrors( cudaFree(normFunction) );

  return 1;
}


int cudaFindKnn(int* h_bf_indexes, float* h_bf_distances, float* h_pointset,
    float* h_query, int kth, int thelier, int nchunks, int pointdim,
    int signalLength, unsigned int useMaxNorm) {
  float *d_bf_pointset, *d_bf_query;
  int *d_bf_indexes;
  float *d_bf_distances;

  unsigned int meminputsignalquerypointset= pointdim * signalLength * sizeof(float);
  unsigned int mem_bfcl_outputsignaldistances= kth * signalLength * sizeof(float);
  unsigned int mem_bfcl_outputsignalindexes = kth * signalLength * sizeof(int);

  CPerfTimer pt1 = startTimer("kNN_upload");
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_query), meminputsignalquerypointset));
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_pointset), meminputsignalquerypointset));
  //GPU output
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_distances), mem_bfcl_outputsignaldistances ));
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_indexes), mem_bfcl_outputsignalindexes ));

  cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  //Upload input data
  checkCudaErrors( cudaMemcpy(d_bf_query, h_query, meminputsignalquerypointset, cudaMemcpyHostToDevice ));
  checkCudaErrors( cudaMemcpy(d_bf_pointset, h_pointset, meminputsignalquerypointset, cudaMemcpyHostToDevice ));
  error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }
  stopTimer(pt1);
  CPerfTimer pt2 = startTimer("kNN_kernel");

  d_cudaFindKnn(d_bf_indexes, d_bf_distances, d_bf_pointset,
                d_bf_query, kth, thelier, nchunks, pointdim,
                signalLength, useMaxNorm);


  checkCudaErrors( cudaDeviceSynchronize() );
  stopTimer(pt2);
  CPerfTimer pt3 = startTimer("kNN_download");

  //Download result
  checkCudaErrors( cudaMemcpy( h_bf_distances, d_bf_distances, mem_bfcl_outputsignaldistances, cudaMemcpyDeviceToHost) );
  checkCudaErrors( cudaMemcpy( h_bf_indexes, d_bf_indexes, mem_bfcl_outputsignalindexes, cudaMemcpyDeviceToHost) );
  error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  //Free resources
  checkCudaErrors(cudaFree(d_bf_query));
  checkCudaErrors(cudaFree(d_bf_pointset));
  checkCudaErrors(cudaFree(d_bf_distances));
  checkCudaErrors(cudaFree(d_bf_indexes));
  stopTimer(pt3);
  // cudaDeviceReset();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }
  
  return 1;
}


int cudaFindKnnSetGPU(int* h_bf_indexes, float* h_bf_distances,
    float* h_pointset, float* h_query, int kth, int thelier, int nchunks,
    int pointdim, int signalLength, unsigned int useMaxNorm, int deviceid) {
  cudaSetDevice(deviceid);
  return cudaFindKnn(h_bf_indexes, h_bf_distances, h_pointset, h_query,
      kth, thelier, nchunks, pointdim, signalLength, useMaxNorm);
}

/*
 * Range search being radius a vector of length number points in queryset/pointset
 */
int cudaFindRSAll(int* h_bf_npointsrange, float* h_pointset, float* h_query,
    float* h_vecradius, int thelier, int nchunks, int pointdim,
    int signalLength, unsigned int useMaxNorm) {

  float *d_bf_pointset, *d_bf_query, *d_bf_vecradius;
  int *d_bf_npointsrange;

  unsigned int meminputsignalquerypointset= pointdim * signalLength * sizeof(float);
  unsigned int mem_bfcl_outputsignalnpointsrange= signalLength * sizeof(int);
  unsigned int mem_bfcl_inputvecradius = signalLength * sizeof(float);

  CPerfTimer pt1 = startTimer("RS_upload");
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_query), meminputsignalquerypointset));
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_pointset), meminputsignalquerypointset));
  checkCudaErrors( cudaMalloc( (void**) &(d_bf_npointsrange), mem_bfcl_outputsignalnpointsrange ));
    checkCudaErrors( cudaMalloc( (void**) &(d_bf_vecradius), mem_bfcl_inputvecradius ));

    cudaError_t error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }
  //Upload input data
  checkCudaErrors( cudaMemcpy(d_bf_query, h_query, meminputsignalquerypointset, cudaMemcpyHostToDevice ));
  checkCudaErrors( cudaMemcpy(d_bf_pointset, h_pointset, meminputsignalquerypointset, cudaMemcpyHostToDevice ));
  checkCudaErrors( cudaMemcpy(d_bf_vecradius, h_vecradius, mem_bfcl_inputvecradius, cudaMemcpyHostToDevice ));

    error = cudaGetLastError();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }
  stopTimer(pt1);
  CPerfTimer pt2 = startTimer("RS_kernel");

  d_cudaFindRSAll(d_bf_npointsrange, d_bf_pointset, d_bf_query,
    d_bf_vecradius, thelier, nchunks, pointdim, signalLength, useMaxNorm);

  stopTimer(pt2);
  CPerfTimer pt3 = startTimer("RS_download");

  checkCudaErrors( cudaMemcpy( h_bf_npointsrange, d_bf_npointsrange,mem_bfcl_outputsignalnpointsrange, cudaMemcpyDeviceToHost) );


  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }

  // Free resources
  checkCudaErrors(cudaFree(d_bf_query));
  checkCudaErrors(cudaFree(d_bf_pointset));
  checkCudaErrors(cudaFree(d_bf_npointsrange));
  checkCudaErrors(cudaFree(d_bf_vecradius));
  cudaDeviceReset();
  if(error!=cudaSuccess){
    fprintf(stderr,"%s",cudaGetErrorString(error));
    return 0;
  }
  stopTimer(pt3);
  
  return 1;
}

int cudaFindRSAllSetGPU(int* h_bf_npointsrange, float* h_pointset,
    float* h_query, float* h_vecradius, int thelier, int nchunks,
    int pointdim, int signalLength, unsigned int useMaxNorm, int deviceid) {
  cudaSetDevice(deviceid);
  return cudaFindRSAll(h_bf_npointsrange, h_pointset, h_query, h_vecradius,
      thelier, nchunks, pointdim, signalLength, useMaxNorm);
}


int findRadiiAlgorithm2(float *radii, const float *data, const int *indexes,
    unsigned int k, unsigned int dim, unsigned int N) {

  unsigned int i, j;

  for (j = 0; j < N; j++) {
    radii[j] = 0.0f;
    for (i = 0; i < k; i++) {
      float d = maxMetricPoints(data + j, data + indexes[j + i*N], dim, N);
      if (d > radii[j]) {
        radii[j] = d;
      }
    }
  }

  return 1;

}



int computeSumDigammas(float *sumDiGammas, int *nx, int *ny, unsigned int N) {

  int *d_nx, *d_ny;
  float *d_sumDiGammas, *partialSumDiGammas;

  unsigned int threads_per_block = 512;
  dim3 n_blocks, n_threads;
  n_blocks.x = ((N % threads_per_block) != 0) ? (N / threads_per_block + 1) : (N / threads_per_block);
  n_threads.x = threads_per_block;
  int memkernel = sizeof(int)*n_threads.x;

  {
  CPerfTimer pt = startTimer("Digammas_upload");
  partialSumDiGammas = (float *) malloc(n_blocks.x * sizeof(float));

  checkCudaErrors( cudaMalloc((void **) &d_nx, N * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_ny, N * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_sumDiGammas, n_blocks.x * sizeof(float)) );

  checkCudaErrors( cudaMemcpy(d_nx, nx, N*sizeof(int), cudaMemcpyHostToDevice) );
  checkCudaErrors( cudaMemcpy(d_ny, ny, N*sizeof(int), cudaMemcpyHostToDevice) );
  stopTimer(pt);
  }

  {
  CPerfTimer pt = startTimer("Digammas_kernel");
  // reduce6<<<n_blocks, n_threads>>>(d_nx, d_ny, d_sumDiGammas, N);
  reduce1<<<n_blocks, n_threads, memkernel>>>(d_nx, d_ny, d_sumDiGammas, N);

  checkCudaErrors( cudaDeviceSynchronize() );
  stopTimer(pt);
  }

  CPerfTimer pt = startTimer("Digammas_download");

  checkCudaErrors( cudaMemcpy(partialSumDiGammas, d_sumDiGammas, n_blocks.x * sizeof(float), cudaMemcpyDeviceToHost) );

  checkCudaErrors( cudaDeviceSynchronize() );

  float tmp = 0;
  for (unsigned int i = 0; i < n_blocks.x; i++) {
    // printf("From block %d we got %f\n", i, partialSumDiGammas[i]);
    tmp += partialSumDiGammas[i];
  }
  *sumDiGammas = tmp;

  free(partialSumDiGammas);
  cudaFree(d_nx);
  cudaFree(d_ny);
  cudaFree(d_sumDiGammas);
  stopTimer(pt);

  return 1;

}

int d_parallelDigammas(float *digammas, float *d_digammas, int *d_nx,
    int *d_ny, int signalLength) {

  // Kernel parameters
  dim3 threads(1,1,1);
  dim3 grid(1,1,1);
  threads.x = 512;
  grid.x = (signalLength-1)/threads.x + 1;

  // Launch kernel
  gpuDigammas<<<grid.x, threads.x>>>(d_digammas, d_nx, d_ny, signalLength);
  checkCudaErrors( cudaDeviceSynchronize() );

  checkCudaErrors( cudaMemcpy(digammas, d_digammas, signalLength * sizeof(float), cudaMemcpyDeviceToHost) );
  checkCudaErrors( cudaDeviceSynchronize() );

  return 1;
}


int parallelDigammas(float *digammas, int *nx, int *ny, int signalLength) {

  int *d_nx, *d_ny;
  float *d_digammas;

  {
  CPerfTimer pt = startTimer("Digammas_upload");
  checkCudaErrors( cudaMalloc((void **) &d_nx, signalLength * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_ny, signalLength * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_digammas, signalLength * sizeof(float)) );

  checkCudaErrors( cudaMemcpy(d_nx, nx, signalLength*sizeof(int), cudaMemcpyHostToDevice) );
  checkCudaErrors( cudaMemcpy(d_ny, ny, signalLength*sizeof(int), cudaMemcpyHostToDevice) );
  stopTimer(pt);
  }

  {
  CPerfTimer pt = startTimer("Digammas_kernel");
  d_parallelDigammas(digammas, d_digammas, d_nx, d_ny, signalLength);
  stopTimer(pt);
  }

  {
  CPerfTimer pt = startTimer("Digammas_download");
  checkCudaErrors( cudaFree(d_nx) );
  checkCudaErrors( cudaFree(d_ny) );
  checkCudaErrors( cudaFree(d_digammas) );
  stopTimer(pt);
  }

  return 1;
}

int d_cudaSumDigammas(float *sumDigammas, int *d_nx, int *d_ny,
    float *d_digammas, int trialLength, int nchunks) {

  float *d_sumDigammas;
  int signalLength = trialLength * nchunks;

  // Kernel parameters
  dim3 threads(1,1,1);
  dim3 grid(1,1,1);
  threads.x = 512;
  grid.x = (signalLength-1)/threads.x + 1;
  checkCudaErrors( cudaMalloc((void **) &d_sumDigammas, nchunks * sizeof(int)) );

  // Launch kernel to calculate (digamma(nx+1) + digamma(ny+1)), and leave
  // results in GPU
  gpuDigammas<<<grid.x, threads.x>>>(d_digammas, d_nx, d_ny, signalLength);
  checkCudaErrors( cudaDeviceSynchronize() );

  int offset_size = nchunks + 1;
  int offsets[offset_size];
  for (int i = 0; i < (nchunks+1); i++) { offsets[i] = i*trialLength; }
  int *d_offsets;
  checkCudaErrors(cudaMalloc((void **) &d_offsets, (nchunks + 1)*sizeof(int)));
  checkCudaErrors(cudaMemcpy(d_offsets, offsets, (nchunks + 1)*sizeof(int), cudaMemcpyHostToDevice));

  void     *d_temp_storage = NULL;
  size_t   temp_storage_bytes = 0;
  cub::DeviceSegmentedReduce::Sum(d_temp_storage, temp_storage_bytes, d_digammas, d_sumDigammas,
          nchunks, d_offsets, d_offsets + 1);
  // Allocate temporary storage
  checkCudaErrors( cudaMalloc(&d_temp_storage, temp_storage_bytes) );
  checkCudaErrors( cudaDeviceSynchronize() );

  // Run sum-reduction
  cub::DeviceSegmentedReduce::Sum(d_temp_storage, temp_storage_bytes, d_digammas, d_sumDigammas,
      nchunks, d_offsets, d_offsets + 1);
  checkCudaErrors( cudaDeviceSynchronize() );

  checkCudaErrors( cudaFree(d_temp_storage) );
  checkCudaErrors( cudaFree(d_offsets) );

  checkCudaErrors(cudaMemcpy(sumDigammas, d_sumDigammas, nchunks*sizeof(float), cudaMemcpyDeviceToHost));
  checkCudaErrors( cudaFree(d_sumDigammas) );

  return 1;
}

/**
 * Calculate and sum digammas in chunks fully in GPU.
 */
int cudaSumDigammas(float *sumDigammas, int *nx, int *ny,
    int trialLength, int nchunks) {

  // Copy neighbour counts to device and allocate memory for all digammas
  int *d_nx, *d_ny;
  float *d_digammas;
  int signalLength = trialLength * nchunks;

  checkCudaErrors( cudaMalloc((void **) &d_nx, signalLength * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_ny, signalLength * sizeof(int)) );
  checkCudaErrors( cudaMalloc((void **) &d_digammas, signalLength * sizeof(float)) );

  checkCudaErrors( cudaMemcpy(d_nx, nx, signalLength*sizeof(int), cudaMemcpyHostToDevice) );
  checkCudaErrors( cudaMemcpy(d_ny, ny, signalLength*sizeof(int), cudaMemcpyHostToDevice) );

  d_cudaSumDigammas(sumDigammas, d_nx, d_ny, d_digammas, trialLength, nchunks);

  checkCudaErrors( cudaFree(d_nx) );
  checkCudaErrors( cudaFree(d_ny) );
  checkCudaErrors( cudaFree(d_digammas) );

  return 1;
}

void device_reset(void) {
  cudaDeviceReset();
}

void gpuWarmUp(void) {
  cudaSetDevice(0);
}
#ifdef __cplusplus
}
#endif

