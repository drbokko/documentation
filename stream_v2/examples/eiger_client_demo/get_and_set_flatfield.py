#!/usr/bin/env python3
"""
Example script to get flatfield from EIGER detector and re-upload it for both thresholds.
Demonstrates the full round-trip process of downloading and uploading flatfield corrections.
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

def encode_darray(numpy_array, darray_version=[1, 0, 0]):
    """
    Encode numpy array to darray format used by EIGER detector.
    """
    # Determine data type string
    dtype_map = {
        np.dtype('<f4'): '<f4',  # float32 little endian
        np.dtype('<u4'): '<u4',  # uint32 little endian
        np.dtype('float32'): '<f4',
        np.dtype('uint32'): '<u4'
    }
    
    # Ensure array is in correct format
    if numpy_array.dtype not in [np.dtype('<f4'), np.dtype('<u4')]:
        numpy_array = numpy_array.astype('<f4')
    
    dtype_str = dtype_map.get(numpy_array.dtype, '<f4')
    
    # Get shape in [width, height] format (flip from numpy's [height, width])
    shape = [numpy_array.shape[1], numpy_array.shape[0]]
    
    # Convert to binary and encode in base64
    binary_data = numpy_array.tobytes()
    base64_data = base64.b64encode(binary_data).decode('ascii')
    
    # Create darray dictionary
    darray_dict = {
        "__darray__": darray_version,
        "type": dtype_str,
        "shape": shape,
        "filters": ["base64"],
        "data": base64_data
    }
    
    logging.debug(f"Encoded darray: type={dtype_str}, shape={shape}, size={len(base64_data)} chars")
    
    return darray_dict

def get_flatfield_from_detector(client):
    """Get current flatfield from detector."""
    logging.info("Getting current flatfield...")
    
    try:
        flatfield_response = client.detectorConfig("flatfield")
        flatfield_data = flatfield_response["value"]
        
        if flatfield_data is not None and isinstance(flatfield_data, dict):
            if "__darray__" in flatfield_data:
                logging.info("Found darray format flatfield")
                flatfield_array = decode_darray(flatfield_data)
                logging.info(f"Retrieved flatfield shape: {flatfield_array.shape}")
                logging.info(f"Range: {flatfield_array.min():.4f} to {flatfield_array.max():.4f}")
                return flatfield_array
            else:
                logging.warning("Flatfield data is not in darray format")
                return None
        else:
            logging.error("No flatfield data available")
            return None
            
    except Exception as e:
        logging.error(f"Error getting flatfield: {e}")
        return None

def set_flatfield_to_detector(client, flatfield_array):
    """Upload flatfield to detector."""
    logging.info("Setting flatfield to detector...")
    
    try:
        # Encode to darray format
        logging.debug("Encoding to darray format...")
        flatfield_darray = encode_darray(flatfield_array)
        
        # Disable flatfield first
        logging.debug("Disabling flatfield correction...")
        client.setDetectorConfig("flatfield_correction_applied", False)
        
        # Upload the flatfield
        logging.debug("Uploading flatfield data...")
        client.setDetectorConfig("flatfield", flatfield_darray)
        
        # Enable flatfield correction
        logging.debug("Enabling flatfield correction...")
        client.setDetectorConfig("flatfield_correction_applied", True)
        
        logging.info("Flatfield uploaded and enabled successfully")
        return True
        
    except Exception as e:
        logging.error(f"Error setting flatfield: {e}")
        return False

def configure_thresholds(client, thresholds):
    """Configure detector thresholds."""
    logging.info(f"Configuring thresholds: {thresholds}")
    
    # Set thresholds
    for i, threshold_energy in enumerate(thresholds, start=1):
        client.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        client.setDetectorConfig(f"threshold/{i}/energy", threshold_energy)
        logging.info(f"Threshold {i}: {threshold_energy} eV")
    
    # Disable additional thresholds if not provided
    if len(thresholds) < 2:
        client.setDetectorConfig("threshold/2/mode", 'disabled')
        logging.info("Threshold 2: disabled")

# Configuration
DCU_IP = 'dev-si-e2dcu-01'
THRESHOLDS = [13000, 30000]  # Two thresholds: 13 keV and 30 keV

logging.info("=" * 60)
logging.info("EIGER Detector Flatfield Get & Set Example")
logging.info("=" * 60)

# Connect to detector
logging.info(f"Connecting to {DCU_IP}...")
c = DEigerClient.DEigerClient(host=DCU_IP)
logging.info(f"Connected - Detector status: {c.detectorStatus('state')['value']}")

# Configure both thresholds
configure_thresholds(c, THRESHOLDS)

# Get detector geometry
try:
    x_pixels = c.detectorConfig("x_pixels_in_detector")["value"]
    y_pixels = c.detectorConfig("y_pixels_in_detector")["value"]
    logging.info(f"Detector geometry: {x_pixels} x {y_pixels} pixels")
except Exception as e:
    logging.warning(f"Could not get detector geometry: {e}")

logging.info("="*60)
logging.info("STEP 1: Getting current flatfield from detector")
logging.info("="*60)

# Get current flatfield
original_flatfield = get_flatfield_from_detector(c)

if original_flatfield is not None:
    # Save original for reference
    np.save("original_flatfield.npy", original_flatfield)
    logging.info("Original flatfield saved as 'original_flatfield.npy'")
    
    logging.info("="*60)
    logging.info("STEP 2: Re-uploading flatfield for both thresholds")
    logging.info("="*60)
    
    # Test 1: Re-upload the same flatfield
    logging.info("Test 1: Re-uploading original flatfield...")
    success1 = set_flatfield_to_detector(c, original_flatfield)
    
    if success1:
        # Verify it was uploaded correctly
        logging.info("Verifying uploaded flatfield...")
        retrieved_flatfield = get_flatfield_from_detector(c)
        
        if retrieved_flatfield is not None:
            # Compare arrays
            if np.allclose(original_flatfield, retrieved_flatfield, rtol=1e-6):
                logging.info("Verification successful: uploaded flatfield matches original")
            else:
                logging.warning("uploaded flatfield differs from original")
                logging.info(f"Max difference: {np.max(np.abs(original_flatfield - retrieved_flatfield))}")
    
    logging.info("="*60)
    logging.info("STEP 3: Testing with modified flatfield")
    logging.info("="*60)
    
    # Test 2: Create and upload a slightly modified flatfield
    logging.info("Test 2: Creating modified flatfield...")
    modified_flatfield = original_flatfield.copy()
    
    # Add small random variations (±2% of original values)
    rng = np.random.default_rng(42)
    noise = rng.uniform(-0.50, 0.50, size=modified_flatfield.shape)
    modified_flatfield = modified_flatfield * (1.0 + noise)
    
    # Ensure reasonable values
    modified_flatfield = np.clip(modified_flatfield, 0.1, 10.0)
    
    logging.info(f"Modified flatfield range: {modified_flatfield.min():.4f} to {modified_flatfield.max():.4f}")
    logging.info(f"Mean change: {np.mean(modified_flatfield - original_flatfield):.6f}")
    
    # Upload modified flatfield
    success2 = set_flatfield_to_detector(c, modified_flatfield)
    
    if success2:
        # Save modified flatfield
        np.save("modified_flatfield.npy", modified_flatfield)
        logging.info("Modified flatfield saved as 'modified_flatfield.npy'")
        
        # Verify
        final_flatfield = get_flatfield_from_detector(c)
        if final_flatfield is not None:
            if np.allclose(modified_flatfield, final_flatfield, rtol=1e-6):
                logging.info("Modified flatfield uploaded and verified successfully")
            else:
                logging.warning("retrieved flatfield differs from uploaded version")
    
    logging.info("="*60)
    logging.info("SUMMARY")
    logging.info("="*60)
    logging.info(f"Both thresholds configured: {THRESHOLDS}")
    logging.info(f"Original flatfield retrieved and saved")
    logging.info(f"Flatfield round-trip test: {'PASSED' if success1 else 'FAILED'}")
    logging.info(f"Modified flatfield test: {'PASSED' if success2 else 'FAILED'}")
    logging.info("Files created:")
    logging.info("- original_flatfield.npy: Original detector flatfield")
    if success2:
        logging.info("- modified_flatfield.npy: Modified flatfield with random variations")
    
else:
    logging.error("="*60)
    logging.error("CANNOT CONTINUE - No flatfield available from detector")
    logging.error("="*60)
    logging.error("Make sure:")
    logging.error("1. Detector has a flatfield correction configured")  
    logging.error("2. Detector is properly connected and initialized")
    logging.error("3. Flatfield correction is enabled")

logging.info("="*60)