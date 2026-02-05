#!/usr/bin/env python3
"""
Minimal script to get flatfield from EIGER detector and save as .npy file
Handles darray format with base64 encoding
"""

import numpy as np
import base64
import logging
from DEigerClient import DEigerClient

logging.basicConfig(level=logging.INFO, format='%(levelname)-8s - %(asctime)s - %(message)s')

def decode_darray(darray_dict):
    """
    Decode darray format used by EIGER detector.
    Format: {"__darray__": version, "type": dtype, "shape": [w,h], "filters": ["base64"], "data": base64_data}
    """
    # Extract metadata
    dtype_str = darray_dict["type"]  # e.g., "<f4" or "<u4" 
    shape = darray_dict["shape"]     # [width, height]
    base64_data = darray_dict["data"]
    
    logging.debug(f"darray type: {dtype_str}")
    logging.debug(f"darray shape: {shape}")
    logging.debug(f"darray version: {darray_dict.get('__darray__', 'unknown')}")
    
    # Decode base64 data
    binary_data = base64.b64decode(base64_data)
    logging.debug(f"decoded {len(binary_data)} bytes")
    
    # Convert to numpy array with correct dtype and shape
    numpy_array = np.frombuffer(binary_data, dtype=dtype_str).reshape(shape[::-1])  # Note: shape is [w,h] but numpy uses [h,w]
    
    return numpy_array

# Configuration
DCU_IP = 'dev-si-e2dcu-01'  # Change this to your detector IP
OUTPUT_FILE = 'detector_flatfield.npy'

# Connect and retrieve flatfield
logging.info(f"Connecting to {DCU_IP}...")
c = DEigerClient.DEigerClient(host=DCU_IP)

logging.info("Getting flatfield...")
flatfield_response = c.detectorConfig("flatfield")
flatfield_data = flatfield_response["value"]

if flatfield_data is not None and isinstance(flatfield_data, dict):
    if "__darray__" in flatfield_data:
        logging.info("Found darray format flatfield")
        try:
            # Decode the darray format
            flatfield_array = decode_darray(flatfield_data)
            
            # Save to file
            np.save(OUTPUT_FILE, flatfield_array)
            
            logging.info(f"Flatfield saved to {OUTPUT_FILE}")
            logging.info(f"Final shape: {flatfield_array.shape}")
            logging.info(f"Data type: {flatfield_array.dtype}")
            logging.info(f"Range: {flatfield_array.min():.4f} to {flatfield_array.max():.4f}")
            
        except Exception as e:
            logging.error(f"Error decoding darray: {e}")
    else:
        logging.warning("Flatfield data is not in darray format")
        logging.info(f"Available keys: {list(flatfield_data.keys())}")
else:
    logging.error("No flatfield data available or wrong format")   