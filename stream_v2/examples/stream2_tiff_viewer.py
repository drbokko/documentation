#!/usr/bin/env python3
"""
stream2_tiff_viewer.py - TIFF viewer for Stream V2 detector images

Features:
- 16-32 bit monochromatic TIFF support
- Dynamic range adjustment (min/max)
- Histogram-based range selection
- Color palette/gradient selection
- Series navigation (movie-like scrolling)
- Metadata display (start_time, image_id, series_id)
"""

import sys
import os
import glob
import re
from pathlib import Path
from typing import Optional, List, Tuple
import numpy as np
from PIL import Image
import json


# Try PyQt5 first, then PyQt6, then fallback to tkinter
try:
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QSlider, QPushButton, QComboBox, QFileDialog, QSpinBox,
        QDoubleSpinBox, QGroupBox, QMessageBox, QScrollArea
    )
    from PyQt5.QtCore import Qt, QTimer, pyqtSignal, QObject
    from PyQt5.QtGui import QImage, QPixmap, QPalette, QColor, QPainter
    # PyQt5 compatibility
    from PyQt5.QtGui import QPainter as QPainter5
    AlignCenter = Qt.AlignCenter
    KeepAspectRatio = Qt.KeepAspectRatio
    SmoothTransformation = Qt.SmoothTransformation
    Antialiasing = QPainter5.Antialiasing
    Format_RGB888 = QImage.Format_RGB888
    QT_VERSION = 5
except ImportError:
    try:
        from PyQt6.QtWidgets import (
            QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
            QLabel, QSlider, QPushButton, QComboBox, QFileDialog, QSpinBox,
            QDoubleSpinBox, QGroupBox, QMessageBox, QScrollArea
        )
        from PyQt6.QtCore import Qt, QTimer, pyqtSignal, QObject
        from PyQt6.QtGui import QImage, QPixmap, QPalette, QColor, QPainter
        # PyQt6 compatibility: constants moved to enum classes
        AlignCenter = Qt.AlignmentFlag.AlignCenter
        KeepAspectRatio = Qt.AspectRatioMode.KeepAspectRatio
        SmoothTransformation = Qt.TransformationMode.SmoothTransformation
        Antialiasing = QPainter.RenderHint.Antialiasing
        Format_RGB888 = QImage.Format.Format_RGB888
        QT_VERSION = 6
    except ImportError:
        print("Error: PyQt5 or PyQt6 is required.")
        print("Install with: pip install PyQt5")
        print("Or: pip install PyQt6")
        print("Note: You can skip optional dependencies with: pip install --no-deps")
        sys.exit(1)


class SeriesMetadata:
    """Store metadata for a series"""
    def __init__(self):
        self.start_times = {}  # image_id -> start_time (seconds)
        self.series_id = None
        self.channel = None


