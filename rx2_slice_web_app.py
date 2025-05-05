import struct

def _parse_chunks(buf, offset=0, parent_len=None):
    """
    Recursively parse REX2 chunks for SLCE entries.
    Returns a list of (slice_offset, slice_length) and bytes consumed.
    """
    slices = []
    start = offset
    total_len = parent_len or len(buf)

    while offset - start < total_len:
        # Read chunk header
        chunk_type = buf[offset:offset+4].decode('ascii', errors='ignore')
        length = struct.unpack('>I', buf[offset+4:offset+8])[0]
        pad = length + (length % 2)
        data_offset = offset + 8

        if chunk_type == 'CAT ':
            # Dive into container
            child_slices, used = _parse_chunks(buf, data_offset, pad)
            slices.extend(child_slices)
        elif chunk_type == 'SLCE':
            # First 4 bytes: slice start (in samples)
            # Next 4 bytes: slice length (in samples)
            slice_start = struct.unpack('>I', buf[data_offset:data_offset+4])[0]
            slice_len   = struct.unpack('>I', buf[data_offset+4:data_offset+8])[0]
            slices.append((slice_start, slice_len))

        # Move to next chunk
        offset += 8 + pad

    return slices, offset - start


def get_slices(rx2_path):
    """
    Read an .rx2 file and return a list of slice (offset, length) tuples.
    """
    with open(rx2_path, 'rb') as f:
        buf = f.read()
    slices, _ = _parse_chunks(buf)
    return slices