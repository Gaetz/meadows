// The shared slice math of the froxel-fog grid and the light-cluster grid
// (docs/LIGHTING.md §5): both slice CAMERA DISTANCE on the same
// exponential mapping (kSliceNear -> far), so a froxel finds its cluster
// by xy downsample with an IDENTICAL z slice. Included by the froxel
// passes, cluster_cull.comp and locallights.glsl — change the mapping in
// lockstep or the grids shear apart.

const ivec3 kClusterDims = ivec3(16, 9, 64);
// Per-cell list capacity. Sized ABOVE the worst overlapping set: an
// interior hall stacks ~18 light spheres over its central cells — at 16
// the farthest-from-camera lights (the far windows) fell off the list.
// On overflow the nearest-to-camera win (UBO order).
const int kClusterSlots = 32; // slot 0 = light count, 1..31 = indices
const float kSliceNear = 1.0;

// Slice coordinate s in [0, slices] -> view distance (dense near camera).
float sliceDepth(float s, float slices, float far) {
    return kSliceNear * pow(far / kSliceNear, s / slices);
}

// Inverse of sliceDepth: view distance -> continuous slice coordinate.
float sliceCoord(float dist, float slices, float far) {
    return slices * log(max(dist, kSliceNear) / kSliceNear) /
           log(far / kSliceNear);
}

int clusterIndex(ivec3 cell) {
    return (cell.z * kClusterDims.y + cell.y) * kClusterDims.x + cell.x;
}