class ImageSeries:
    """Manages a series of TIFF images"""
    def __init__(self, base_dir: str):
        self.base_dir = Path(base_dir)
        self.series_id = None
        self.channel = None
        self.image_files = []
        self.metadata = SeriesMetadata()
        self._current_img = None  # Store current image for metadata access
        self._load_series()
    
    def _load_series(self):
        """Load all TIFF files from the series directory"""
        # Find all TIFF files in the directory
        pattern = str(self.base_dir / "stream2_*.tiff")
        files = sorted(glob.glob(pattern))
        
        if not files:
            return
        
        # Extract series_id and channel from first file
        first_file = Path(files[0])
        match = re.search(r'serie_(\d+)', str(first_file.parent))
        if match:
            self.series_id = int(match.group(1))
        
        match = re.search(r'stream2_([^_]+)_(\d+)\.tiff', first_file.name)
        if match:
            self.channel = match.group(1)
        
        # Sort by image_id
        def get_image_id(fpath):
            match = re.search(r'stream2_[^_]+_(\d+)\.tiff', Path(fpath).name)
            return int(match.group(1)) if match else 0
        
        self.image_files = sorted(files, key=get_image_id)
        
        # Try to load metadata JSON if it exists
        metadata_file = self.base_dir / "metadata.json"
        if metadata_file.exists():
            try:
                with open(metadata_file, 'r') as f:
                    data = json.load(f)
                    self.metadata.start_times = data.get('start_times', {})
                    self.metadata.series_id = data.get('series_id', self.series_id)
                    self.metadata.channel = data.get('channel', self.channel)
            except Exception as e:
                print(f"Warning: Could not load metadata: {e}")
    
    def __len__(self):
        return len(self.image_files)
    
    def get_image(self, index: int) -> Optional[np.ndarray]:
        """Load image at index as numpy array"""
        if index < 0 or index >= len(self.image_files):
            return None
        try:
            img = Image.open(self.image_files[index])
            # Store image object for metadata access
            self._current_img = img
            return np.array(img, dtype=np.float32)
        except Exception as e:
            print(f"Error loading image {index}: {e}")
            return None
    
    def get_image_metadata(self, index: int) -> dict:
        """Get TIFF metadata tags for image at index"""
        if index < 0 or index >= len(self.image_files):
            return {}
        try:
            img = Image.open(self.image_files[index])
            # Get all TIFF tags - PIL uses different attribute names in different versions
            tags = {}
            
            # Try tag_v2 (Pillow 8.0+)
            if hasattr(img, 'tag_v2'):
                tags = dict(img.tag_v2)
            
            # Try tag (older Pillow)
            if hasattr(img, 'tag') and not tags:
                tags = dict(img.tag)
            
            # Try _getexif (deprecated but still works)
            if hasattr(img, '_getexif') and not tags:
                try:
                    exif = img._getexif()
                    if exif:
                        tags = dict(exif)
                except:
                    pass
            
            # Also try to get custom tags via getexif() (PIL 8.0+)
            if hasattr(img, 'getexif'):
                try:
                    exif = img.getexif()
                    if exif:
                        for tag_id, value in exif.items():
                            tags[tag_id] = value
                except:
                    pass
            
            img.close()  # Close to free resources
            return tags
        except Exception as e:
            print(f"Error reading metadata for image {index}: {e}")
            return {}
    
    def get_image_id(self, index: int) -> Optional[int]:
        """Get image_id for image at index"""
        if index < 0 or index >= len(self.image_files):
            return None
        match = re.search(r'stream2_[^_]+_(\d+)\.tiff', Path(self.image_files[index]).name)
        return int(match.group(1)) if match else None
    
    def get_start_time(self, index: int) -> Optional[float]:
        """Get start_time for image at index from TIFF metadata or JSON fallback"""
        # First try to get from TIFF metadata
        tiff_metadata = self.get_image_metadata(index)
        start_time = None
        
        # Try to find start_time in TIFF tags
        # Common approaches:
        # 1. Check ImageDescription tag (270) for text containing start_time
        # 2. Check custom/private tags (typically 33000+)
        # 3. Check if there's a custom tag with start_time
        
        # Try ImageDescription tag (270)
        if 270 in tiff_metadata:
            desc = tiff_metadata[270]
            if isinstance(desc, (list, tuple)) and len(desc) > 0:
                desc_str = desc[0] if isinstance(desc[0], str) else str(desc[0])
                # Try to extract start_time from description (JSON or key=value format)
                import re
                # Look for JSON format: "start_time": 0.123
                json_match = re.search(r'"start_time"\s*:\s*([0-9.]+)', desc_str)
                if json_match:
                    try:
                        start_time = float(json_match.group(1))
                    except:
                        pass
                # Look for key=value format: start_time=0.123
                if start_time is None:
                    kv_match = re.search(r'start_time[:\s=]+([0-9.]+)', desc_str, re.IGNORECASE)
                    if kv_match:
                        try:
                            start_time = float(kv_match.group(1))
                        except:
                            pass
        
        # Try custom/private tags (33000-65535 are private tags)
        # Common custom tag ranges: 33000-34999, 40000-65535
        for tag_id in range(33000, 34000):
            if tag_id in tiff_metadata:
                tag_value = tiff_metadata[tag_id]
                try:
                    if isinstance(tag_value, (int, float)):
                        start_time = float(tag_value)
                        break
                    elif isinstance(tag_value, (list, tuple)) and len(tag_value) > 0:
                        start_time = float(tag_value[0])
                        break
                    elif isinstance(tag_value, str):
                        # Try to parse as float
                        start_time = float(tag_value)
                        break
                except:
                    pass
        
        # Fallback to JSON metadata if TIFF metadata doesn't have it
        if start_time is None:
            image_id = self.get_image_id(index)
            if image_id is not None:
                start_time = self.metadata.start_times.get(image_id)
        
        return start_time


class ColorPalette:
    """Color palette/gradient generators (vectorized for performance)"""
    @staticmethod
    def grayscale(values) -> np.ndarray:
        """Standard grayscale - vectorized"""
        v = np.clip(values * 255, 0, 255).astype(np.uint8)
        result = np.stack([v, v, v], axis=-1)
        return result
    
    @staticmethod
    def hot(values) -> np.ndarray:
        """Hot colormap (black -> red -> yellow -> white) - vectorized"""
        values = np.clip(values, 0, 1)
        r = np.zeros_like(values)
        g = np.zeros_like(values)
        b = np.zeros_like(values)
        
        mask1 = values < 0.33
        r[mask1] = values[mask1] * 3 * 255
        
        mask2 = (values >= 0.33) & (values < 0.67)
        r[mask2] = 255
        g[mask2] = (values[mask2] - 0.33) * 3 * 255
        
        mask3 = values >= 0.67
        r[mask3] = 255
        g[mask3] = 255
        b[mask3] = (values[mask3] - 0.67) * 3 * 255
        
        result = np.stack([r, g, b], axis=-1).astype(np.uint8)
        return result
    
    @staticmethod
    def cool(values) -> np.ndarray:
        """Cool colormap (cyan -> magenta) - vectorized"""
        values = np.clip(values, 0, 1)
        r = (values * 255).astype(np.uint8)
        g = ((1 - values) * 255).astype(np.uint8)
        b = np.full_like(r, 255, dtype=np.uint8)
        result = np.stack([r, g, b], axis=-1)
        return result
    
    @staticmethod
    def jet(values) -> np.ndarray:
        """Jet colormap (blue -> cyan -> yellow -> red) - vectorized"""
        values = np.clip(values, 0, 1)
        r = np.zeros_like(values)
        g = np.zeros_like(values)
        b = np.zeros_like(values)
        
        mask1 = values < 0.125
        b[mask1] = (0.5 + values[mask1] * 4) * 255
        
        mask2 = (values >= 0.125) & (values < 0.375)
        g[mask2] = (values[mask2] - 0.125) * 4 * 255
        b[mask2] = 255
        
        mask3 = (values >= 0.375) & (values < 0.625)
        r[mask3] = (values[mask3] - 0.375) * 4 * 255
        g[mask3] = 255
        b[mask3] = (0.625 - values[mask3]) * 4 * 255
        
        mask4 = (values >= 0.625) & (values < 0.875)
        r[mask4] = 255
        g[mask4] = (0.875 - values[mask4]) * 4 * 255
        
        mask5 = values >= 0.875
        r[mask5] = (255 - (values[mask5] - 0.875) * 4 * 127.5).astype(np.uint8)
        
        result = np.stack([r, g, b], axis=-1).astype(np.uint8)
        return result
    
    @staticmethod
    def viridis(values) -> np.ndarray:
        """Viridis colormap approximation - vectorized"""
        values = np.clip(values, 0, 1)
        r = (68 + (252 - 68) * values).astype(np.uint8)
        g = (1 + (253 - 1) * values).astype(np.uint8)
        b = (84 + (231 - 84) * values).astype(np.uint8)
        result = np.stack([r, g, b], axis=-1)
        return result
    
    PALETTES = {
        "Grayscale": grayscale,
        "Hot": hot,
        "Cool": cool,
        "Jet": jet,
        "Viridis": viridis,
    }


