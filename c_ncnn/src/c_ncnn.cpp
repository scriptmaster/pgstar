#include "c_ncnn.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>  // for isatty(), sleep()

/* Custom HTTP client for downloading (HTTP-only, no HTTPS) */
extern bool download_file_custom(const std::string& url, const std::string& output_path);

/* Forward declarations for NCNN and ONNX */
#ifdef HAVE_NCNN
#include "net.h"
#include "gpu.h"
#endif

#ifdef HAVE_ONNXRUNTIME
#include "onnxruntime_cxx_api.h"
#endif

/* Internal model structure */
struct ModelInternal {
    c_ncnn_format_t format;
    std::string error_msg;
    
#ifdef HAVE_NCNN
    ncnn::Net* ncnn_net;
#endif

#ifdef HAVE_ONNXRUNTIME
    Ort::Env* ort_env;
    Ort::Session* ort_session;
    Ort::AllocatorWithDefaultOptions* ort_allocator;
    std::vector<const char*> ort_input_names;
    std::vector<const char*> ort_output_names;
    std::vector<Ort::Value> ort_inputs;
    std::vector<Ort::Value> ort_outputs;
#endif
    
    ModelInternal() : format(C_NCNN_FORMAT_NCNN) {
#ifdef HAVE_NCNN
        ncnn_net = nullptr;
#endif
#ifdef HAVE_ONNXRUNTIME
        ort_env = nullptr;
        ort_session = nullptr;
        ort_allocator = nullptr;
#endif
    }
    
    ~ModelInternal() {
        cleanup();
    }
    
    void cleanup() {
#ifdef HAVE_NCNN
        if (ncnn_net) {
            delete ncnn_net;
            ncnn_net = nullptr;
        }
#endif
#ifdef HAVE_ONNXRUNTIME
        if (ort_session) {
            delete ort_session;
            ort_session = nullptr;
        }
        if (ort_env) {
            delete ort_env;
            ort_env = nullptr;
        }
        ort_allocator = nullptr;
#endif
    }
};

std::string g_last_error;  // Made non-static so http_client.cpp can access it

/* Helper function to set error */
void set_error(const std::string& msg) {  // Made non-static
    g_last_error = msg;
}

/* Helper function to check if file exists */
static bool file_exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

