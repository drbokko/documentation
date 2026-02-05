#!/usr/bin/env python3
"""
Simple script to get and re-upload flatfield for both thresholds
"""

import numpy as np
import base64
import logging
from DEigerClient import DEigerClient

logging.basicConfig(level=logging.INFO, format='%(levelname)-8s - %(asctime)s - %(message)s')

def decode_darray(darray_dict):
    """Decode darray format to numpy array."""
    dtype_str = darray_dict["type"]
    shape = darray_dict["shape"]
    base64_data = darray_dict["data"]
    
    binary_data = base64.b64decode(base64_data)
    numpy_array = np.frombuffer(binary_data, dtype=dtype_str).reshape(shape[::-1])
    return numpy_array

def encode_darray(numpy_array):
    """Encode numpy array to darray format."""
    if numpy_array.dtype != np.dtype('<f4'):
        numpy_array = numpy_array.astype('<f4')
    
    shape = [numpy_array.shape[1], numpy_array.shape[0]]  # [width, height]
    binary_data = numpy_array.tobytes()
    base64_data = base64.b64encode(binary_data).decode('ascii')
    
    return {
        "__darray__": [1, 0, 0],
        "type": "<f4",
        "shape": shape,
        "filters": ["base64"],
        "data": base64_data
    }

# Configuration
DCU_IP = 'dev-si-e2dcu-01'
THRESHOLDS = [13000, 30000]

logging.info(f"Connecting to {DCU_IP}...")
c = DEigerClient.DEigerClient(host=DCU_IP)

# Configure both thresholds
logging.info("Setting thresholds...")
for i, threshold in enumerate(THRESHOLDS, start=1):
    c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
    c.setDetectorConfig(f"threshold/{i}/energy", threshold)
    logging.info(f"Threshold {i}: {threshold} eV")

# Get current flatfield
logging.info("Getting current flatfield...")
flatfield_data = c.detectorConfig("flatfield")["value"]

if flatfield_data and "__darray__" in flatfield_data:
    # Decode current flatfield
    flatfield_array = decode_darray(flatfield_data)
    logging.info(f"Got flatfield: shape {flatfield_array.shape}, range {flatfield_array.min():.3f}-{flatfield_array.max():.3f}")
    
    # Re-encode and upload
    logging.info("Re-uploading flatfield...")
    c.setDetectorConfig("flatfield_correction_applied", False)
    
    encoded_flatfield = encode_darray(flatfield_array)
    c.setDetectorConfig("flatfield", encoded_flatfield)
    
    c.setDetectorConfig("flatfield_correction_applied", True)
    logging.info("Flatfield re-uploaded successfully")
    
    # Save for reference
    np.save("roundtrip_flatfield.npy", flatfield_array)
    logging.info("Saved as roundtrip_flatfield.npy")
    
else:
    logging.error("No flatfield data available or wrong format")