class ZoomableLabel(QLabel):
    """QLabel with mouse wheel zoom support"""
    zoom_requested = pyqtSignal(float)  # Signal with zoom delta
    
    def wheelEvent(self, event):
        """Handle mouse wheel events"""
        if QT_VERSION == 6:
            delta = event.angleDelta().y()
        else:
            delta = event.angleDelta().y()
        
        # Emit zoom signal (positive = zoom in, negative = zoom out)
        zoom_delta = 0.1 if delta > 0 else -0.1
        self.zoom_requested.emit(zoom_delta)


class TimelineWidget(QWidget):
    """Widget to display timeline of frames with time axis"""
    frame_changed = pyqtSignal(int)
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.total_frames = 0
        self.current_frame = 0
        self.start_times = {}  # frame_index -> start_time in seconds
        self.setMinimumHeight(100)
        self.setMaximumHeight(120)
        self.dragging = False
    
    def set_frames(self, total: int, current: int = 0, start_times: dict = None):
        """Set total number of frames, current frame, and start times"""
        self.total_frames = total
        self.current_frame = current
        self.start_times = start_times if start_times is not None else {}
        self.update()
    
    def set_current_frame(self, frame: int):
        """Update current frame position"""
        self.current_frame = frame
        self.update()
    
    def _get_event_x(self, event):
        """Get x coordinate from mouse event (PyQt5/PyQt6 compatible)"""
        if QT_VERSION == 6:
            return int(event.position().x())
        else:
            return event.x()
    
    def paintEvent(self, event):
        if QT_VERSION == 5:
            from PyQt5.QtGui import QPainter, QPen, QBrush
            from PyQt5.QtCore import QRect
        else:
            from PyQt6.QtGui import QPainter, QPen, QBrush
            from PyQt6.QtCore import QRect
        
        painter = QPainter(self)
        painter.setRenderHint(Antialiasing)
        
        rect = self.rect()
        left_margin = 50
        right_margin = 10
        top_margin = 20
        bottom_margin = 45  # More space for two axis labels
        plot_rect = QRect(left_margin, top_margin,
                         rect.width() - left_margin - right_margin,
                         rect.height() - top_margin - bottom_margin)
        
        if self.total_frames == 0:
            painter.drawText(rect, AlignCenter, "No frames")
            return
        
        # Draw timeline bar
        painter.fillRect(plot_rect, QColor(200, 200, 200))
        
        # Draw current frame indicator
        if self.total_frames > 0:
            frame_x = plot_rect.left() + int((self.current_frame / self.total_frames) * plot_rect.width())
            painter.fillRect(frame_x - 2, plot_rect.top(), 4, plot_rect.height(), QColor(255, 0, 0))
        
        # Draw axis labels
        painter.setPen(QPen(QColor(0, 0, 0)))
        font = painter.font()
        font.setPointSize(8)
        painter.setFont(font)
        
        # Top X-axis label (Frame Number)
        painter.drawText(plot_rect.left() + plot_rect.width() // 2 - 50,
                        top_margin - 5, "Frame Number")
        
        # Bottom X-axis label (Time)
        has_time_data = len(self.start_times) > 0
        if has_time_data:
            painter.drawText(plot_rect.left() + plot_rect.width() // 2 - 30,
                            rect.height() - 5, "Time (s)")
        
        # Frame number labels (top axis)
        if self.total_frames > 0:
            painter.drawText(plot_rect.left(), top_margin - 5, "0")
            painter.drawText(plot_rect.right() - 30, top_margin - 5, str(self.total_frames - 1))
            painter.drawText(frame_x - 15, top_margin - 5, str(self.current_frame))
        
        # Time labels (bottom axis)
        if has_time_data and self.total_frames > 0:
            # Get time range
            times = [self.start_times.get(i, 0.0) for i in range(self.total_frames)]
            if times:
                min_time = min(times)
                max_time = max(times)
                current_time = self.start_times.get(self.current_frame, 0.0)
                
                # Format time (use ms if < 1s, otherwise seconds)
                def format_time(t):
                    if abs(t) < 1.0:
                        return f"{t*1000:.1f} ms"
                    else:
                        return f"{t:.3f} s"
                
                painter.drawText(plot_rect.left(), rect.height() - 15, format_time(min_time))
                painter.drawText(plot_rect.right() - 60, rect.height() - 15, format_time(max_time))
                painter.drawText(frame_x - 30, rect.height() - 15, format_time(current_time))
        
        # Draw axis lines
        painter.setPen(QPen(QColor(0, 0, 0), 1))
        painter.drawLine(plot_rect.left(), plot_rect.top(), plot_rect.right(), plot_rect.top())  # Top axis
        painter.drawLine(plot_rect.left(), plot_rect.bottom(), plot_rect.right(), plot_rect.bottom())  # Bottom axis
    
    def mousePressEvent(self, event):
        if self.total_frames == 0:
            return
        
        left_margin = 50
        right_margin = 10
        top_margin = 20
        bottom_margin = 25
        plot_rect = self.rect()
        plot_rect.adjust(left_margin, top_margin, -right_margin, -bottom_margin)
        
        x = self._get_event_x(event) - plot_rect.left()
        if 0 <= x <= plot_rect.width():
            frame = int((x / plot_rect.width()) * self.total_frames)
            frame = np.clip(frame, 0, self.total_frames - 1)
            self.frame_changed.emit(frame)
            self.dragging = True
    
    def mouseMoveEvent(self, event):
        if self.dragging and self.total_frames > 0:
            left_margin = 50
            right_margin = 10
            top_margin = 20
            bottom_margin = 45  # More space for two axis labels
            plot_rect = self.rect()
            plot_rect.adjust(left_margin, top_margin, -right_margin, -bottom_margin)
            
            x = self._get_event_x(event) - plot_rect.left()
            x = np.clip(x, 0, plot_rect.width())
            frame = int((x / plot_rect.width()) * self.total_frames)
            frame = np.clip(frame, 0, self.total_frames - 1)
            self.frame_changed.emit(frame)
    
    def mouseReleaseEvent(self, event):
        self.dragging = False


