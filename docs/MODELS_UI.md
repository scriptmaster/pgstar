# Models UI Endpoint

## Overview

The `/v1/ui/models` endpoint provides a web interface for searching and browsing NCNN models from multiple sources.

## Endpoint

- **URL**: `/v1/ui/models`
- **Method**: `GET`
- **Query Parameters**:
  - `q` (optional): Search query string

## Features

### 1. Search Interface
- Clean, responsive UI using Pico CSS
- Search form with query parameter support
- Real-time model search results

### 2. Model Sources

#### Hugging Face (Primary)
- Searches Hugging Face API for NCNN models
- Filters by NCNN format
- Sorts by download count
- Shows model metadata (name, description, downloads, files)

#### GitHub Repositories
The UI also includes links to popular GitHub repositories with NCNN models:
- **Baiyuetribe/ncnn-models**: Collection of AI models compatible with NCNN
- **TNTwise/Universal-NCNN-Upscaler**: ESRGAN and Real-ESRGAN models
- **EdVince/Stable-Diffusion-NCNN**: Stable Diffusion using NCNN framework
- **nihui/waifu2x-ncnn-vulkan**: waifu2x image upscaler

### 3. Model Information Display
Each model card shows:
- Model name with source badge
- Description
- Download count (for Hugging Face models)
- Available files (.param and .bin files)
- Links to view details
- Download information button

## Usage Examples

### Basic Access
```bash
curl http://localhost:5480/v1/ui/models
```

### Search for Models
```bash
curl "http://localhost:5480/v1/ui/models?q=image"
```

### In Browser
Open in your browser:
```
http://localhost:5480/v1/ui/models
http://localhost:5480/v1/ui/models?q=ncnn
```

## Implementation Details

### Frontend
- Uses Pico CSS v2 for styling
- Client-side JavaScript for API calls
- Responsive design with grid layout
- Model cards with badges for source identification

### Backend
- C endpoint handler: `pgstar_route_v1_ui_models()`
- HTML generation using StringInfo
- Query parameter parsing
- Static HTML with embedded JavaScript

### API Integration
- Hugging Face API: `https://huggingface.co/api/models`
- Search parameters:
  - `search`: Query string
  - `filter`: ncnn
  - `sort`: downloads
  - `direction`: -1 (descending)
  - `limit`: 20

## Model Sources

### Primary Sources

1. **Hugging Face**
   - Largest collection of NCNN models
   - API-based search
   - Rich metadata
   - Direct file access

2. **GitHub Repositories**
   - Community-maintained collections
   - Specialized model types
   - Conversion tools and examples

### Other Potential Sources

- **Model Zoo**: Official model repositories
- **Academic Repositories**: Research model collections
- **Commercial Providers**: Enterprise model catalogs

## Future Enhancements

- Direct download integration
- Model preview/visualization
- Filter by model type (image, text, etc.)
- Model comparison features
- Caching of search results
- Bookmarking favorite models

## Notes

- The endpoint returns HTML, not JSON
- Search is performed client-side via JavaScript
- Hugging Face API calls are made directly from the browser
- CORS may need to be configured for API access
- GitHub sources are static links (not API-based)

