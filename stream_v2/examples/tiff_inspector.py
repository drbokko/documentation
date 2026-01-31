#!/usr/bin/env python3
"""
tiff_inspector.py - Inspect TIFF file metadata

Usage:
    python3 tiff_inspector.py <tiff_file>
    python3 tiff_inspector.py <tiff_file> --json  # Output as JSON
"""

import sys
import json
from pathlib import Path
from PIL import Image, TiffTags
from PIL.ExifTags import TAGS


def get_tiff_tag_name(tag_id):
    """Get human-readable name for TIFF tag ID"""
    # Try TiffTags first (Pillow 8.0+)
    if hasattr(TiffTags, 'TAGS'):
        if tag_id in TiffTags.TAGS:
            return TiffTags.TAGS[tag_id]
    
    # Try Exif tags
    if tag_id in TAGS:
        return TAGS[tag_id]
    
    # Return as unknown tag
    return f"Unknown Tag {tag_id}"


def format_tag_value(value):
    """Format tag value for display"""
    if isinstance(value, (list, tuple)):
        if len(value) == 1:
            return value[0]
        elif len(value) <= 10:
            return list(value)
        else:
            return f"<list of {len(value)} items: {list(value[:5])}...>"
    elif isinstance(value, bytes):
        try:
            # Try to decode as UTF-8 string
            decoded = value.decode('utf-8', errors='replace')
            if len(decoded) > 200:
                return f"<bytes: {decoded[:200]}...>"
            return decoded
        except:
            if len(value) > 100:
                return f"<bytes: {len(value)} bytes>"
            return f"<bytes: {value.hex()}>"
    elif isinstance(value, str):
        if len(value) > 200:
            return f"{value[:200]}..."
        return value
    else:
        return value


def inspect_tiff(file_path, output_json=False):
    """Inspect and print TIFF metadata"""
    file_path = Path(file_path)
    
    if not file_path.exists():
        print(f"Error: File not found: {file_path}", file=sys.stderr)
        return 1
    
    try:
        img = Image.open(file_path)
    except Exception as e:
        print(f"Error: Could not open TIFF file: {e}", file=sys.stderr)
        return 1
    
    # Basic image info
    basic_info = {
        "filename": str(file_path),
        "format": img.format,
        "mode": img.mode,
        "size": img.size,
        "width": img.width,
        "height": img.height,
    }
    
    # Get all metadata tags
    all_tags = {}
    
    # Try tag_v2 (Pillow 8.0+)
    if hasattr(img, 'tag_v2'):
        for tag_id, value in img.tag_v2.items():
            all_tags[tag_id] = value
    
    # Try tag (older Pillow)
    if hasattr(img, 'tag') and not all_tags:
        for tag_id, value in img.tag.items():
            all_tags[tag_id] = value
    
    # Try getexif() (PIL 8.0+)
    if hasattr(img, 'getexif'):
        try:
            exif = img.getexif()
            if exif:
                for tag_id, value in exif.items():
                    all_tags[tag_id] = value
        except Exception as e:
            pass
    
    # Try _getexif() (deprecated)
    if hasattr(img, '_getexif'):
        try:
            exif = img._getexif()
            if exif:
                for tag_id, value in exif.items():
                    if tag_id not in all_tags:
                        all_tags[tag_id] = value
        except Exception:
            pass
    
    # Close image
    img.close()
    
    # Prepare output
    if output_json:
        output = {
            "basic_info": basic_info,
            "tags": {}
        }
        for tag_id, value in sorted(all_tags.items()):
            tag_name = get_tiff_tag_name(tag_id)
            output["tags"][tag_id] = {
                "name": tag_name,
                "value": format_tag_value(value)
            }
        print(json.dumps(output, indent=2, default=str))
    else:
        # Human-readable output
        print("=" * 80)
        print(f"TIFF File: {file_path}")
        print("=" * 80)
        print("\nBasic Information:")
        print("-" * 80)
        for key, value in basic_info.items():
            print(f"  {key:20s}: {value}")
        
        if all_tags:
            print("\nTIFF Tags:")
            print("-" * 80)
            for tag_id in sorted(all_tags.keys()):
                tag_name = get_tiff_tag_name(tag_id)
                value = format_tag_value(all_tags[tag_id])
                print(f"  Tag {tag_id:5d} ({tag_name:30s}): {value}")
        else:
            print("\nNo TIFF tags found.")
        
        print("=" * 80)
    
    return 0


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tiff_inspector.py <tiff_file> [--json]", file=sys.stderr)
        sys.exit(1)
    
    file_path = sys.argv[1]
    output_json = "--json" in sys.argv
    
    sys.exit(inspect_tiff(file_path, output_json))


if __name__ == "__main__":
    main()

