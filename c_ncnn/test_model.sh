#!/bin/bash
# Simple test script for c_ncnn library
# This script demonstrates how to download and use a lightweight NCNN model

set -e

MODEL_DIR="/tmp/pgstar_models"
mkdir -p "$MODEL_DIR"

echo "Testing c_ncnn library..."
echo "Model directory: $MODEL_DIR"

# Example: Download a lightweight model (you would replace this with an actual model URL)
# For now, we'll create dummy files for testing
echo "Creating test model files..."

# Create dummy .param file
cat > "$MODEL_DIR/test_model.param" << 'EOF'
7767517
75 83
Input            data                     0 1 data 0=224 1=224 2=3
EOF

# Create dummy .bin file (just a placeholder)
touch "$MODEL_DIR/test_model.bin"

echo "Test model files created at:"
echo "  $MODEL_DIR/test_model.param"
echo "  $MODEL_DIR/test_model.bin"

echo ""
echo "To use with pgstar:"
echo "  mkdir -p /opt/pgstar/models"
echo "  cp $MODEL_DIR/test_model.param /opt/pgstar/models/pgstar_model.param"
echo "  cp $MODEL_DIR/test_model.bin /opt/pgstar/models/pgstar_model.bin"
echo ""
echo "Then test the endpoint:"
echo "  curl -X POST http://localhost:5481/v1/chat/completions \\"
echo "    -H 'Content-Type: application/json' \\"
echo "    -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}]}'"