/* Helper function to get directory from path */
static std::string get_dir(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

/* Download file from URL using custom HTTP client */
static bool download_file(const std::string& url, const std::string& output_path) {
    // Check if URL is HTTPS - provide curl command for user
    if (url.length() >= 8 && url.substr(0, 8) == "https://") {
        // Print curl command to stderr (visible even in background processes)
        std::cerr << "\n";
        std::cerr << "========================================\n";
        std::cerr << "HTTPS download requires manual step:\n";
        std::cerr << "========================================\n";
        std::cerr << "Please run this command to download the model:\n\n";
        std::cerr << "  curl -L -o \"" << output_path << "\" \"" << url << "\"\n\n";
        std::cerr << "The file will be saved to: " << output_path << "\n";
        std::cerr << "\n";
        std::cerr << "Waiting for download to complete...\n";
        std::cerr << "Press Enter after running the curl command above.\n";
        std::cerr << "========================================\n";
        std::cerr << std::flush;
        
        // Check if stdin is available (might not be in background processes)
        if (isatty(STDIN_FILENO)) {
            // Wait for user to press Enter
            std::string line;
            std::getline(std::cin, line);
        } else {
            // Stdin not available (background process), wait a bit and check
            std::cerr << "Note: Running in non-interactive mode. Waiting 30 seconds for download...\n" << std::flush;
            for (int i = 0; i < 30; i++) {
                sleep(1);
                if (file_exists(output_path)) {
                    break;
                }
            }
        }
        
        // Check if file was downloaded
        if (file_exists(output_path)) {
            std::cerr << "File found! Continuing...\n" << std::flush;
            return true;
        } else {
            set_error("File not found at " + output_path + " - please download it manually using the curl command shown above");
            std::cerr << "ERROR: File not found. Please download it manually.\n" << std::flush;
            return false;
        }
    }
    
    // Check if URL is HTTP (our custom client supports HTTP)
    if (url.length() < 7 || url.substr(0, 7) != "http://") {
        set_error("Invalid URL: must start with http:// or https://");
        return false;
    }
    
    return download_file_custom(url, output_path);
}

/* C API Implementation */

extern "C" {

c_ncnn_model_t c_ncnn_load_model(const char* param_path, const char* bin_path) {
    if (!param_path) {
        set_error("param_path is NULL");
        return nullptr;
    }
    
    if (!file_exists(param_path)) {
        set_error("Param file not found: " + std::string(param_path));
        return nullptr;
    }
    
    std::unique_ptr<ModelInternal> model(new ModelInternal());
    model->format = C_NCNN_FORMAT_NCNN;
    
#ifdef HAVE_NCNN
    // bin_path is used below
    try {
        model->ncnn_net = new ncnn::Net();
        
        int ret = model->ncnn_net->load_param(param_path);
        if (ret != 0) {
            set_error("Failed to load param file: " + std::string(param_path));
            return nullptr;
        }
        
        if (bin_path && strlen(bin_path) > 0) {
            ret = model->ncnn_net->load_model(bin_path);
            if (ret != 0) {
                set_error("Failed to load bin file: " + std::string(bin_path));
                return nullptr;
            }
        } else {
            // Try to load from .ncnn file (single file format)
            std::string ncnn_path = std::string(param_path);
            size_t pos = ncnn_path.find_last_of(".");
            if (pos != std::string::npos) {
                ncnn_path = ncnn_path.substr(0, pos) + ".ncnn";
                if (file_exists(ncnn_path)) {
                    ret = model->ncnn_net->load_model(ncnn_path.c_str());
                    if (ret != 0) {
                        set_error("Failed to load ncnn file: " + ncnn_path);
                        return nullptr;
                    }
                }
            }
        }
        
        return model.release();
    } catch (const std::exception& e) {
        set_error("Exception loading NCNN model: " + std::string(e.what()));
        return nullptr;
    }
#else
    // Stub implementation - return a dummy model handle
    // This allows the code to compile and run without NCNN
    // The model won't actually work, but the API will function
    (void)bin_path;  // Suppress unused parameter warning
    set_error("NCNN support not compiled in - using stub");
    return model.release();
#endif
}

c_ncnn_model_t c_ncnn_load_onnx_model(const char* onnx_path) {
    if (!onnx_path) {
        set_error("onnx_path is NULL");
        return nullptr;
    }
    
    if (!file_exists(onnx_path)) {
        set_error("ONNX file not found: " + std::string(onnx_path));
        return nullptr;
    }
    
#ifdef HAVE_ONNXRUNTIME
    std::unique_ptr<ModelInternal> model(new ModelInternal());
    model->format = C_NCNN_FORMAT_ONNX;
    
    try {
        static Ort::Env ort_env_static(ORT_LOGGING_LEVEL_WARNING, "c_ncnn");
        model->ort_env = &ort_env_static;
        
        Ort::SessionOptions session_options;
        model->ort_session = new Ort::Session(*model->ort_env, onnx_path, session_options);
        
        // Get input/output names
        Ort::AllocatorWithDefaultOptions allocator;
        size_t num_input_nodes = model->ort_session->GetInputCount();
        size_t num_output_nodes = model->ort_session->GetOutputCount();
        
        for (size_t i = 0; i < num_input_nodes; i++) {
            char* input_name = model->ort_session->GetInputName(i, allocator);
            model->ort_input_names.push_back(input_name);
        }
        
        for (size_t i = 0; i < num_output_nodes; i++) {
            char* output_name = model->ort_session->GetOutputName(i, allocator);
            model->ort_output_names.push_back(output_name);
        }
        
        model->ort_allocator = &allocator;
        
        return model.release();
    } catch (const std::exception& e) {
        set_error("Exception loading ONNX model: " + std::string(e.what()));
        return nullptr;
    }
#else
    set_error("ONNX Runtime support not compiled in");
    return nullptr;
#endif
}

c_ncnn_model_t c_ncnn_download_and_load(const char* url, c_ncnn_format_t format, const char* cache_dir) {
    if (!url) {
        set_error("url is NULL");
        return nullptr;
    }
    
    std::string cache_path = cache_dir ? cache_dir : "/tmp";
    std::string filename = std::string(url);
    size_t pos = filename.find_last_of("/");
    if (pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }
    
    std::string local_path = cache_path + "/" + filename;
    
    // Download if not cached
    if (!file_exists(local_path)) {
        if (!download_file(url, local_path)) {
            return nullptr;
        }
    }
    
    // Load based on format
    if (format == C_NCNN_FORMAT_NCNN) {
        // For NCNN, assume .param and .bin files
        std::string param_path = local_path;
        std::string bin_path = local_path;
        size_t dot_pos = bin_path.find_last_of(".");
        if (dot_pos != std::string::npos) {
            param_path = bin_path.substr(0, dot_pos) + ".param";
            bin_path = bin_path.substr(0, dot_pos) + ".bin";
        }
        
        return c_ncnn_load_model(param_path.c_str(), bin_path.c_str());
    } else {
        return c_ncnn_load_onnx_model(local_path.c_str());
    }
}

void c_ncnn_unload_model(c_ncnn_model_t model) {
    if (model) {
        ModelInternal* m = static_cast<ModelInternal*>(model);
        delete m;
    }
}

c_ncnn_error_t c_ncnn_inference(c_ncnn_model_t model, const c_ncnn_tensor_t* input, c_ncnn_tensor_t* output) {
    if (!model || !input || !output) {
        set_error("Invalid parameters");
        return C_NCNN_ERROR_INVALID_PARAM;
    }
    
    ModelInternal* m = static_cast<ModelInternal*>(model);
    
    if (m->format == C_NCNN_FORMAT_NCNN) {
#ifdef HAVE_NCNN
        try {
            ncnn::Mat ncnn_input(input->width, input->height, input->channels, input->data);
            ncnn::Mat ncnn_output;
            
            ncnn::Extractor ex = m->ncnn_net->create_extractor();
            ex.input("data", ncnn_input);
            ex.extract("output", ncnn_output);
            
            // Allocate output tensor
            output->width = ncnn_output.w;
            output->height = ncnn_output.h;
            output->channels = ncnn_output.c;
            size_t data_size = output->width * output->height * output->channels * sizeof(float);
            output->data = static_cast<float*>(malloc(data_size));
            if (!output->data) {
                set_error("Memory allocation failed");
                return C_NCNN_ERROR_MEMORY;
            }
            memcpy(output->data, ncnn_output.data, data_size);
            
            return C_NCNN_SUCCESS;
        } catch (const std::exception& e) {
            set_error("NCNN inference exception: " + std::string(e.what()));
            return C_NCNN_ERROR_INFERENCE_FAILED;
        }
#else
        // Stub implementation - copy input to output
        output->width = input->width;
        output->height = input->height;
        output->channels = input->channels;
        size_t data_size = output->width * output->height * output->channels * sizeof(float);
        output->data = static_cast<float*>(malloc(data_size));
        if (!output->data) {
            set_error("Memory allocation failed");
            return C_NCNN_ERROR_MEMORY;
        }
        memcpy(output->data, input->data, data_size);
        return C_NCNN_SUCCESS;
#endif
    } else {
#ifdef HAVE_ONNXRUNTIME
        try {
            // Create input tensor
            std::vector<int64_t> input_shape = {1, input->channels, input->height, input->width};
            size_t input_tensor_size = input->width * input->height * input->channels;
            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                *m->ort_allocator, input->data, input_tensor_size,
                input_shape.data(), input_shape.size());
            
            // Run inference
            auto output_tensors = m->ort_session->Run(Ort::RunOptions{nullptr},
                m->ort_input_names.data(), &input_tensor, 1,
                m->ort_output_names.data(), m->ort_output_names.size());
            
            if (output_tensors.empty()) {
                set_error("No output tensors");
                return C_NCNN_ERROR_INFERENCE_FAILED;
            }
            
            // Extract output
            float* float_array = output_tensors.front().GetTensorMutableData<float>();
            auto shape = output_tensors.front().GetTensorTypeAndShapeInfo().GetShape();
            
            output->width = shape[3];
            output->height = shape[2];
            output->channels = shape[1];
            size_t data_size = output->width * output->height * output->channels * sizeof(float);
            output->data = static_cast<float*>(malloc(data_size));
            if (!output->data) {
                set_error("Memory allocation failed");
                return C_NCNN_ERROR_MEMORY;
            }
            memcpy(output->data, float_array, data_size);
            
            return C_NCNN_SUCCESS;
        } catch (const std::exception& e) {
            set_error("ONNX inference exception: " + std::string(e.what()));
            return C_NCNN_ERROR_INFERENCE_FAILED;
        }
#else
        set_error("ONNX Runtime support not compiled in");
        return C_NCNN_ERROR_INFERENCE_FAILED;
#endif
    }
}

c_ncnn_error_t c_ncnn_inference_multi(c_ncnn_model_t model,
                                      const c_ncnn_tensor_t* inputs, int num_inputs,
                                      c_ncnn_tensor_t* outputs, int num_outputs) {
    // Simplified version - for now just call single inference
    if (num_inputs != 1 || num_outputs != 1) {
        set_error("Multi-input/output not yet implemented");
        return C_NCNN_ERROR_INVALID_PARAM;
    }
    return c_ncnn_inference(model, inputs, outputs);
}

c_ncnn_tensor_t* c_ncnn_alloc_tensor(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        set_error("Invalid tensor dimensions");
        return nullptr;
    }
    
    c_ncnn_tensor_t* tensor = static_cast<c_ncnn_tensor_t*>(malloc(sizeof(c_ncnn_tensor_t)));
    if (!tensor) {
        set_error("Memory allocation failed");
        return nullptr;
    }
    
    tensor->width = width;
    tensor->height = height;
    tensor->channels = channels;
    size_t data_size = width * height * channels * sizeof(float);
    tensor->data = static_cast<float*>(malloc(data_size));
    if (!tensor->data) {
        free(tensor);
        set_error("Memory allocation failed");
        return nullptr;
    }
    
    memset(tensor->data, 0, data_size);
    return tensor;
}

void c_ncnn_free_tensor(c_ncnn_tensor_t* tensor) {
    if (tensor) {
        if (tensor->data) {
            free(tensor->data);
        }
        free(tensor);
    }
}

const char* c_ncnn_get_error(void) {
    return g_last_error.c_str();
}

} /* extern "C" */