class HistogramWidget(QWidget):
    """Widget to display histogram and allow range selection"""
    range_changed = pyqtSignal(float, float)
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.hist_data = None
        self.bins = None
        self.min_val = 0.0
        self.max_val = 1.0
        self.selected_min = 0.0
        self.selected_max = 1.0
        self.dragging_min = False
        self.dragging_max = False
        self.setMinimumHeight(100)  # Smaller histogram
        self.setMaximumHeight(120)  # Limit maximum height
        self.setMinimumWidth(400)
    
    def _get_event_x(self, event):
        """Get x coordinate from mouse event (PyQt5/PyQt6 compatible)"""
        if QT_VERSION == 6:
            return int(event.position().x())
        else:
            return event.x()
    
    def set_histogram(self, data: np.ndarray, bins: int = 256, preserve_range: bool = False):
        """Set histogram data
        
        Args:
            data: Image data array
            bins: Number of histogram bins
            preserve_range: If True, keep existing selected_min/max instead of resetting to data range
        """
        if data is None or data.size == 0:
            self.hist_data = None
            self.bins = None
            self.update()
            return
        
        # Compute histogram
        self.hist_data, self.bins = np.histogram(data, bins=bins)
        data_min = float(np.min(data))
        data_max = float(np.max(data))
        
        if preserve_range:
            # Keep existing range, but update min_val/max_val to data bounds
            # Clamp selected range to data bounds if needed
            self.min_val = data_min
            self.max_val = data_max
            self.selected_min = np.clip(self.selected_min, data_min, data_max)
            self.selected_max = np.clip(self.selected_max, data_min, data_max)
        else:
            # Reset range to data bounds
            self.min_val = data_min
            self.max_val = data_max
            self.selected_min = self.min_val
            self.selected_max = self.max_val
        
        self.update()
        self.range_changed.emit(self.selected_min, self.selected_max)
    
    def paintEvent(self, event):
        if QT_VERSION == 5:
            from PyQt5.QtGui import QPainter, QPen, QBrush
            from PyQt5.QtCore import QRect
        else:
            from PyQt6.QtGui import QPainter, QPen, QBrush
            from PyQt6.QtCore import QRect
        
        painter = QPainter(self)
        painter.setRenderHint(Antialiasing)
        
        rect = self.rect()
        # Reserve space for axis labels
        left_margin = 50
        right_margin = 10
        top_margin = 20
        bottom_margin = 30
        plot_rect = QRect(left_margin, top_margin, 
                         rect.width() - left_margin - right_margin, 
                         rect.height() - top_margin - bottom_margin)
        
        if self.hist_data is None or self.bins is None:
            painter.drawText(rect, AlignCenter, "No histogram data")
            return
        
        # Draw histogram bars
        max_count = float(np.max(self.hist_data))
        if max_count == 0:
            max_count = 1
        
        bin_width = plot_rect.width() / len(self.hist_data)
        for i, count in enumerate(self.hist_data):
            height = int((count / max_count) * plot_rect.height())
            x = plot_rect.left() + int(i * bin_width)
            painter.fillRect(x, plot_rect.bottom() - height, int(bin_width), height, QColor(100, 100, 100))
        
        # Draw selected range
        min_x = plot_rect.left() + int((self.selected_min - self.min_val) / (self.max_val - self.min_val) * plot_rect.width())
        max_x = plot_rect.left() + int((self.selected_max - self.min_val) / (self.max_val - self.min_val) * plot_rect.width())
        
        # Highlight selected range
        painter.fillRect(min_x, plot_rect.top(), max_x - min_x, plot_rect.height(), QColor(255, 255, 0, 50))
        
        # Draw min/max lines
        pen = QPen(QColor(255, 0, 0), 2)
        painter.setPen(pen)
        painter.drawLine(min_x, plot_rect.top(), min_x, plot_rect.bottom())
        painter.drawLine(max_x, plot_rect.top(), max_x, plot_rect.bottom())
        
        # Draw axis labels
        painter.setPen(QPen(QColor(0, 0, 0)))
        font = painter.font()
        font.setPointSize(8)
        painter.setFont(font)
        
        # X-axis label (bottom)
        painter.drawText(plot_rect.left() + plot_rect.width() // 2 - 60, 
                        rect.height() - 5, "Pixel Intensity")
        
        # Y-axis label (left, rotated)
        # Note: Qt doesn't easily support rotated text, so we'll just put it at the top-left
        painter.drawText(5, top_margin + 10, "Count")
        
        # X-axis tick labels
        painter.drawText(plot_rect.left(), rect.height() - 15, f"{self.min_val:.1f}")
        painter.drawText(plot_rect.right() - 40, rect.height() - 15, f"{self.max_val:.1f}")
        
        # Y-axis tick labels
        painter.drawText(5, plot_rect.bottom(), "0")
        painter.drawText(5, plot_rect.top() + 10, f"{int(max_count)}")
        
        # Selected range labels
        painter.setPen(QPen(QColor(255, 0, 0)))
        painter.drawText(min_x - 25, rect.height() - 15, f"{self.selected_min:.1f}")
        painter.drawText(max_x - 25, rect.height() - 15, f"{self.selected_max:.1f}")
        
        # Draw axis lines
        painter.setPen(QPen(QColor(0, 0, 0), 1))
        painter.drawLine(plot_rect.left(), plot_rect.bottom(), plot_rect.right(), plot_rect.bottom())  # X-axis
        painter.drawLine(plot_rect.left(), plot_rect.top(), plot_rect.left(), plot_rect.bottom())  # Y-axis
    
    def mousePressEvent(self, event):
        if self.hist_data is None:
            return
        
        margin = 40
        plot_rect = self.rect()
        plot_rect.adjust(margin, 10, -margin, -10)
        
        x = self._get_event_x(event) - plot_rect.left()
        if x < 0 or x > plot_rect.width():
            return
        
        # Convert x to value
        value = self.min_val + (x / plot_rect.width()) * (self.max_val - self.min_val)
        
        # Check if clicking near min or max line
        min_x = int((self.selected_min - self.min_val) / (self.max_val - self.min_val) * plot_rect.width())
        max_x = int((self.selected_max - self.min_val) / (self.max_val - self.min_val) * plot_rect.width())
        
        if abs(x - min_x) < 10:
            self.dragging_min = True
        elif abs(x - max_x) < 10:
            self.dragging_max = True
        else:
            # Click in middle - set both to same value and drag
            self.selected_min = value
            self.selected_max = value
            self.update()
            self.range_changed.emit(self.selected_min, self.selected_max)
    
    def mouseMoveEvent(self, event):
        if self.hist_data is None:
            return
        
        left_margin = 50
        right_margin = 10
        top_margin = 20
        bottom_margin = 30
        plot_rect = self.rect()
        plot_rect.adjust(left_margin, top_margin, -right_margin, -bottom_margin)
        
        x = self._get_event_x(event) - plot_rect.left()
        if x < 0:
            x = 0
        if x > plot_rect.width():
            x = plot_rect.width()
        
        value = self.min_val + (x / plot_rect.width()) * (self.max_val - self.min_val)
        
        if self.dragging_min:
            self.selected_min = np.clip(value, self.min_val, self.selected_max)
            self.update()
            self.range_changed.emit(self.selected_min, self.selected_max)
        elif self.dragging_max:
            self.selected_max = np.clip(value, self.selected_min, self.max_val)
            self.update()
            self.range_changed.emit(self.selected_min, self.selected_max)
    
    def mouseReleaseEvent(self, event):
        self.dragging_min = False
        self.dragging_max = False


