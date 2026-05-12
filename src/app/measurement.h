#ifndef IDIFF_MEASUREMENT_H
#define IDIFF_MEASUREMENT_H

namespace idiff {

// A saved rectangular measurement anchored in the source image's native
// pixel coordinates, so labels keep reporting the image-resolution size
// regardless of subsequent zoom, pan, or viewport resize.  The source
// cell/tex dimensions captured at mouse-down are frozen for the lifetime
// of the measurement; the rectangle can extend outside [0..tex_w/h) when
// the user drags past the cell edge.
struct Measurement {
    int   id;                  // monotonically assigned, never reused
    int   source_cell_index;   // cell index the mouse-down point fell in
    int   src_tex_w;           // source image native width, pixels
    int   src_tex_h;           // source image native height, pixels
    float x0, y0, x1, y1;      // rectangle in source image pixel coords
};

} // namespace idiff

#endif // IDIFF_MEASUREMENT_H
