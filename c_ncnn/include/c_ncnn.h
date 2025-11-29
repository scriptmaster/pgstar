#ifndef C_NCNN_H
#define C_NCNN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Model handle - opaque pointer */
typedef void* c_ncnn_model_t;

/* Error codes */
typedef enum {
    C_NCNN_SUCCESS = 0,
    C_NCNN_ERROR_INVALID_PARAM = -1,
    C_NCNN_ERROR_FILE_NOT_FOUND = -2,
    C_NCNN_ERROR_MODEL_LOAD_FAILED = -3,
    C_NCNN_ERROR_INFERENCE_FAILED = -4,
    C_NCNN_ERROR_MEMORY = -5,
    C_NCNN_ERROR_UNKNOWN = -99
} c_ncnn_error_t;

/* Model format */
typedef enum {
    C_NCNN_FORMAT_NCNN = 0,
    C_NCNN_FORMAT_ONNX = 1
} c_ncnn_format_t;

/* Input/output tensor structure */
typedef struct {
    int width;
    int height;
    int channels;
    float* data;  /* Row-major format: [height][width][channels] */
} c_ncnn_tensor_t;

/* Model loading */
/**
 * Load an NCNN model from param and bin files
 * @param param_path Path to .param file
 * @param bin_path Path to .bin file (or NULL if using .ncnn single file)
 * @return Model handle or NULL on error
 */
c_ncnn_model_t c_ncnn_load_model(const char* param_path, const char* bin_path);

/**
 * Load an ONNX model
 * @param onnx_path Path to .onnx file
 * @return Model handle or NULL on error
 */
c_ncnn_model_t c_ncnn_load_onnx_model(const char* onnx_path);

/**
 * Download and load a model from URL
 * @param url URL to download model from
 * @param format Model format (NCNN or ONNX)
 * @param cache_dir Directory to cache downloaded models (can be NULL for /tmp)
 * @return Model handle or NULL on error
 */
c_ncnn_model_t c_ncnn_download_and_load(const char* url, c_ncnn_format_t format, const char* cache_dir);

/**
 * Unload a model and free resources
 * @param model Model handle (can be NULL)
 */
void c_ncnn_unload_model(c_ncnn_model_t model);

/* Inference */
/**
 * Run inference on a model
 * @param model Model handle
 * @param input Input tensor
 * @param output Output tensor (will be allocated, caller must free with c_ncnn_free_tensor)
 * @return Error code
 */
c_ncnn_error_t c_ncnn_inference(c_ncnn_model_t model, const c_ncnn_tensor_t* input, c_ncnn_tensor_t* output);

/**
 * Run inference with multiple inputs/outputs
 * @param model Model handle
 * @param inputs Array of input tensors
 * @param num_inputs Number of input tensors
 * @param outputs Array of output tensors (will be allocated)
 * @param num_outputs Number of output tensors
 * @return Error code
 */
c_ncnn_error_t c_ncnn_inference_multi(c_ncnn_model_t model,
                                      const c_ncnn_tensor_t* inputs, int num_inputs,
                                      c_ncnn_tensor_t* outputs, int num_outputs);

/* Tensor utilities */
/**
 * Allocate a tensor
 * @param width Width dimension
 * @param height Height dimension
 * @param channels Number of channels
 * @return Allocated tensor or NULL on error
 */
c_ncnn_tensor_t* c_ncnn_alloc_tensor(int width, int height, int channels);

/**
 * Free a tensor allocated by c_ncnn
 * @param tensor Tensor to free (can be NULL)
 */
void c_ncnn_free_tensor(c_ncnn_tensor_t* tensor);

/* Error handling */
/**
 * Get error message for last operation
 * @return Error message string (valid until next operation)
 */
const char* c_ncnn_get_error(void);

#ifdef __cplusplus
}
#endif

#endif /* C_NCNN_H */


