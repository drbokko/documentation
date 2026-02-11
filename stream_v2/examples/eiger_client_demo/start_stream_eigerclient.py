#! /usr/bin/env python3
"""
EIGER Detector Stream Acquisition Script with Custom Flatfield Correction

This script allows you to:
1. Configure EIGER detector for streaming data acquisition
2. Apply custom flatfield corrections from numpy arrays

To use custom flatfield correction:
1. Set "use_custom_flatfield": True in configuration
2. Either:
   - Provide path to .npy file: "flatfield_file": "/path/to/your/flatfield.npy"
   - Let script create example flatfield (replace with your calibration data)
3. The flatfield array should match detector geometry (y_pixels, x_pixels)

Example flatfield creation:
    flatfield = np.load("my_flatfield_calibration.npy")
    np.save("flatfield_correction.npy", flatfield)
"""

import json
import numpy as np
import base64
import logging
from DEigerClient import DEigerClient
from datetime import datetime
from pathlib import Path
import os
import time

logging.basicConfig(level=logging.INFO, format='%(asctime)s-%(name)s-%(levelname)s: %(message)s')


if __name__ == '__main__':
            
    ################
    ## USER INPUT ##
    ################
    configuration = {
        # Detector IP and Initialization
        "DCU_IP": '172.31.1.1',  # IP of the detector control unit
        "force_initialization": False,  # Force initialization if needed

        # File & Naming Settings
        "working_directory": "/dev/shm",
        "measurement_name": f'stream_test_eigerclient',
        "image_name": 'img',
        "acquisition_time": datetime.now().strftime('%y%m%d_%H%M%S'),
        # "acquisition_time": "20250430_120924", #  for some manual download of specific h5 files

        # Data acquisition parameters
        "thresholds": [45000],  # Energy thresholds [eV], insert 1 or 2 values
        "number_of_images": 6000,  # Number of images to capture
        "exposure_time": 1/1000,  # Exposure time per image [s]
        "sleep_time": 0.0,  # Delay between frames [s]
        
        # Data Acquisition Interfaces
        "monitor": "disabled", #works only with 1 threshold, slow speed     
        "filewriter2": "disabled",
        "stream": "enabled",
        "save_raw": False,  # Save raw data if using filewriter2, otherwise images are saved as tiff
        "nimages_per_file": 100,  # Max images per HDF5 file
        
        # Flatfield Correction Settings
        "use_custom_flatfield": False,  # Enable custom flatfield correction
        "flatfield_file": None,  # Path to .npy file (if None, will create example flatfield)
        # Example: "flatfield_file": "/path/to/your/flatfield_correction.npy",
        "flatfield_save_reference": True,  # Save the flatfield array for reference
    }

    # Construct full output directory path
    configuration["full_path"] = Path(configuration["working_directory"], f"{configuration['acquisition_time']}_{configuration['measurement_name']}")
    os.makedirs(configuration["full_path"], exist_ok=True)  # Ensure directory exists

    # Save configuration to JSON

    # Connect to Detector
    logging.info(f"Connecting to detector at {configuration['DCU_IP']}...")
    c = DEigerClient.DEigerClient(host=configuration["DCU_IP"])
    logging.info(f"Detector status: {c.detectorStatus('state')['value']}")
    # c.sendFileWriterCommand('clear')
    
    c.sendDetectorCommand("disarm")  # Writes all data to file and disarms the trigger unit.
    logging.info("Detector disarmed - acquisition complete")

    if configuration["force_initialization"] or c.detectorStatus('state')['value'] != 'idle':
        logging.info("Initializing detector...")
        c.sendDetectorCommand("initialize")

    logging.info(f"Detector high_voltage status: {c.detectorStatus('high_voltage/state')['value']}")
    logging.info(f"Detector temperature: {c.detectorStatus('temperature')['value']:.1f} deg")
    logging.info(f"Detector humidity: {c.detectorStatus('humidity')['value']:.1f} %")

    # Configure Detector
    logging.info("Configuring detector settings...")
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True) #pixels marked in the pixel_mask will be set to zero
    c.setDetectorConfig("test_image_mode", "")
    # c.setDetectorConfig("test_image_value", 100)
    logging.debug("Basic detector configuration complete")
      # Handle Custom Flatfield Correction
    if configuration["use_custom_flatfield"]:
        # ToDo: Implement custom flatfield correction
        pass
    else:
        c.setDetectorConfig("flatfield_correction_applied", False)
        logging.info("Flatfield correction disabled")

    # c.setDetectorConfig("photon_energy",10000) # set for example to 80,0000 eV, only for monochromatic

    # Set Thresholds
    logging.info(f"Configuring {len(configuration['thresholds'])} threshold(s): {configuration['thresholds']}")
    for i, th in enumerate(configuration["thresholds"], start=1):
        c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        c.setDetectorConfig(f"threshold/{i}/energy", th)
        logging.info(f"Threshold {i}: {th} eV")

    # Disable additional thresholds if not provided
    if len(configuration["thresholds"]) < 2:
        c.setDetectorConfig("threshold/2/mode", 'disabled')
        logging.debug("Threshold 2: disabled")

    # Set Acquisition Parameters
    logging.info("Setting acquisition parameters...")
    c.setDetectorConfig("count_time", configuration["exposure_time"])
    c.setDetectorConfig('frame_time', configuration["exposure_time"] + configuration["sleep_time"])
    c.setDetectorConfig("nimages", configuration["number_of_images"])
    c.setDetectorConfig('auto_summation', False)

    logging.info(f'count_time: {c.detectorConfig("count_time")["value"]} s')
    logging.info(f'frame_time: {c.detectorConfig("frame_time")["value"]} s')
    logging.info(f'number_of_images: {configuration["number_of_images"]}')

    # Configure Data Acquisition Interfaces
    logging.info("Configuring data acquisition interfaces...")
    c.setMonitorConfig("mode", configuration["monitor"])
    logging.info(f"Monitor mode: {configuration['monitor']}")
    
    c.setFileWriterConfig('mode', configuration["filewriter2"])
    logging.info(f"FileWriter mode: {configuration['filewriter2']}")

    if configuration["filewriter2"] == "enabled":
        c.setFileWriterConfig('name_pattern', f"{configuration['acquisition_time']}_{configuration['measurement_name']}")
        c.setFileWriterConfig("nimages_per_file", configuration["nimages_per_file"])
        c.setFileWriterConfig('compression_enabled', False)
        c.setFileWriterConfig('format', 'hdf5 nexus v2024.2 nxmx')
        logging.info(f"FileWriter configured with {configuration['nimages_per_file']} images per file")

    c.setStreamConfig('mode', configuration["stream"])
    c.setStreamConfig('format', 'cbor')
    c.setStreamConfig('header_detail', 'all')
    logging.info(f"Stream mode: {configuration['stream']}, format: cbor")

    # Run Acquisition
    logging.info("Starting data acquisition...")
    c.sendDetectorCommand('arm')
    logging.info("Detector armed")
    logging.info("Acquisition triggered")
    c.sendDetectorCommand('trigger')
    c.sendDetectorCommand("disarm")  # Writes all data to file and disarms the trigger unit.
    logging.info("Detector disarmed - acquisition complete")

