#section support_code_struct

int
APPLY_SPECIFIC(conv_gi)(PyGpuArrayObject *kerns, PyGpuArrayObject *output,
                        PyGpuArrayObject *im,
                        cudnnConvolutionDescriptor_t desc,
                        double alpha, double beta, PyGpuArrayObject **input) {
  cudnnStatus_t err = CUDNN_STATUS_SUCCESS;
  float af = alpha, bf = beta;
  void *alpha_p;
  void *beta_p;
  PyGpuContextObject *c = pygpu_default_context();

  if (PyGpuArray_DIMS(im)[1] != PyGpuArray_DIMS(kerns)[1]) {
    PyErr_SetString(PyExc_ValueError, "images and kernel must have the same "
                    "stack size");
    return 1;
  }

  if (c_set_tensorNd(output, APPLY_SPECIFIC(output)) == -1)
    return 1;
  if (c_set_filter(kerns, APPLY_SPECIFIC(kerns)) == -1)
    return 1;

  switch (im->ga.typecode) {
  case GA_DOUBLE:
    alpha_p = (void *)&alpha;
    beta_p = (void *)&beta;
    break;
  case GA_FLOAT:
  case GA_HALF:
    alpha_p = (void *)&af;
    beta_p = (void *)&bf;
    break;
  default:
    PyErr_SetString(PyExc_TypeError, "Unsupported type in convolution");
    return 1;
  }

#ifdef CONV_INPLACE
  Py_XDECREF(*input);
  *input = im;
  Py_INCREF(*input);
#else
  if (theano_prep_output(input, PyGpuArray_NDIM(im), PyGpuArray_DIMS(im),
                         im->ga.typecode, GA_C_ORDER, c) != 0)
    return 1;
  if (beta != 0.0 && pygpu_move(*input, im))
    return 1;
#endif

  if (c_set_tensorNd(*input, APPLY_SPECIFIC(input)) == -1)
    return 1;

  cudnnConvolutionBwdDataAlgo_t algo = CONV_ALGO;

  cuda_enter(c->ctx);

#ifdef CHOOSE_ALGO
  static int reuse_algo = 0;
  static cudnnConvolutionBwdDataAlgo_t prev_algo = CONV_ALGO;

#ifndef CHOOSE_ONCE
  static size_t prev_kern_dims[5] = {0};
  static size_t prev_top_dims[5] = {0};

  reuse_algo = 1;
  for (unsigned int i = 0; i < PyGpuArray_NDIM(kerns); i++) {
    reuse_algo = (reuse_algo &&
                  PyGpuArray_DIM(kerns, i) == prev_kern_dims[i]);
    reuse_algo = (reuse_algo &&
                  PyGpuArray_DIM(output, i) == prev_top_dims[i]);
  }
#endif

  if (!reuse_algo) {
#ifdef CHOOSE_TIME
    int count;
    cudnnConvolutionBwdDataAlgoPerf_t choice;

    err = cudnnFindConvolutionBackwardDataAlgorithm(
      APPLY_SPECIFIC(_handle), APPLY_SPECIFIC(input), APPLY_SPECIFIC(output), desc,
      APPLY_SPECIFIC(kerns), 1, &count, &choice);

    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError, "error selecting convolution algo: %s",
                   cudnnGetErrorString(err));
      cuda_exit(c->ctx);
      return 1;
    }

    algo = choice.algo;
#else
    size_t free = 0, total = 0;
    cudaError_t err2 = cudaMemGetInfo(&free, &total);
    if (err2 != cudaSuccess){
      cudaGetLastError();
      PyErr_Format(PyExc_RuntimeError, "Error when trying to find the memory "
                   "information on the GPU: %s\n", cudaGetErrorString(err2));
      cuda_exit(c->ctx);
      return 1;
    }

    err = cudnnGetConvolutionBackwardDataAlgorithm(
      APPLY_SPECIFIC(_handle), APPLY_SPECIFIC(input), APPLY_SPECIFIC(output),
      desc, APPLY_SPECIFIC(kerns),
      CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT, free, &algo);
    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError, "error selecting convolution algo: %s",
                   cudnnGetErrorString(err));
      cuda_exit(c->ctx);
      return 1;
    }
#endif
    prev_algo = algo;
  } else {
    algo = prev_algo;
  }

#ifdef CHOOSE_ONCE
  reuse_algo = 1;
#else
  for (unsigned int i = 0; i < PyGpuArray_NDIM(kerns); i++) {
    prev_kern_dims[i] = PyGpuArray_DIM(kerns, i);
    prev_top_dims[i] = PyGpuArray_DIM(output, i);
  }
#endif

#endif

#if CUDNN_VERSION > 3000
  if (algo == CUDNN_CONVOLUTION_BWD_DATA_ALGO_FFT) {
    int nd;
    int pad[2];
    int stride[2];
    int upscale[2];
    cudnnConvolutionMode_t mode;
    err = cudnnGetConvolutionNdDescriptor(desc, 2, &nd, pad, stride,
                                          upscale, &mode);
    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError,
                   "error getting convolution properties: %s",
                   cudnnGetErrorString(err));
      cuda_exit(c->ctx);
      return 1;
    }

    if (stride[0] != 1 || stride[1] != 1 ||
        PyGpuArray_DIM(*input, 0) > 1024 || PyGpuArray_DIM(*input, 1) > 1024 ||
        (PyGpuArray_DIM(kerns, 0) == 1 && PyGpuArray_DIM(kerns, 1) == 1)) {
      algo = CUDNN_CONVOLUTION_BWD_DATA_ALGO_0;
    }
  }
#endif

  size_t worksize;
  gpudata *workspace;

  err = cudnnGetConvolutionBackwardDataWorkspaceSize(
    APPLY_SPECIFIC(_handle), APPLY_SPECIFIC(kerns), APPLY_SPECIFIC(output), desc,
    APPLY_SPECIFIC(input), algo, &worksize);

  if (err != CUDNN_STATUS_SUCCESS) {
    PyErr_Format(PyExc_RuntimeError, "error getting worksize: %s",
                 cudnnGetErrorString(err));
    cuda_exit(c->ctx);
    return 1;
  }

  if (worksize != 0) {
    workspace = c->ops->buffer_alloc(c->ctx, worksize, NULL, 0, NULL);
    if (workspace == NULL) {
      PyErr_SetString(PyExc_RuntimeError,
                      "Could not allocate working memory");
      cuda_exit(c->ctx);
      return 1;
    }
  }

  err = cudnnConvolutionBackwardData_v3(
    APPLY_SPECIFIC(_handle),
    alpha_p,
    APPLY_SPECIFIC(kerns), PyGpuArray_DEV_DATA(kerns),
    APPLY_SPECIFIC(output), PyGpuArray_DEV_DATA(output),
    desc, algo, worksize == 0 ? NULL : *(void **)workspace, worksize,
    beta_p,
    APPLY_SPECIFIC(input), PyGpuArray_DEV_DATA(*input));

  if (worksize != 0)
    c->ops->buffer_release(workspace);

  cuda_exit(c->ctx);

  if (err != CUDNN_STATUS_SUCCESS) {
    PyErr_Format(PyExc_RuntimeError, "error doing operation: %s",
                 cudnnGetErrorString(err));
    return 1;
  }
  return 0;
}