class TIFFViewer(QMainWindow):
    def __init__(self):
        super().__init__()
        self.series: Optional[ImageSeries] = None
        self.current_index = 0
        self.current_image: Optional[np.ndarray] = None
        self.min_val = 0.0
        self.max_val = 1.0
        self.palette_name = "Grayscale"
        self.playing = False
        self.play_timer = QTimer()
        self.play_timer.timeout.connect(self.next_image)
        self.play_speed_ms = 100  # Default: 100 ms = 10 fps
        self.zoom_factor = 1.0
        self.image_pixmap = None  # Store original pixmap for zooming
        
        self.init_ui()
    
    def init_ui(self):
        self.setWindowTitle("Stream V2 TIFF Viewer")
        self.setGeometry(100, 100, 1200, 800)
        # Make window resizable
        self.setMinimumSize(800, 600)
        
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        layout = QHBoxLayout(central_widget)
        
        # Left panel - controls
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_panel.setMaximumWidth(300)
        
        # File selection
        file_group = QGroupBox("File Selection")
        file_layout = QVBoxLayout()
        self.open_series_btn = QPushButton("Open Series Directory")
        self.open_series_btn.clicked.connect(self.open_series)
        file_layout.addWidget(self.open_series_btn)
        self.open_image_btn = QPushButton("Open Single Image")
        self.open_image_btn.clicked.connect(self.open_single_image)
        file_layout.addWidget(self.open_image_btn)
        self.series_label = QLabel("No image loaded")
        file_layout.addWidget(self.series_label)
        file_group.setLayout(file_layout)
        left_layout.addWidget(file_group)
        
        # Navigation
        nav_group = QGroupBox("Navigation")
        nav_layout = QVBoxLayout()
        nav_buttons = QHBoxLayout()
        self.prev_btn = QPushButton("◀ Prev")
        self.prev_btn.clicked.connect(self.prev_image)
        self.play_btn = QPushButton("▶ Play")
        self.play_btn.clicked.connect(self.toggle_play)
        self.next_btn = QPushButton("Next ▶")
        self.next_btn.clicked.connect(self.next_image)
        nav_buttons.addWidget(self.prev_btn)
        nav_buttons.addWidget(self.play_btn)
        nav_buttons.addWidget(self.next_btn)
        nav_layout.addLayout(nav_buttons)
        
        self.index_spin = QSpinBox()
        self.index_spin.setMinimum(0)
        self.index_spin.setMaximum(0)
        self.index_spin.valueChanged.connect(self.goto_image)
        nav_layout.addWidget(QLabel("Image Index:"))
        nav_layout.addWidget(self.index_spin)
        
        # Playback speed control
        speed_layout = QHBoxLayout()
        speed_layout.addWidget(QLabel("Speed:"))
        self.speed_slider = QSlider()
        if QT_VERSION == 5:
            self.speed_slider.setOrientation(Qt.Horizontal)
        else:
            self.speed_slider.setOrientation(Qt.Orientation.Horizontal)
        self.speed_slider.setMinimum(10)  # 10 ms = 100 fps
        self.speed_slider.setMaximum(1000)  # 1000 ms = 1 fps
        self.speed_slider.setValue(100)  # Default: 100 ms = 10 fps
        self.speed_slider.valueChanged.connect(self.update_playback_speed)
        speed_layout.addWidget(self.speed_slider)
        self.speed_label = QLabel("10.0 fps")
        speed_layout.addWidget(self.speed_label)
        nav_layout.addLayout(speed_layout)
        
        nav_group.setLayout(nav_layout)
        left_layout.addWidget(nav_group)
        
        # Range controls
        range_group = QGroupBox("Dynamic Range")
        range_layout = QVBoxLayout()
        
        self.min_spin = QDoubleSpinBox()
        self.min_spin.setDecimals(2)
        self.min_spin.setRange(-1e10, 1e10)
        self.min_spin.valueChanged.connect(self.update_range)
        range_layout.addWidget(QLabel("Min Value:"))
        range_layout.addWidget(self.min_spin)
        
        self.max_spin = QDoubleSpinBox()
        self.max_spin.setDecimals(2)
        self.max_spin.setRange(-1e10, 1e10)
        self.max_spin.valueChanged.connect(self.update_range)
        range_layout.addWidget(QLabel("Max Value:"))
        range_layout.addWidget(self.max_spin)
        
        self.auto_range_btn = QPushButton("Auto Range")
        self.auto_range_btn.clicked.connect(self.auto_range)
        range_layout.addWidget(self.auto_range_btn)
        
        range_group.setLayout(range_layout)
        left_layout.addWidget(range_group)
        
        # Palette selection
        palette_group = QGroupBox("Color Palette")
        palette_layout = QVBoxLayout()
        self.palette_combo = QComboBox()
        self.palette_combo.addItems(list(ColorPalette.PALETTES.keys()))
        self.palette_combo.currentTextChanged.connect(self.change_palette)
        palette_layout.addWidget(self.palette_combo)
        palette_group.setLayout(palette_layout)
        left_layout.addWidget(palette_group)
        
        # Histogram (moved to left panel)
        hist_group = QGroupBox("Histogram (click/drag to set range)")
        hist_layout = QVBoxLayout()
        self.histogram = HistogramWidget()
        self.histogram.range_changed.connect(self.set_range_from_histogram)
        hist_layout.addWidget(self.histogram)
        hist_group.setLayout(hist_layout)
        left_layout.addWidget(hist_group)
        
        # Metadata display
        meta_group = QGroupBox("Metadata")
        meta_layout = QVBoxLayout()
        self.meta_label = QLabel("No image loaded")
        self.meta_label.setWordWrap(True)
        meta_layout.addWidget(self.meta_label)
        meta_group.setLayout(meta_layout)
        left_layout.addWidget(meta_group)
        
        left_layout.addStretch()
        layout.addWidget(left_panel)
        
        # Right panel - image and histogram
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        
        # Image display (moved to top)
        image_group = QGroupBox("Image")
        image_layout = QVBoxLayout()
        self.image_label = ZoomableLabel("No image loaded")
        self.image_label.setAlignment(AlignCenter)
        self.image_label.setMinimumSize(400, 300)
        self.image_label.setScaledContents(False)  # We'll handle scaling manually
        self.image_label.setStyleSheet("background-color: black;")
        self.image_label.zoom_requested.connect(self.handle_zoom)
        image_layout.addWidget(self.image_label)
        image_group.setLayout(image_layout)
        right_layout.addWidget(image_group, stretch=1)  # Image takes most space
        
        # Timeline widget
        timeline_group = QGroupBox("Timeline")
        timeline_layout = QVBoxLayout()
        self.timeline = TimelineWidget()
        self.timeline.frame_changed.connect(self.goto_image)
        timeline_layout.addWidget(self.timeline)
        timeline_group.setLayout(timeline_layout)
        right_layout.addWidget(timeline_group)  # Timeline at very bottom
        
        layout.addWidget(right_panel, stretch=1)
    
    def open_series(self):
        dir_path = QFileDialog.getExistingDirectory(self, "Open Series Directory", "/dev/shm")
        if dir_path:
            self.series = ImageSeries(dir_path)
            if len(self.series) == 0:
                QMessageBox.warning(self, "Error", "No TIFF files found in directory")
                return
            
            self.series_label.setText(f"Series {self.series.series_id}, Channel: {self.series.channel}\n{len(self.series)} images")
            self.index_spin.setMaximum(len(self.series) - 1)
            self.current_index = 0
            self.index_spin.setValue(0)
            # Update timeline with start times (cache them)
            start_times = {}
            for i in range(len(self.series)):
                start_time = self.series.get_start_time(i)
                if start_time is not None:
                    start_times[i] = start_time
            self._cached_start_times = start_times if start_times else {}
            self.timeline.set_frames(len(self.series), 0, start_times if start_times else None)
            self.load_image(0)
    
    def open_single_image(self):
        """Open a single TIFF image file"""
        file_path, _ = QFileDialog.getOpenFileName(
            self, "Open TIFF Image", "/dev/shm", "TIFF Files (*.tiff *.tif);;All Files (*)")
        if file_path:
            try:
                # Load single image
                img = Image.open(file_path)
                img_array = np.array(img, dtype=np.float32)
                
                # Create a temporary series-like structure for single image
                class SingleImageSeries:
                    def __init__(self, img_array, file_path):
                        self.image_files = [file_path]
                        self.series_id = None
                        self.channel = None
                        self.metadata = SeriesMetadata()
                        self._img_array = img_array
                    
                    def __len__(self):
                        return 1
                    
                    def get_image(self, index):
                        return self._img_array if index == 0 else None
                    
                    def get_image_id(self, index):
                        return 0
                    
                    def get_start_time(self, index):
                        return None
                
                self.series = SingleImageSeries(img_array, file_path)
                self.series_label.setText(f"Single Image\n{Path(file_path).name}")
                self.index_spin.setMaximum(0)
                self.current_index = 0
                self.index_spin.setValue(0)
                # Clear cached start times for single image
                self._cached_start_times = {}
                # Clear cached start times for single image
                self._cached_start_times = {}
                # Update timeline for single image
                self.timeline.set_frames(1, 0)
                self.load_image(0)
            except Exception as e:
                QMessageBox.critical(self, "Error", f"Failed to load image: {e}")
    
    def load_image(self, index: int):
        if self.series is None or index < 0 or index >= len(self.series):
            return
        
        self.current_index = index
        self.current_image = self.series.get_image(index)
        
        if self.current_image is None:
            return
        
        # Update metadata
        image_id = self.series.get_image_id(index)
        start_time = self.series.get_start_time(index)
        
        meta_text = f"Image ID: {image_id}\n"
        meta_text += f"Series ID: {self.series.series_id}\n"
        meta_text += f"Channel: {self.series.channel}\n"
        meta_text += f"Index: {index + 1} / {len(self.series)}\n"
        if start_time is not None:
            meta_text += f"Start Time: {start_time:.6f} s\n"
        meta_text += f"Shape: {self.current_image.shape}\n"
        meta_text += f"Data Type: {self.current_image.dtype}\n"
        meta_text += f"Min: {np.min(self.current_image):.2f}\n"
        meta_text += f"Max: {np.max(self.current_image):.2f}\n"
        self.meta_label.setText(meta_text)
        
        # Update histogram (preserve existing range when changing images)
        # Check if we have a valid range set (not just default values)
        preserve_range = (hasattr(self, 'min_val') and hasattr(self, 'max_val') and 
                         self.min_val != self.max_val and
                         not (self.min_val == 0.0 and self.max_val == 1.0))
        
        if preserve_range:
            # Sync the viewer's range to histogram before updating
            self.histogram.selected_min = self.min_val
            self.histogram.selected_max = self.max_val
        
        self.histogram.set_histogram(self.current_image, preserve_range=preserve_range)
        
        # Sync histogram range back to viewer
        if preserve_range:
            self.min_val = self.histogram.selected_min
            self.max_val = self.histogram.selected_max
            self.min_spin.setValue(self.min_val)
            self.max_spin.setValue(self.max_val)
        
        # Update timeline
        if self.series:
            self.timeline.set_current_frame(index)
            # Update timeline start times if available (only load once, cache it)
            if not hasattr(self, '_cached_start_times') or self._cached_start_times is None:
                # Load start times once and cache them
                if hasattr(self.series, 'metadata') and hasattr(self.series.metadata, 'start_times'):
                    start_times = {}
                    for i in range(len(self.series)):
                        start_time = self.series.get_start_time(i)
                        if start_time is not None:
                            start_times[i] = start_time
                    if start_times:
                        self._cached_start_times = start_times
                        self.timeline.start_times = start_times
                        self.timeline.update()
                    else:
                        self._cached_start_times = {}
                else:
                    self._cached_start_times = {}
            else:
                # Use cached start times
                self.timeline.start_times = self._cached_start_times
                self.timeline.update()
        
        # Only auto-range on first image load, not when navigating
        if not preserve_range:
            self.auto_range()
        else:
            # Update display with preserved range
            self.update_display()
    
    def auto_range(self):
        if self.current_image is None:
            return
        self.min_val = float(np.min(self.current_image))
        self.max_val = float(np.max(self.current_image))
        self.min_spin.setValue(self.min_val)
        self.max_spin.setValue(self.max_val)
        self.update_display()
    
    def update_range(self):
        self.min_val = self.min_spin.value()
        self.max_val = self.max_spin.value()
        self.update_display()
    
    def set_range_from_histogram(self, min_val: float, max_val: float):
        self.min_val = min_val
        self.max_val = max_val
        self.min_spin.setValue(min_val)
        self.max_spin.setValue(max_val)
        self.update_display()
    
    def change_palette(self, name: str):
        self.palette_name = name
        self.update_display()
    
    def update_display(self):
        if self.current_image is None:
            return
        
        # Normalize image to [0, 1] based on selected range
        range_size = self.max_val - self.min_val
        if range_size == 0:
            range_size = 1
        normalized = (self.current_image - self.min_val) / range_size
        normalized = np.clip(normalized, 0, 1)
        
        # Apply color palette
        palette_func = ColorPalette.PALETTES[self.palette_name]
        rgb_image = palette_func(normalized)
        
        # Get timestamp for overlay
        timestamp_text = None
        if self.series:
            start_time = self.series.get_start_time(self.current_index)
            if start_time is not None:
                if abs(start_time) < 1.0:
                    timestamp_text = f"t = {start_time*1000:.2f} ms"
                else:
                    timestamp_text = f"t = {start_time:.3f} s"
        
        # Convert to QImage
        h, w = rgb_image.shape[:2]
        # Ensure contiguous array for QImage
        rgb_image = np.ascontiguousarray(rgb_image)
        qimage = QImage(rgb_image.data, w, h, w * 3, Format_RGB888)
        pixmap = QPixmap.fromImage(qimage)
        
        # Draw timestamp overlay on pixmap
        if timestamp_text:
            if QT_VERSION == 5:
                from PyQt5.QtGui import QPainter, QFont
            else:
                from PyQt6.QtGui import QPainter, QFont
            painter = QPainter(pixmap)
            font = QFont()
            font.setPointSize(14)
            font.setBold(True)
            painter.setFont(font)
            # Draw with black outline for visibility, then yellow text
            painter.setPen(QColor(0, 0, 0))
            painter.drawText(11, 31, timestamp_text)  # Offset for outline
            painter.drawText(9, 31, timestamp_text)
            painter.drawText(10, 30, timestamp_text)
            painter.drawText(10, 32, timestamp_text)
            painter.setPen(QColor(255, 255, 0))  # Yellow text
            painter.drawText(10, 30, timestamp_text)
            painter.end()
        
        # Store original pixmap for zooming
        self.image_pixmap = pixmap
        
        # Apply zoom and scale to fit label while maintaining aspect ratio
        self._update_image_display()
    
    def resizeEvent(self, event):
        """Handle window resize - update image display"""
        super().resizeEvent(event)
        if self.image_pixmap is not None:
            # Only update the display scaling, not reprocess the image
            self._update_image_display()
    
    def prev_image(self):
        if self.series and self.current_index > 0:
            self.current_index -= 1
            self.index_spin.setValue(self.current_index)
            self.load_image(self.current_index)
    
    def next_image(self):
        if self.series and self.current_index < len(self.series) - 1:
            self.current_index += 1
            self.index_spin.setValue(self.current_index)
            self.load_image(self.current_index)
        else:
            self.toggle_play()  # Stop at end
    
    def goto_image(self, index: int):
        if self.series and 0 <= index < len(self.series):
            self.load_image(index)
    
    def toggle_play(self):
        self.playing = not self.playing
        if self.playing:
            self.play_btn.setText("⏸ Pause")
            self.play_timer.start(self.play_speed_ms)
        else:
            self.play_btn.setText("▶ Play")
            self.play_timer.stop()
    
    def update_playback_speed(self, value: int):
        """Update playback speed from slider"""
        self.play_speed_ms = value
        fps = 1000.0 / value
        self.speed_label.setText(f"{fps:.1f} fps")
        # Update timer if playing
        if self.playing:
            self.play_timer.setInterval(self.play_speed_ms)
    
    def handle_zoom(self, zoom_delta: float):
        """Handle zoom request from ZoomableLabel"""
        if self.image_pixmap is None:
            return
        
        # Update zoom factor
        self.zoom_factor = np.clip(self.zoom_factor + zoom_delta, 0.1, 10.0)
        
        # Update display
        self._update_image_display()
    
    def _update_image_display(self):
        """Update image display with current zoom"""
        if self.image_pixmap is None:
            return
        
        label_size = self.image_label.size()
        if label_size.width() <= 0 or label_size.height() <= 0:
            return
        
        # Calculate zoomed size
        original_size = self.image_pixmap.size()
        if QT_VERSION == 5:
            zoomed_width = int(original_size.width() * self.zoom_factor)
            zoomed_height = int(original_size.height() * self.zoom_factor)
        else:
            zoomed_width = int(original_size.width() * self.zoom_factor)
            zoomed_height = int(original_size.height() * self.zoom_factor)
        
        # Scale to fit label while maintaining aspect ratio
        if QT_VERSION == 5:
            scaled_pixmap = self.image_pixmap.scaled(zoomed_width, zoomed_height, 
                                                     KeepAspectRatio, SmoothTransformation)
        else:
            scaled_pixmap = self.image_pixmap.scaled(zoomed_width, zoomed_height,
                                                     KeepAspectRatio, SmoothTransformation)
        
        # If zoomed image is smaller than label, scale to fit
        if scaled_pixmap.width() < label_size.width() and scaled_pixmap.height() < label_size.height():
            # Scale to fit label
            scaled_pixmap = self.image_pixmap.scaled(label_size, KeepAspectRatio, SmoothTransformation)
        
        self.image_label.setPixmap(scaled_pixmap)


def main():
    app = QApplication(sys.argv)
    viewer = TIFFViewer()
    viewer.show()
    # PyQt6 uses exec(), PyQt5 uses exec_()
    if QT_VERSION == 6:
        sys.exit(app.exec())
    else:
        sys.exit(app.exec_())


if __name__ == "__main__":
    main()

